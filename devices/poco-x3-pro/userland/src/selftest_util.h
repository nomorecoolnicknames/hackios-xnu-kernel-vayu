/*
 * selftest_util.h -- the three pieces of the self-test that are plain C, so that they can be
 * compiled and exercised on the build machine as well as on the phone:
 *
 *   st_boot_arg_is    a strict NAME=VALUE token in the kernel's command line (kern.bootargs)
 *   st_atoms          the top-level atoms of a QuickTime/MP4 file (ftyp, moov, mdat ...)
 *   st_image_kind     what a still picture is, by its first bytes
 *   st_wave_analyze   peak, RMS, distinct values and "all samples equal" from a PCM WAV file
 *
 * The phone side is devices/poco-x3-pro/userland/src/selftest.m and selftest_camera_app.m; the
 * host test is /dev/shm/lead-tasks/Q-patches/host-tests/selftest_util_test.c (not in the tree:
 * it is a build-machine test, and the tree has no host build for it).
 *
 * No allocation, no Foundation, no I/O beyond read/pread/open: a job that runs at boot must not
 * depend on more than it has to.
 */
#ifndef SELFTEST_UTIL_H
#define SELFTEST_UTIL_H

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- the kernel's command line -------------------------------------------------------------- */

/*
 * Is NAME=VALUE one of the blank-separated tokens of ARGS? The whole token must match: "selftest=1"
 * is found in "rd=md0 selftest=1 -v" and not in "selftest=10", "xselftest=1" or "selftest=".
 * (The same shape as userland/src/var_store.c:boot_arg_is and userland/src/init.c:boot_arg.)
 */
static inline int
st_boot_arg_is(const char *args, const char *name, const char *value)
{
	size_t name_length, value_length;

	if (args == NULL || name == NULL || value == NULL) {
		return 0;
	}
	name_length = strlen(name);
	value_length = strlen(value);
	while (*args != '\0') {
		const char *token;
		size_t length;

		while (*args == ' ' || *args == '\t' || *args == '\n') {
			args++;
		}
		token = args;
		while (*args != '\0' && *args != ' ' && *args != '\t' && *args != '\n') {
			args++;
		}
		length = (size_t)(args - token);
		if (length == name_length + 1 + value_length &&
		    memcmp(token, name, name_length) == 0 &&
		    token[name_length] == '=' &&
		    memcmp(token + name_length + 1, value, value_length) == 0) {
			return 1;
		}
	}
	return 0;
}

/* ---- a QuickTime/MP4 file's top-level atoms ------------------------------------------------ */

static inline uint32_t
st_be32(const unsigned char *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline uint64_t
st_be64(const unsigned char *p)
{
	return ((uint64_t)st_be32(p) << 32) | (uint64_t)st_be32(p + 4);
}

/* RIFF/WAVE is little-endian, unlike QuickTime/ISO BMFF: the same four bytes mean another size. */
static inline uint32_t
st_le32(const unsigned char *p)
{
	return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[0];
}

static inline int
st_fourcc_printable(const unsigned char *p)
{
	for (int i = 0; i < 4; i++) {
		if (p[i] < 0x20 || p[i] > 0x7e) {
			return 0;
		}
	}
	return 1;
}

/*
 * Walk the top-level atoms of PATH. SUMMARY (if not NULL) receives the four-character types in
 * order, comma-separated and bounded; HAS_MOOV is 1 when one of them is "moov"; SIZE receives the
 * file's size. Returns 0 when the file is a well-formed atom list, -1 with WHY filled otherwise.
 *
 * Only the top level is read: "moov present" is exactly the question a 5 s recording has to answer,
 * and a recording killed before its writer closed leaves mdat with no moov. Sizes 1 (64-bit) and 0
 * (extends to the end) are handled; a size smaller than the header, or one that runs past the file,
 * is a malformed file and says so.
 */
static inline int
st_atoms(const char *path, char *summary, size_t summary_size, int *has_moov, long long *size,
    char *why, size_t why_size)
{
	unsigned char header[16];
	struct stat status;
	long long offset = 0;
	unsigned atoms = 0;
	size_t used = 0;
	int fd;

	if (summary != NULL && summary_size > 0) {
		summary[0] = '\0';
	}
	if (has_moov != NULL) {
		*has_moov = 0;
	}
	if (size != NULL) {
		*size = 0;
	}
	if (why != NULL && why_size > 0) {
		why[0] = '\0';
	}
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		if (why != NULL) {
			snprintf(why, why_size, "%s: %s", path, strerror(errno));
		}
		return -1;
	}
	if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode)) {
		if (why != NULL) {
			snprintf(why, why_size, "%s: not a regular file", path);
		}
		close(fd);
		return -1;
	}
	if (size != NULL) {
		*size = (long long)status.st_size;
	}
	if (status.st_size == 0) {
		if (why != NULL) {
			snprintf(why, why_size, "%s: empty", path);
		}
		close(fd);
		return -1;
	}
	while (offset < (long long)status.st_size && atoms < 64) {
		uint64_t atom_size;
		ssize_t got;

		if ((long long)status.st_size - offset < 8) {
			if (why != NULL) {
				snprintf(why, why_size, "%s: %lld trailing byte(s) where an atom header was due",
				    path, (long long)status.st_size - offset);
			}
			close(fd);
			return -1;
		}
		got = pread(fd, header, 8, (off_t)offset);
		if (got != 8) {
			if (why != NULL) {
				snprintf(why, why_size, "%s: short read at %lld: %s", path, offset,
				    got < 0 ? strerror(errno) : "end of file");
			}
			close(fd);
			return -1;
		}
		atom_size = st_be32(header);
		if (atom_size == 1) {
			if ((long long)status.st_size - offset < 16 ||
			    pread(fd, header + 8, 8, (off_t)offset + 8) != 8) {
				if (why != NULL) {
					snprintf(why, why_size, "%s: a 64-bit atom at %lld has no size", path, offset);
				}
				close(fd);
				return -1;
			}
			atom_size = st_be64(header + 8);
			if (atom_size < 16) {
				if (why != NULL) {
					snprintf(why, why_size, "%s: atom at %lld has size %llu", path, offset,
					    (unsigned long long)atom_size);
				}
				close(fd);
				return -1;
			}
		} else if (atom_size == 0) {
			atom_size = (uint64_t)((long long)status.st_size - offset);
		} else if (atom_size < 8) {
			if (why != NULL) {
				snprintf(why, why_size, "%s: atom at %lld has size %llu", path, offset,
				    (unsigned long long)atom_size);
			}
			close(fd);
			return -1;
		}
		if (atom_size > (uint64_t)((long long)status.st_size - offset)) {
			if (why != NULL) {
				snprintf(why, why_size, "%s: atom '%.4s' at %lld runs past the end (%llu > %lld)",
				    path, header + 4, offset, (unsigned long long)atom_size,
				    (long long)status.st_size - offset);
			}
			close(fd);
			return -1;
		}
		if (st_fourcc_printable(header + 4)) {
			if (summary != NULL && used + 5 < summary_size) {
				used += (size_t)snprintf(summary + used, summary_size - used, "%s%.4s",
				    used > 0 ? "," : "", header + 4);
			}
			if (memcmp(header + 4, "moov", 4) == 0 && has_moov != NULL) {
				*has_moov = 1;
			}
		} else if (summary != NULL && used + 9 < summary_size) {
			used += (size_t)snprintf(summary + used, summary_size - used, "%s0x%08x",
			    used > 0 ? "," : "", st_be32(header + 4));
		}
		offset += (long long)atom_size;
		atoms++;
	}
	close(fd);
	if (atoms == 0) {
		if (why != NULL) {
			snprintf(why, why_size, "%s: no atoms", path);
		}
		return -1;
	}
	return 0;
}

/* ---- a still picture, by its first bytes --------------------------------------------------- */

/*
 * "jpeg", "heic", "png", "unknown" or "missing"; WHY (if not NULL) says what was seen. A HEIC file
 * is an ISO BMFF file whose major brand is one of the HEIF ones; the camera stack may hand back
 * either a JPEG or a HEIC picture depending on the settings it accepts.
 */
static inline const char *
st_image_kind(const char *path, char *why, size_t why_size)
{
	unsigned char head[16];
	ssize_t got;
	int fd;

	if (why != NULL && why_size > 0) {
		why[0] = '\0';
	}
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		if (why != NULL) {
			snprintf(why, why_size, "%s: %s", path, strerror(errno));
		}
		return "missing";
	}
	got = read(fd, head, sizeof(head));
	close(fd);
	if (got < 4) {
		if (why != NULL) {
			snprintf(why, why_size, "%s: %zd bytes", path, got);
		}
		return "unknown";
	}
	if (got >= 3 && head[0] == 0xff && head[1] == 0xd8 && head[2] == 0xff) {
		return "jpeg";
	}
	if (got >= 12 && memcmp(head + 4, "ftyp", 4) == 0) {
		static const char *brands[] = { "heic", "heix", "hevc", "hevx", "heim", "heis", "mif1", "msf1" };

		for (unsigned i = 0; i < sizeof(brands) / sizeof(brands[0]); i++) {
			if (memcmp(head + 8, brands[i], 4) == 0) {
				return "heic";
			}
		}
		if (why != NULL) {
			snprintf(why, why_size, "%s: ISO BMFF brand '%.4s'", path, head + 8);
		}
		return "unknown";
	}
	if (got >= 8 && head[0] == 0x89 && head[1] == 'P' && head[2] == 'N' && head[3] == 'G') {
		return "png";
	}
	if (why != NULL) {
		snprintf(why, why_size, "%s: first bytes %02x %02x %02x %02x", path, head[0], head[1], head[2],
		    head[3]);
	}
	return "unknown";
}

/* ---- a PCM WAV file ------------------------------------------------------------------------ */

struct st_wave {
	int             ok;             /* the file parsed and holds samples */
	char            why[160];       /* what was wrong, when it was */
	unsigned        channels;
	unsigned        rate;
	unsigned        bits;
	unsigned long long frames;      /* frames, not samples */
	unsigned long long samples;     /* frames * channels, the denominator of the RMS */
	int             peak;           /* the largest |sample|, 0..32768 */
	double          rms;            /* root mean square of the samples */
	unsigned long long distinct;    /* how many of the 65536 possible values occur */
	unsigned long long nonzero;
	unsigned long long crossings;   /* sign changes in the first channel */
	int             all_equal;      /* min == max: a stuck converter, not a quiet room */
	int             min, max;
};

static inline int
st_read_exact(int fd, void *buffer, size_t size, off_t offset)
{
	unsigned char *p = buffer;
	size_t done = 0;

	while (done < size) {
		ssize_t got = pread(fd, p + done, size - done, offset + (off_t)done);

		if (got < 0 && errno == EINTR) {
			continue;
		}
		if (got <= 0) {
			return -1;
		}
		done += (size_t)got;
	}
	return 0;
}

/*
 * Analyze a RIFF/WAVE file of 16-bit PCM (the format AVAudioRecorder is asked for by selftest.m).
 * Frames are counted from the data chunk; peak and RMS are over every sample; DISTINCT counts the
 * values in a 64 KB bitmap, which is how "all the samples are the same" is told apart from "the
 * room is quiet": a stuck path has one value, a quiet one has many small ones.
 */
static inline void
st_wave_analyze(const char *path, struct st_wave *out)
{
	unsigned char riff[12], chunk[8];
	static unsigned char seen[65536 / 8];   /* one bit per 16-bit value */
	unsigned long long sum_squares = 0;
	off_t offset = 12;
	struct stat status;
	int fd;

	memset(out, 0, sizeof(*out));
	memset(seen, 0, sizeof(seen));
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		snprintf(out->why, sizeof(out->why), "%s: %s", path, strerror(errno));
		return;
	}
	if (fstat(fd, &status) != 0 || status.st_size < 44) {
		snprintf(out->why, sizeof(out->why), "%s: too short for a WAV file", path);
		close(fd);
		return;
	}
	if (st_read_exact(fd, riff, 12, 0) != 0 || memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
		snprintf(out->why, sizeof(out->why), "%s: not RIFF/WAVE", path);
		close(fd);
		return;
	}
	out->bits = 0;
	while (offset + 8 <= (off_t)status.st_size) {
		uint32_t size;

		if (st_read_exact(fd, chunk, 8, offset) != 0) {
			break;
		}
		size = st_le32(chunk + 4);
		if (memcmp(chunk, "fmt ", 4) == 0 && size >= 16 && offset + 8 + (off_t)size <= (off_t)status.st_size) {
			unsigned char format[40];

			if (st_read_exact(fd, format, 16, offset + 8) == 0) {
				unsigned tag = (unsigned)format[0] | ((unsigned)format[1] << 8);

				out->channels = (unsigned)format[2] | ((unsigned)format[3] << 8);
				out->rate = (unsigned)format[4] | ((unsigned)format[5] << 8) |
				    ((unsigned)format[6] << 16) | ((unsigned)format[7] << 24);
				out->bits = (unsigned)format[14] | ((unsigned)format[15] << 8);
				if (tag != 1) {
					snprintf(out->why, sizeof(out->why), "%s: format tag %u, not PCM (1)", path, tag);
					close(fd);
					return;
				}
			}
		} else if (memcmp(chunk, "data", 4) == 0) {
			off_t data = offset + 8;
			off_t end = data + (off_t)size;

			if (end > (off_t)status.st_size) {
				end = (off_t)status.st_size;
			}
			if (out->bits != 16 || out->channels == 0) {
				snprintf(out->why, sizeof(out->why), "%s: no fmt chunk before data", path);
				close(fd);
				return;
			}
			{
				unsigned char buffer[16384];
				unsigned long long samples = 0;
				int previous = 0, have_previous = 0;
				int min = 32767, max = -32768;

				while (data < end) {
					size_t want = (size_t)(end - data);
					ssize_t got;
					size_t i;

					if (want > sizeof(buffer)) {
						want = sizeof(buffer);
					}
					got = pread(fd, buffer, want, data);
					if (got < 0 && errno == EINTR) {
						continue;
					}
					if (got <= 0) {
						break;
					}
					for (i = 0; i + 1 < (size_t)got; i += 2) {
						int value = (int)(int16_t)((unsigned)buffer[i] | ((unsigned)buffer[i + 1] << 8));
						int magnitude = value < 0 ? -value : value;
						unsigned bit = (unsigned)(uint16_t)value;

						if (magnitude > out->peak) {
							out->peak = magnitude;
						}
						if (value < min) {
							min = value;
						}
						if (value > max) {
							max = value;
						}
						sum_squares += (unsigned long long)((int64_t)value * value);
						if (seen[bit >> 3] & (unsigned char)(1u << (bit & 7))) {
							/* already counted */
						} else {
							seen[bit >> 3] |= (unsigned char)(1u << (bit & 7));
							out->distinct++;
						}
						if (value != 0) {
							out->nonzero++;
						}
						if ((samples % out->channels) == 0) {
							if (have_previous && ((previous < 0 && value >= 0) ||
							    (previous >= 0 && value < 0))) {
								out->crossings++;
							}
							previous = value;
							have_previous = 1;
						}
						samples++;
					}
					data += got;
				}
				out->samples = samples;
				out->frames = samples / out->channels;
				out->min = min;
				out->max = max;
				out->all_equal = min == max;
				out->rms = samples > 0 ? sqrt((double)sum_squares / (double)samples) : 0.0;
				if (samples == 0) {
					snprintf(out->why, sizeof(out->why), "%s: the data chunk held no samples", path);
					close(fd);
					return;
				}
				out->ok = 1;
				close(fd);
				return;
			}
		}
		offset += 8 + (off_t)size + (size & 1);   /* chunks are word aligned */
	}
	snprintf(out->why, sizeof(out->why), "%s: no data chunk", path);
	close(fd);
}

/* A human line for the log: 48000 Hz, 1 channel, 16 bits, 3.0 s, peak 1234/32768 (-28.5 dBFS) ... */
static inline void
st_wave_describe(const struct st_wave *wave, char *out, size_t size)
{
	double peak_dbfs = wave->peak > 0 ? 20.0 * log10((double)wave->peak / 32768.0) : -1.0e9;
	double rms_dbfs = wave->rms > 0 ? 20.0 * log10(wave->rms / 32768.0) : -1.0e9;

	snprintf(out, size,
	    "peak %d/32768 (%.1f dBFS), rms %.1f (%.1f dBFS), samples %llu, distinct %llu, "
	    "zero crossings %llu, min %d, max %d, all equal %s",
	    wave->peak, peak_dbfs, wave->rms, rms_dbfs, wave->samples, wave->distinct, wave->crossings,
	    wave->min, wave->max, wave->all_equal ? "YES" : "no");
}

#endif /* SELFTEST_UTIL_H */
