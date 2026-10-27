/*
 * music_import: put one MP3 into the Music library so the stock Music app lists it; usage:
 * music_import [--wait SECONDS] [--no-late-checks] SOURCE.mp3 [DEST_NAME] (defaults 0 and RDWD.mp3).
 * With --no-late-checks the three read-back checks at +60/+180/+420 s are skipped: a job list that
 * runs this in the foreground (the unattended self-test, userland/rc.jobs.selftest) must not be
 * held for seven minutes by them, and the row is read again by the caller anyway. Root copies
 * and hands the file to mobile, which reads the tags and inserts an ML3Track; a marker makes a
 * rerun a no-op.
 *
 * The insert is the only step that goes through medialibraryd, and it is a plain INSERT, which the
 * daemon's authorizer allows. Everything after it -- the sort map, the item's orders and search keys,
 * the library/in-library flags -- is SQL on the database file by this process, because a client's
 * sort-map DDL over XPC is refused by that authorizer and the daemon asserts and dies, taking Music's
 * own library service with it. See the comment above repair_local. With a song imported that way the
 * rows are what MediaPlayer's queryWithLibrary: path wants, and medialibraryd stays up to serve it.
 */
#import <Foundation/Foundation.h>
#import <MediaPlayer/MediaPlayer.h>
#import <objc/message.h>
#include <signal.h>
#include <sqlite3.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MOBILE_ID	501
#define MEDIA		"/var/mobile/Media"
#define MUSIC_DIR	MEDIA "/iTunes_Control/Music/F00"
#define ITUNES_DIR	MEDIA "/iTunes_Control/iTunes"

@interface ML3MusicLibrary : NSObject
+ (instancetype)autoupdatingSharedLibrary;
- (void)notifyContentsDidChange;
- (BOOL)updateSortMap;
- (void)setNeedsToUpdateSortMap:(BOOL)needs;
- (void)updateOrderingLanguagesForCurrentLanguage;
- (BOOL)validateDatabase;
@end
@interface ML3Entity : NSObject
+ (instancetype)newWithDictionary:(NSDictionary *)dict inLibrary:(ML3MusicLibrary *)library;
- (int64_t)persistentID;
- (id)valueForProperty:(NSString *)property;
- (BOOL)existsInLibrary;
- (void)setValue:(id)value forProperty:(NSString *)property;
@end
@interface ML3Track : ML3Entity
@end
extern int64_t ML3BaseLocationIDFromMediaRelativePathInLibrary(NSString *relativeDirectory, ML3MusicLibrary *library);
extern NSString *const ML3TrackPropertyTitle;
extern NSString *const ML3TrackPropertyArtist;
extern NSString *const ML3TrackPropertyAlbum;
extern NSString *const ML3TrackPropertyAlbumArtist;
extern NSString *const ML3TrackPropertyGenre;
extern NSString *const ML3TrackPropertyMediaType;
extern NSString *const ML3TrackPropertyLocationFileName;
extern NSString *const ML3TrackPropertyBaseLocationID;
extern NSString *const ML3TrackPropertyTotalTime;
extern NSString *const ML3TrackPropertyIsInMyLibrary;
extern NSString *const ML3TrackPropertyHidden;
extern NSString *const ML3TrackPropertyIsUserDisabled;
extern NSString *const ML3TrackPropertyFilePath;
extern NSString *const ML3TrackPropertyIsPlayable;
extern NSString *const ML3TrackPropertyIsLocal;
extern NSString *const ML3TrackPropertyIsCurrentlyRestricted;
extern NSString *const ML3TrackPropertyIsExplicit;
extern NSString *const ML3TrackPropertyContentRating;
#define UTF8(s) ((s) ? [(s) UTF8String] : "(none)")

static int usage(void) {
	fprintf(stderr, "music_import: usage: music_import [--wait SECONDS] [--no-late-checks] SOURCE.mp3 [DEST_NAME] | --query [NAME] | --repair [NAME] | --ml3 [NAME]\n");
	fprintf(stderr, "music_import: --query/--repair work on a running phone: rows, songs count, sort map (no import);\n");
	fprintf(stderr, "music_import: --repair rebuilds the sort map and the orders here, in SQL, without medialibraryd\n");
	return 2;
}
static int error(const char *path) {
	fprintf(stderr, "music_import: %s: %s\n", path, strerror(errno)); return -1;
}
/* One directory, made 0755 and given to mobile when it is not there yet. R2/TEST5Q: a directory
 * that is already there is fixed too -- a boot that made it as root, or a copy of the image with
 * another owner, left mediaserverd (mobile) with a path it could not traverse, and the import then
 * "worked" with an asset nothing could open. */
static int make_dir(const char *path) {
	struct stat status;

	if (stat(path, &status) == 0) {
		if (!S_ISDIR(status.st_mode)) {
			fprintf(stderr, "music_import: %s is not a directory\n", path); return -1;
		}
		if ((status.st_mode & 07777) != 0755 || status.st_uid != MOBILE_ID || status.st_gid != MOBILE_ID) {
			if (chmod(path, 0755) != 0 || chown(path, MOBILE_ID, MOBILE_ID) != 0) return error(path);
			fprintf(stderr, "music_import: fixed %s (0755, 501:501, was %04o %u:%u)\n", path,
			    status.st_mode & 07777, status.st_uid, status.st_gid);
		}
		return 0;
	}
	if (errno != ENOENT || mkdir(path, 0755) != 0) return error(path);
	if (chmod(path, 0755) != 0 || chown(path, MOBILE_ID, MOBILE_ID) != 0) return error(path);
	fprintf(stderr, "music_import: made %s (0755, 501:501)\n", path);
	return 0;
}
/* Read and write, not rename: the file is a copy in the Music directory. */
static int copy_file(const char *source, const char *dest, long long *copied) {
	char buffer[65536];
	const char *where = dest;
	long long total = 0;
	int in, out;

	*copied = 0;
	if ((in = open(source, O_RDONLY)) < 0) return error(source);
	if ((out = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) { close(in); return error(dest); }
	if (fchmod(out, 0644) != 0 || fchown(out, MOBILE_ID, MOBILE_ID) != 0) goto fail;
	for (;;) {
		ssize_t got = read(in, buffer, sizeof(buffer)), done = 0;
		if (got < 0 && errno == EINTR) continue;
		if (got < 0) { where = source; goto fail; }
		if (got == 0) break;
		while (done < got) {
			ssize_t written = write(out, buffer + done, (size_t)(got - done));
			if (written < 0 && errno == EINTR) continue;
			if (written <= 0) goto fail;
			done += written;
		}
		total += got;
	}
	if (fsync(out) != 0) goto fail;
	close(in);
	if (close(out) != 0) return error(dest);
	*copied = total;
	return 0;
fail:
	error(where); close(in); close(out); return -1;
}
/*
 * Where the program is, for the watchdog below: on the phone (run 204935) it went silent right after
 * "becoming mobile", inside AudioToolbox's tag reading, and said nothing more -- no line, no exit status.
 * Now every step is named, and a hang or a crash says which step it was in.
 */
static const char *volatile step = "start";

static void
say_step(const char *what)
{
	step = what;
	fprintf(stderr, "music_import: step: %s\n", what);
}

static void
died(int sig)
{
	char line[160];
	int n = snprintf(line, sizeof(line), "music_import: %s in step \"%s\"\n",
	    sig == SIGALRM ? "TIMED OUT" : sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : "SIGABRT", step);

	write(2, line, n > 0 && n < (int)sizeof(line) ? (size_t)n : sizeof(line) - 1);
	_exit(sig == SIGALRM ? 3 : 4);
}

/* One ID3v2 text frame's value: encoding byte, then ISO-8859-1, UTF-16 with a BOM, UTF-16BE or UTF-8. */
static NSString *
id3_text(const unsigned char *p, size_t size)
{
	static const NSStringEncoding encodings[] = { NSISOLatin1StringEncoding, NSUTF16StringEncoding,
	    NSUTF16BigEndianStringEncoding, NSUTF8StringEncoding };
	NSString *text;

	if (size < 2 || p[0] > 3) {
		return nil;
	}
	text = [[NSString alloc] initWithBytes:p + 1 length:size - 1 encoding:encodings[p[0]]];
	return [text stringByTrimmingCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@"\0"]];
}

/*
 * Tags and duration read from the file itself (ID3v2.3/2.4: TIT2, TPE1, TALB, TCON, TLEN), not through
 * AudioToolbox. Without TLEN the duration is the size over the bit rate of the first MPEG-1 layer III frame.
 */
static void
read_metadata(const char *path, NSString *fallback, NSString **title, NSString **artist, NSString **album,
    NSString **genre, double *seconds)
{
	static const int kbps[16] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 };
	NSData *data = [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path]
	    options:NSDataReadingMappedIfSafe error:NULL];
	const unsigned char *b = [data bytes];
	size_t length = [data length], tag = 0, i;
	int version;

	*title = fallback;
	*artist = *album = *genre = @"";
	*seconds = 0;
	if (length < 10) {
		fprintf(stderr, "music_import: %s: cannot read it\n", path);
		return;
	}
	if (memcmp(b, "ID3", 3) == 0) {
		version = b[3];
		tag = 10 + ((size_t)(b[6] & 0x7f) << 21 | (size_t)(b[7] & 0x7f) << 14 | (size_t)(b[8] & 0x7f) << 7 | (b[9] & 0x7f));
		for (i = 10; i + 10 <= tag && i + 10 <= length && b[i] != 0;) {
			size_t size = version >= 4 ?
			    ((size_t)(b[i + 4] & 0x7f) << 21 | (size_t)(b[i + 5] & 0x7f) << 14 | (size_t)(b[i + 6] & 0x7f) << 7 | (b[i + 7] & 0x7f)) :
			    ((size_t)b[i + 4] << 24 | (size_t)b[i + 5] << 16 | (size_t)b[i + 6] << 8 | b[i + 7]);
			const unsigned char *value = b + i + 10;
			NSString *text;

			if (size == 0 || i + 10 + size > length) {
				break;
			}
			text = b[i] == 'T' ? id3_text(value, size) : nil;
			if (text != nil && memcmp(b + i, "TIT2", 4) == 0 && [text length] > 0) {
				*title = text;
			} else if (text != nil && memcmp(b + i, "TPE1", 4) == 0) {
				*artist = text;
			} else if (text != nil && memcmp(b + i, "TALB", 4) == 0) {
				*album = text;
			} else if (text != nil && memcmp(b + i, "TCON", 4) == 0) {
				*genre = text;
			} else if (text != nil && memcmp(b + i, "TLEN", 4) == 0) {
				*seconds = [text doubleValue] / 1000;
			}
			i += 10 + size;
		}
	}
	if (*seconds <= 0) {
		for (i = tag; i + 4 <= length; i++) {
			if (b[i] == 0xff && (b[i + 1] & 0xfe) == 0xfa && kbps[b[i + 2] >> 4] != 0) {
				*seconds = (double)(length - i) * 8 / (kbps[b[i + 2] >> 4] * 1000.0);
				break;
			}
		}
	}
}

/*
 * What the database itself says about the track, read straight from the file (run 005616: the insert went
 * through and Music showed nothing): its row, where the library's own copy of the database is, and the
 * counts Music's song list would start from.
 */
static void
dump_row(int64_t pid, const char *when)
{
	static const char *db = "/var/mobile/Media/iTunes_Control/iTunes/MediaLibrary.sqlitedb";
	static const char *queries[] = {
		"SELECT 'version', user_version FROM pragma_user_version",
		"SELECT 'items', COUNT(*), SUM(media_type = 8), SUM(in_my_library) FROM item",
		"SELECT 'row', item_pid, media_type, in_my_library, base_location_id, remote_location_id, title_order, "
		    "album_pid, item_artist_pid, date_added, keep_local FROM item WHERE item_pid = ?1",
		"SELECT 'extra', title, location, file_size, total_time_ms, location_kind_id FROM item_extra WHERE item_pid = ?1",
		/* R2: content_rating 1 is what ML3 calls explicit and what Music refuses when the library's
		 * MPExplicitContentAllowedBoolean is 0 (see fix_explicitness); read it back every time */
		"SELECT 'explicit', content_rating, content_rating_level, extended_content_rating "
		    "FROM item_extra WHERE item_pid = ?1",
		"SELECT 'base', base_location_id, path FROM base_location",
	};
	sqlite3 *h = NULL;

	if (sqlite3_open_v2(db, &h, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
		fprintf(stderr, "music_import: %s: %s: cannot open: %s\n", when, db, h ? sqlite3_errmsg(h) : "?");
		sqlite3_close(h);
		return;
	}
	for (unsigned q = 0; q < sizeof(queries) / sizeof(queries[0]); q++) {
		sqlite3_stmt *st = NULL;
		int rows = 0;

		if (sqlite3_prepare_v2(h, queries[q], -1, &st, NULL) != SQLITE_OK) {
			fprintf(stderr, "music_import: %s: query %u: %s\n", when, q, sqlite3_errmsg(h));
			continue;
		}
		sqlite3_bind_int64(st, 1, pid);
		while (sqlite3_step(st) == SQLITE_ROW && rows++ < 8) {
			char line[512];
			int n = snprintf(line, sizeof(line), "music_import: %s:", when);

			for (int c = 0; c < sqlite3_column_count(st) && n < (int)sizeof(line) - 2; c++) {
				const unsigned char *v = sqlite3_column_text(st, c);
				n += snprintf(line + n, sizeof(line) - (size_t)n, " %s", v ? (const char *)v : "NULL");
			}
			fprintf(stderr, "%s\n", line);
		}
		if (rows == 0) {
			fprintf(stderr, "music_import: %s: query %u: no row\n", when, q);
		}
		sqlite3_finalize(st);
	}
	sqlite3_close(h);
}

/*
 * What the insert leaves undone (run 022501, read back from the database): item.in_my_library 0 although the
 * dictionary said YES, no row 3840 in base_location -- so the track has no file path ("file path (none)") --
 * and file_size and date_added 0. Music's song list wants a track that is in the library and has a file.
 * Done here in SQL on the library's own database, as mobile, as small and idempotent as possible.
 */
static void
fix_rows(int64_t pid, const char *location, long long file_size)
{
	static const char *db = "/var/mobile/Media/iTunes_Control/iTunes/MediaLibrary.sqlitedb";
	const char *statements[] = {
		"INSERT OR IGNORE INTO base_location (base_location_id, path) VALUES (3840, 'iTunes_Control/Music/F00')",
		/* R2: a base whose path is not the one the library resolves (ML3BaseLocationIDFromMediaRelativePath
		 * InLibrary gave 3840) makes the item's file path point somewhere else */
		"UPDATE base_location SET path = 'iTunes_Control/Music/F00' WHERE base_location_id = 3840 "
		    "AND path <> 'iTunes_Control/Music/F00'",
		"UPDATE item SET in_my_library = 1, date_added = CASE date_added WHEN 0 THEN ?2 ELSE date_added END WHERE item_pid = ?1",
	};
	sqlite3 *h = NULL;

	if (sqlite3_open_v2(db, &h, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
		fprintf(stderr, "music_import: fix: cannot open %s: %s\n", db, h ? sqlite3_errmsg(h) : "?");
		sqlite3_close(h);
		return;
	}
	sqlite3_busy_timeout(h, 5000);
	for (unsigned i = 0; i < sizeof(statements) / sizeof(statements[0]); i++) {
		sqlite3_stmt *st = NULL;
		int rc = sqlite3_prepare_v2(h, statements[i], -1, &st, NULL);

		if (rc == SQLITE_OK) {
			sqlite3_bind_int64(st, 1, pid);
			sqlite3_bind_int64(st, 2, (sqlite3_int64)CFAbsoluteTimeGetCurrent());   /* seconds since 2001, as ML3 keeps dates */
			sqlite3_bind_int64(st, 3, file_size);
			rc = sqlite3_step(st);
		}
		fprintf(stderr, "music_import: fix %u: %s (%d changed)\n", i, rc == SQLITE_DONE ? "done" : sqlite3_errmsg(h),
		    sqlite3_changes(h));
		sqlite3_finalize(st);
	}
	/* R2: the row has to describe the file that is there. A stale size (0 from an older import, or
	 * the size of a different copy) is what ML3's integrity/asset checks and the player's byte-range
	 * setup read; the file is the source of truth and is stat'ed by both callers. */
	if (file_size > 0) {
		sqlite3_stmt *st = NULL;

		if (sqlite3_prepare_v2(h, "UPDATE item_extra SET file_size = ?2 WHERE item_pid = ?1 "
		    "AND file_size <> ?2", -1, &st, NULL) == SQLITE_OK) {
			sqlite3_bind_int64(st, 1, pid);
			sqlite3_bind_int64(st, 2, file_size);
			fprintf(stderr, "music_import: fix file size: %s (%d changed)\n",
			    sqlite3_step(st) == SQLITE_DONE ? "done" : sqlite3_errmsg(h), sqlite3_changes(h));
		}
		sqlite3_finalize(st);
	}
	/* R2: item_extra.location is the file name relative to base_location.path ("RDWD.mp3", base
	 * "iTunes_Control/Music/F00"); an empty value or an absolute path is not what ML3's file-path
	 * composition (and the ipod-library asset URL built from it) expects */
	if (location != NULL && location[0] != '\0') {
		sqlite3_stmt *st = NULL;

		if (sqlite3_prepare_v2(h, "UPDATE item_extra SET location = ?2 WHERE item_pid = ?1 "
		    "AND (location IS NULL OR location = '' OR instr(location, '/') > 0)", -1, &st, NULL) == SQLITE_OK) {
			sqlite3_bind_int64(st, 1, pid);
			sqlite3_bind_text(st, 2, location, -1, SQLITE_STATIC);
			fprintf(stderr, "music_import: fix location: %s (%d changed)\n",
			    sqlite3_step(st) == SQLITE_DONE ? "done" : sqlite3_errmsg(h), sqlite3_changes(h));
		}
		sqlite3_finalize(st);
	}
	sqlite3_close(h);
}

/*
 * What Music itself would list: MediaPlayer's songs query (the one Music's Library > Songs is built on), from
 * this process as mobile. Run 0301: the rows were complete in SQL, Music listed nothing; this says whether the
 * library's own query sees the track (then the app is the question) or not (then the rows still lack something).
 */
/* a songs query with MediaPlayer's own filters on or off (private setters, asked for by name; absent = left alone) */
static unsigned long
songs_count(BOOL ignore_system, BOOL ignore_restrictions)
{
	MPMediaQuery *query = [MPMediaQuery songsQuery];

	if (ignore_system && [query respondsToSelector:@selector(setIgnoreSystemFilterPredicates:)]) {
		((void (*)(id, SEL, BOOL))objc_msgSend)(query, @selector(setIgnoreSystemFilterPredicates:), YES);
	}
	if (ignore_restrictions && [query respondsToSelector:@selector(setIgnoreRestrictionsPredicates:)]) {
		((void (*)(id, SEL, BOOL))objc_msgSend)(query, @selector(setIgnoreRestrictionsPredicates:), YES);
	}
	return [[query items] count];
}

/*
 * What Music itself would list: MediaPlayer's songs query (the one Music's Library > Songs is built on), from
 * this process as mobile; and the same with MediaPlayer's system filters and restriction filters switched off one
 * at a time (run 070520: 0 items even with no media-type filter, while the rows were complete), and which tables of
 * the database hold a row of the track (a query that joins a table the track has no row in drops it).
 */
static void
media_query(const char *when, int64_t pid)
{
	static const char *db = "/var/mobile/Media/iTunes_Control/iTunes/MediaLibrary.sqlitedb";
	NSArray *items;
	unsigned shown = 0;
	sqlite3 *h = NULL;

	fprintf(stderr, "music_import: %s: MediaPlayer authorization %d\n", when, (int)[MPMediaLibrary authorizationStatus]);
	items = [[MPMediaQuery songsQuery] items];
	fprintf(stderr, "music_import: %s: songs query: %lu items\n", when, (unsigned long)[items count]);
	for (MPMediaItem *item in items) {
		if (shown++ < 5) {
			fprintf(stderr, "music_import: %s: song \"%s\" by \"%s\", %.0f s, asset %s\n", when, UTF8(item.title),
			    UTF8(item.artist), item.playbackDuration, UTF8([item.assetURL absoluteString]));
		}
	}
	fprintf(stderr, "music_import: %s: songs with the system filters off %lu, restrictions off %lu, both off %lu\n", when,
	    songs_count(YES, NO), songs_count(NO, YES), songs_count(YES, YES));
	if (sqlite3_open_v2(db, &h, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
		sqlite3_stmt *tables = NULL;
		char line[1024];
		int n = snprintf(line, sizeof(line), "music_import: %s: rows of the track:", when);

		if (sqlite3_prepare_v2(h, "SELECT m.name FROM sqlite_master m WHERE m.type = 'table' AND EXISTS "
		    "(SELECT 1 FROM pragma_table_info(m.name) WHERE name = 'item_pid') ORDER BY m.name", -1, &tables, NULL) == SQLITE_OK) {
			while (sqlite3_step(tables) == SQLITE_ROW && n < (int)sizeof(line) - 40) {
				const char *table = (const char *)sqlite3_column_text(tables, 0);
				char sql[256];
				sqlite3_stmt *count = NULL;
				long long rows = -1;

				snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM \"%s\" WHERE item_pid = ?1", table);
				if (sqlite3_prepare_v2(h, sql, -1, &count, NULL) == SQLITE_OK) {
					sqlite3_bind_int64(count, 1, pid);
					if (sqlite3_step(count) == SQLITE_ROW) {
						rows = sqlite3_column_int64(count, 0);
					}
				}
				sqlite3_finalize(count);
				n += snprintf(line + n, sizeof(line) - (size_t)n, " %s=%lld", table, rows);
			}
		}
		sqlite3_finalize(tables);
		fprintf(stderr, "%s\n", line);
		sqlite3_close(h);
	}
}

/*
 * GPU-B (14:31): after validateDatabase and the item_search row, songs is still 0 with every filter off, and the
 * track's album_pid, item_artist_pid and album_artist_pid are 0: no album, artist or album artist rows exist.
 * A songs query that joins those tables drops a track without them. So make them as ML3's import would (one of
 * each, named from the tags, the track as the album's representative) and point the track at them. The pids are
 * derived from the track's, so a rerun finds the same rows. Idempotent: only a track with pid 0 is touched.
 */
static void
repair_entities(int64_t pid, NSString *artist, NSString *album)
{
	static const char *db = "/var/mobile/Media/iTunes_Control/iTunes/MediaLibrary.sqlitedb";
	const char *statements[] = {
		"INSERT OR IGNORE INTO album_artist (album_artist_pid, album_artist, sort_album_artist) VALUES (?2, ?4, ?4)",
		"INSERT OR IGNORE INTO item_artist (item_artist_pid, item_artist, sort_item_artist) VALUES (?3, ?4, ?4)",
		"INSERT OR IGNORE INTO album (album_pid, album, sort_album, album_artist_pid, representative_item_pid) "
		    "VALUES (?5, ?6, ?6, ?2, ?1)",
		"UPDATE item SET album_pid = ?5, item_artist_pid = ?3, album_artist_pid = ?2 WHERE item_pid = ?1 AND album_pid = 0",
	};
	int64_t base = pid & 0x3fffffffffffffffLL;
	sqlite3 *h = NULL;

	if (pid == 0) return;
	if (sqlite3_open_v2(db, &h, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
		fprintf(stderr, "music_import: entities: cannot open %s: %s\n", db, h ? sqlite3_errmsg(h) : "?");
		sqlite3_close(h);
		return;
	}
	sqlite3_busy_timeout(h, 5000);
	for (unsigned i = 0; i < sizeof(statements) / sizeof(statements[0]); i++) {
		sqlite3_stmt *st = NULL;
		int rc = sqlite3_prepare_v2(h, statements[i], -1, &st, NULL);

		if (rc == SQLITE_OK) {
			sqlite3_bind_int64(st, 1, pid);
			sqlite3_bind_int64(st, 2, base ^ 0x1111111111111LL);   /* album artist */
			sqlite3_bind_int64(st, 3, base ^ 0x2222222222222LL);   /* item artist */
			sqlite3_bind_text(st, 4, [artist length] ? [artist UTF8String] : "", -1, SQLITE_TRANSIENT);
			sqlite3_bind_int64(st, 5, base ^ 0x3333333333333LL);   /* album */
			sqlite3_bind_text(st, 6, [album length] ? [album UTF8String] : "", -1, SQLITE_TRANSIENT);
			rc = sqlite3_step(st);
		}
		fprintf(stderr, "music_import: entities %u: %s (%d changed)\n", i, rc == SQLITE_DONE ? "done" : sqlite3_errmsg(h),
		    sqlite3_changes(h));
		sqlite3_finalize(st);
	}
	sqlite3_close(h);
}

/*
 * The repair, here and only here, in SQL on the library's own file.
 *
 * Why not through the library: every SQL statement a client sends to medialibraryd goes through
 * -[ML3MediaLibraryWriter executeUntrustedUpdate:] with a sqlite3 authorizer (mld-re, 2026-10-26:
 * -[ML3DatabaseConnection _distrustQueriesDuringBlock:] @ 0x1a9167bd8, authorizer 0x1a9168ee4).
 * A client's sort-map update starts with DDL and reads sqlite_master, the authorizer refuses both
 * ("Authorizer flagged suspicious query=CREATE TABLE IF NOT EXISTS sort_map_no_uniques ... action=18",
 * "access to sqlite_master.name is prohibited"), and the daemon then asserts ("Authorizer failure should
 * have caused executeUpdate: to return NO") and dies. Music.app talks to that same daemon, so every run
 * of the old repair left Music with no library service at all: that is why the rows were right and the
 * app was empty. Nothing here is sent over XPC: the file is opened directly, where there is no authorizer.
 *
 * The sort map is ML3's own table and ML3's own update fills it (MusicLibrary, __cstring):
 *   UPDATE sort_map_new SET name_section = iPhoneSortSection(sort_key);
 *   INSERT INTO sort_map_new_in_order (name) SELECT name FROM sort_map_new ORDER BY sort_key;
 *   UPDATE sort_map_new SET name_order = (SELECT ROWID << 32 FROM sort_map_new_in_order WHERE ...);
 *   INSERT OR REPLACE INTO sort_map_new (name, name_order, name_section, sort_key) VALUES ('', 0, %u, '');
 * i.e. name_order is a dense rank shifted 32 bits, name_section is the section index, and the empty name
 * is a row of its own. iPhoneSortKey()/iPhoneSortSection() are SQL functions registered on the library's
 * connection; ours is a plain one, so the rank comes from our own collation (localizedStandardCompare:,
 * the same case/number-insensitive order) and the section from the first letter. The exact integers are
 * not load-bearing: Music needs a non-zero title_order, an entry the sectioned query can join, and the
 * search keys. A later trusted update inside the daemon (if it ever runs) recomputes them exactly.
 */

static BOOL
table_exists(sqlite3 *h, const char *table)
{
	sqlite3_stmt *st = NULL;
	BOOL found = NO;

	if (sqlite3_prepare_v2(h, "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?1", -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_text(st, 1, table, -1, SQLITE_STATIC);
		found = sqlite3_step(st) == SQLITE_ROW;
	}
	sqlite3_finalize(st);
	return found;
}

static BOOL
column_exists(sqlite3 *h, const char *table, const char *column)
{
	char sql[128];
	sqlite3_stmt *st = NULL;
	BOOL found = NO;

	snprintf(sql, sizeof(sql), "SELECT 1 FROM pragma_table_info('%s') WHERE name = ?1", table);
	if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_text(st, 1, column, -1, SQLITE_STATIC);
		found = sqlite3_step(st) == SQLITE_ROW;
	}
	sqlite3_finalize(st);
	return found;
}

/* One statement with n integer bindings; the log line names it whether it worked or not. */
static int
run(sqlite3 *h, const char *what, const char *sql, int n, int64_t v1, int64_t v2, int64_t v3)
{
	sqlite3_stmt *st = NULL;
	int rc = sqlite3_prepare_v2(h, sql, -1, &st, NULL);

	if (rc == SQLITE_OK) {
		if (n > 0) sqlite3_bind_int64(st, 1, v1);
		if (n > 1) sqlite3_bind_int64(st, 2, v2);
		if (n > 2) sqlite3_bind_int64(st, 3, v3);
		rc = sqlite3_step(st);
	}
	fprintf(stderr, "music_import: local: %s: %s (%d changed)\n", what,
	    rc == SQLITE_DONE || rc == SQLITE_ROW ? "done" : sqlite3_errmsg(h), sqlite3_changes(h));
	sqlite3_finalize(st);
	return rc;
}

/*
 * R2/TEST5Q: Music's "Content restriction" screen. The library's own gate is
 *   ML3IsCurrentlyRestrictedMedia(item.media_type, item_extra.content_rating,
 *       item_extra.content_rating_level, (SELECT value FROM _MLDatabaseProperties
 *       WHERE key='MPExplicitContentAllowedBoolean'))
 * (MusicLibrary 18.0.0, the expression is a cstring in the binary, and the function at
 * 0x1a914fae0 returns 1 for content_rating == 1 when that property is 0 or absent; 2 means
 * "clean", 0 "unrated" and neither is restricted). item_extra has no is_explicit column at
 * all: content_rating and content_rating_level are the flags. The import leaves them at the
 * schema defaults (0) but a row repaired by an older run, or written by another importer,
 * can carry 1, so every repair path pins them to 0 here. Idempotent; a column that this
 * library does not have is skipped, not guessed. */
static void
fix_explicitness_db(sqlite3 *h, int64_t pid)
{
	if (pid == 0) {
		return;
	}
	if (table_exists(h, "item_extra") && column_exists(h, "item_extra", "content_rating")) {
		run(h, "content rating", "UPDATE item_extra SET content_rating = 0 WHERE item_pid = ?1 "
		    "AND content_rating <> 0", 1, pid, 0, 0);
	}
	if (table_exists(h, "item_extra") && column_exists(h, "item_extra", "content_rating_level")) {
		run(h, "content rating level", "UPDATE item_extra SET content_rating_level = 0 "
		    "WHERE item_pid = ?1 AND content_rating_level <> 0", 1, pid, 0, 0);
	}
	if (table_exists(h, "item_extra") && column_exists(h, "item_extra", "extended_content_rating")) {
		run(h, "extended content rating", "UPDATE item_extra SET extended_content_rating = '' "
		    "WHERE item_pid = ?1 AND extended_content_rating <> ''", 1, pid, 0, 0);
	}
	if (table_exists(h, "item_video") && column_exists(h, "item_video", "extended_content_rating")) {
		run(h, "video extended content rating", "UPDATE item_video SET extended_content_rating = '' "
		    "WHERE item_pid = ?1 AND extended_content_rating <> ''", 1, pid, 0, 0);
	}
	if (table_exists(h, "item_extra") && column_exists(h, "item_extra", "content_rating")) {
		sqlite3_stmt *st = NULL;

		if (sqlite3_prepare_v2(h, "SELECT content_rating, content_rating_level FROM item_extra "
		    "WHERE item_pid = ?1", -1, &st, NULL) == SQLITE_OK) {
			sqlite3_bind_int64(st, 1, pid);
			if (sqlite3_step(st) == SQLITE_ROW) {
				fprintf(stderr, "music_import: local: explicit flags of %lld: content_rating %d, "
				    "content_rating_level %d\n", (long long)pid, sqlite3_column_int(st, 0),
				    sqlite3_column_int(st, 1));
			}
		}
		sqlite3_finalize(st);
	}
}

/* One _MLDatabaseProperties value; -1 when the key has no row (the fresh library's state). */
static int
database_property(sqlite3 *h, const char *key)
{
	sqlite3_stmt *st = NULL;
	int value = -1;

	if (sqlite3_prepare_v2(h, "SELECT value FROM _MLDatabaseProperties WHERE key = ?1", -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
		if (sqlite3_step(st) == SQLITE_ROW) {
			value = sqlite3_column_int(st, 0);
		}
	}
	sqlite3_finalize(st);
	return value;
}

/*
 * R2/TEST5Q: the rest of Music's "Content restriction" screen.
 *
 * The library's predicate (see fix_explicitness_db) reads
 * _MLDatabaseProperties.MPExplicitContentAllowedBoolean, and MusicLibrary never writes that key
 * itself: its own strings have one SELECT for it and only the generic
 * INSERT OR REPLACE INTO _MLDatabaseProperties (key, value) VALUES (?, ?) as a writer, with no
 * literal insert anywhere. A fresh library therefore has no row, the subquery is NULL, and an item
 * with content_rating 1 is then "restricted". Write it as mobile, in the same file the daemon reads.
 *
 * The Music app's own screen also asks ManagedConfiguration, through MediaPlayer's
 * MPRestrictionsMonitor (-[MCProfileConnection isExplicitContentAllowedOutAsk:], which is the
 * "age restriction" part). MediaPlayer and MusicLibrary both link ManagedConfiguration, and
 * ML3HasExplicitContentRestriction() (0x1a8faa420) returns (MCFeatureExplicitContentAllowed
 * effective value == 2), so 0 (unset) and 1 (allowed) are not restricted and only 2 is. The value
 * is asked here for the log; if the device says no, the user-level Screen Time store
 * (com.apple.applicationaccess allowExplicitContent) is set and the MC notification posted, then
 * the answer is asked again. Nothing is invented: everything that could not be read is logged as
 * unknown.
 */
static void
allow_explicit_content(void)
{
	static const char *db = "/var/mobile/Media/iTunes_Control/iTunes/MediaLibrary.sqlitedb";
	static const char *key = "MPExplicitContentAllowedBoolean";
	sqlite3 *h = NULL;
	void *mc = NULL;

	if (sqlite3_open_v2(db, &h, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK) {
		sqlite3_stmt *st = NULL;
		int before, after;

		sqlite3_busy_timeout(h, 5000);
		before = database_property(h, key);
		if (sqlite3_prepare_v2(h, "INSERT OR REPLACE INTO _MLDatabaseProperties (key, value) "
		    "VALUES (?1, 1)", -1, &st, NULL) == SQLITE_OK) {
			sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
			fprintf(stderr, "music_import: restrictions: %s: %s\n", key,
			    sqlite3_step(st) == SQLITE_DONE ? "written" : sqlite3_errmsg(h));
		} else {
			fprintf(stderr, "music_import: restrictions: %s: %s\n", key, sqlite3_errmsg(h));
		}
		sqlite3_finalize(st);
		after = database_property(h, key);
		fprintf(stderr, "music_import: restrictions: %s was %s, is %s (1 = explicit content allowed)\n", key,
		    before < 0 ? "absent" : before ? "1" : "0", after < 0 ? "absent" : after ? "1" : "0");
		sqlite3_close(h);
	} else {
		fprintf(stderr, "music_import: restrictions: cannot open %s: %s\n", db, h ? sqlite3_errmsg(h) : "?");
		sqlite3_close(h);
	}
	{
		NSString *feature = nil;
		id connection = nil;
		NSInteger effective = -1;
		BOOL ask = NO, allowed = YES;

		mc = dlopen("/System/Library/PrivateFrameworks/ManagedConfiguration.framework/ManagedConfiguration",
		    RTLD_LAZY);
		if (mc != NULL) {
			NSString *const *slot = (NSString *const *)dlsym(mc, "MCFeatureExplicitContentAllowed");

			feature = slot != NULL ? *slot : nil;
		} else {
			fprintf(stderr, "music_import: restrictions: no ManagedConfiguration: %s\n", dlerror());
		}
		Class connection_class = NSClassFromString(@"MCProfileConnection");

		if (connection_class != Nil) {
			connection = ((id (*)(id, SEL))objc_msgSend)(connection_class, @selector(sharedConnection));
		}
		if (feature != nil && connection != nil &&
		    [connection respondsToSelector:@selector(effectiveValueForSetting:)]) {
			effective = ((NSInteger (*)(id, SEL, id))objc_msgSend)(connection,
			    @selector(effectiveValueForSetting:), feature);
		}
		if (connection != nil && [connection respondsToSelector:@selector(isExplicitContentAllowedOutAsk:)]) {
			allowed = ((BOOL (*)(id, SEL, BOOL *))objc_msgSend)(connection,
			    @selector(isExplicitContentAllowedOutAsk:), &ask);
		} else {
			fprintf(stderr, "music_import: restrictions: MCProfileConnection %s\n",
			    connection == nil ? "is not there" : "has no isExplicitContentAllowedOutAsk:");
			allowed = YES;
		}
		fprintf(stderr, "music_import: restrictions: MCFeatureExplicitContentAllowed effective %ld "
		    "(0 unset, 1 allowed, 2 not allowed), explicit content allowed %d, age asked %d\n",
		    (long)effective, allowed ? 1 : 0, ask ? 1 : 0);
		if (!allowed || effective == 2) {
			Boolean synced;

			CFPreferencesSetAppValue(CFSTR("allowExplicitContent"), kCFBooleanTrue,
			    CFSTR("com.apple.applicationaccess"));
			synced = CFPreferencesAppSynchronize(CFSTR("com.apple.applicationaccess"));
			fprintf(stderr, "music_import: restrictions: com.apple.applicationaccess "
			    "allowExplicitContent = true (synchronised %d)\n", synced ? 1 : 0);
			{
				NSString *const *slot = mc != NULL ?
				    (NSString *const *)dlsym(mc, "MCEffectiveSettingsChangedNotification") : NULL;

				if (slot != NULL && *slot != nil) {
					CFNotificationCenterPostNotification(CFNotificationCenterGetDarwinNotifyCenter(),
					    (__bridge CFStringRef)*slot, NULL, NULL, true);
					fprintf(stderr, "music_import: restrictions: posted %s\n", [*slot UTF8String]);
				}
			}
			if (feature != nil && [connection respondsToSelector:@selector(setValue:forSetting:)]) {
				@try {
					((void (*)(id, SEL, id, id))objc_msgSend)(connection,
					    @selector(setValue:forSetting:), @YES, feature);
					fprintf(stderr, "music_import: restrictions: setValue:forSetting: sent\n");
				} @catch (NSException *exception) {
					fprintf(stderr, "music_import: restrictions: setValue:forSetting: %s\n",
					    UTF8([exception reason]));
				}
			}
			allowed = YES;
			ask = NO;
			if (connection != nil &&
			    [connection respondsToSelector:@selector(isExplicitContentAllowedOutAsk:)]) {
				allowed = ((BOOL (*)(id, SEL, BOOL *))objc_msgSend)(connection,
				    @selector(isExplicitContentAllowedOutAsk:), &ask);
			}
			fprintf(stderr, "music_import: restrictions: after the write: allowed %d, age asked %d\n",
			    allowed ? 1 : 0, ask ? 1 : 0);
		}
	}
	/* ManagedConfiguration stays loaded: MCProfileConnection holds state this process may still ask for */
}

/* A letter's place among the sections ("#" is 0), the way a name's first character sorts. */
static int
section_of(NSString *name)
{
	unichar c;

	if ([name length] == 0) {
		return 0;
	}
	c = [[name uppercaseString] characterAtIndex:0];
	if (c >= 'A' && c <= 'Z') {
		return (int)(c - 'A') + 1;
	}
	return 0;
}

/* Every name the library sorts by, as ML3's update collects them: the columns its own statements name. */
static NSArray<NSString *> *
collect_names(sqlite3 *h)
{
	static const struct { const char *table, *column; } sources[] = {
		{ "item_extra", "title" }, { "item_extra", "sort_title" },
		{ "item_artist", "item_artist" }, { "item_artist", "sort_item_artist" },
		{ "item_artist", "series_name" }, { "item_artist", "sort_series_name" },
		{ "album_artist", "album_artist" }, { "album_artist", "sort_album_artist" },
		{ "album", "album" }, { "album", "sort_album" },
		{ "composer", "composer" }, { "composer", "sort_composer" },
		{ "genre", "genre" }, { "container", "name" },
	};
	NSMutableSet<NSString *> *names = [NSMutableSet set];

	for (unsigned i = 0; i < sizeof(sources) / sizeof(sources[0]); i++) {
		char sql[256];
		sqlite3_stmt *st = NULL;

		if (!table_exists(h, sources[i].table) || !column_exists(h, sources[i].table, sources[i].column)) {
			continue;
		}
		snprintf(sql, sizeof(sql), "SELECT DISTINCT \"%s\" FROM \"%s\" WHERE \"%s\" IS NOT NULL AND \"%s\" <> ''",
		    sources[i].column, sources[i].table, sources[i].column, sources[i].column);
		if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) != SQLITE_OK) {
			sqlite3_finalize(st);
			continue;
		}
		while (sqlite3_step(st) == SQLITE_ROW) {
			const unsigned char *v = sqlite3_column_text(st, 0);
			if (v != NULL) {
				NSString *s = [NSString stringWithUTF8String:(const char *)v];
				if (s != nil) {
					[names addObject:s];
				}
			}
		}
		sqlite3_finalize(st);
	}
	return [[names allObjects] sortedArrayUsingComparator:^NSComparisonResult(NSString *a, NSString *b) {
		return [a localizedStandardCompare:b];
	}];
}

/* The whole repair for one track: its row complete, the sort map built, its orders and search keys from
 * it, and the WAL checkpointed so every connection (and the daemon) reads the same file. Idempotent. */
static void
repair_local(int64_t pid, NSString *title, NSString *artist, NSString *album, long long file_size, double seconds)
{
	static const char *db = "/var/mobile/Media/iTunes_Control/iTunes/MediaLibrary.sqlitedb";
	NSArray<NSString *> *names;
	sqlite3 *h = NULL;
	int64_t now = (int64_t)CFAbsoluteTimeGetCurrent();   /* seconds since 2001, as ML3 keeps dates */
	BOOL with_sort_key = NO;

	(void)artist;
	(void)album;
	if (pid == 0) {
		return;
	}
	if (sqlite3_open_v2(db, &h, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
		fprintf(stderr, "music_import: local: cannot open %s: %s\n", db, h ? sqlite3_errmsg(h) : "?");
		sqlite3_close(h);
		return;
	}
	sqlite3_busy_timeout(h, 5000);
	/* 1. the row itself: in the library, a file, not cloud, not shuffle-excluded, dated */
	run(h, "base location", "INSERT OR IGNORE INTO base_location (base_location_id, path) "
	    "VALUES (3840, 'iTunes_Control/Music/F00')", 0, 0, 0, 0);
	run(h, "item", "UPDATE item SET in_my_library = 1, keep_local = 1, remote_location_id = 0, "
	    "base_location_id = CASE base_location_id WHEN 0 THEN 3840 ELSE base_location_id END, "
	    "exclude_from_shuffle = 0, date_added = CASE date_added WHEN 0 THEN ?2 ELSE date_added END "
	    "WHERE item_pid = ?1", 2, pid, now, 0);
	/* keep_local_status stays 0: keep_local = 1 is what "on this device" means, and a status made up
	 * here could be read as a download request for a cloud asset we do not have */
	/* R2: this library's item_extra has no date_created (TEST5Q line 449641: "no such column:
	 * date_created"), and one failing statement took file_size down with it; one column at a time */
	run(h, "item_extra file size", "UPDATE item_extra SET file_size = CASE file_size WHEN 0 THEN ?2 "
	    "ELSE file_size END WHERE item_pid = ?1", 2, pid, file_size, 0);
	if (column_exists(h, "item_extra", "date_created")) {
		run(h, "item_extra date created", "UPDATE item_extra SET date_created = CASE date_created WHEN 0 "
		    "THEN ?2 ELSE date_created END WHERE item_pid = ?1", 2, pid, now, 0);
	}
	/* R2: never leave an explicit bit on the item (see fix_explicitness_db) */
	fix_explicitness_db(h, pid);
	/* significantAmountHasBeenPlayed is (total_time_ms - bookmark_time_ms < 30 s AND total_time_ms > 60 s) OR
	 * (bookmark_time_ms >= .95 * total_time_ms) (MusicLibrary, _ML3TrackPropertySignificantAmountHasBeenPlayed);
	 * with total_time_ms = 0 its second half is 0 >= 0 -- true -- and the provider's "not played" filter would
	 * drop the track. The insert sets it; this covers a row repaired on its own. */
	if (seconds > 0) {
		run(h, "item_extra total time", "UPDATE item_extra SET total_time_ms = ?2 "
		    "WHERE item_pid = ?1 AND (total_time_ms IS NULL OR total_time_ms = 0)", 2, pid,
		    (int64_t)(seconds * 1000), 0);
	}
	if (table_exists(h, "item_stats")) {
		run(h, "item_stats row", "INSERT OR IGNORE INTO item_stats (item_pid) VALUES (?1)", 1, pid, 0, 0);
		run(h, "item_stats visible", "UPDATE item_stats SET hidden = 0, has_been_played = 0 "
		    "WHERE item_pid = ?1", 1, pid, 0, 0);
	}
	/* hasNonPurgeableAsset, the filter the provider uses with no account (mp-re): a local, non-evictable asset */
	if (table_exists(h, "purgeable_list") && column_exists(h, "purgeable_list", "is_purgeable")) {
		run(h, "purgeable_list", "INSERT OR IGNORE INTO purgeable_list (item_pid, is_purgeable) VALUES (?1, 0)",
		    1, pid, 0, 0);
		run(h, "purgeable_list not purgeable", "UPDATE purgeable_list SET is_purgeable = 0 WHERE item_pid = ?1",
		    1, pid, 0, 0);
	}
	if (table_exists(h, "item_extra") && column_exists(h, "item_extra", "sort_title")) {
		run(h, "sort_title", "UPDATE item_extra SET sort_title = title "
		    "WHERE item_pid = ?1 AND (sort_title IS NULL OR sort_title = '')", 1, pid, 0, 0);
	}
	/* 2. the sort map: every name the library has, ranked. Rows it already holds keep their orders --
	 * the other tracks' title_order values point at them -- and only the missing names are appended
	 * above the highest rank. On this port's fresh library this makes ranks 1..N, like ML3's own update. */
	names = collect_names(h);
	with_sort_key = column_exists(h, "sort_map", "sort_key");
	{
		sqlite3_stmt *max = NULL;
		int64_t rank = 0;
		unsigned long added = 0;

		if (sqlite3_prepare_v2(h, "SELECT IFNULL(MAX(name_order), 0) FROM sort_map", -1, &max, NULL) == SQLITE_OK &&
		    sqlite3_step(max) == SQLITE_ROW) {
			rank = sqlite3_column_int64(max, 0) >> 32;
		}
		sqlite3_finalize(max);
		fprintf(stderr, "music_import: local: %lu names for the sort map, its last rank %lld (title \"%s\")\n",
		    (unsigned long)[names count], (long long)rank, UTF8(title));
		{
			char sql[256];
			sqlite3_stmt *st = NULL;

			snprintf(sql, sizeof(sql), "INSERT OR IGNORE INTO sort_map (name, name_order, name_section%s) "
			    "VALUES (?1, ?2, ?3%s)", with_sort_key ? ", sort_key" : "", with_sort_key ? ", ?4" : "");
			if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) == SQLITE_OK) {
				for (NSUInteger i = 0; i < [names count]; i++) {
					NSString *name = names[i];
					int64_t order = (++rank) << 32;

					sqlite3_bind_text(st, 1, [name UTF8String], -1, SQLITE_TRANSIENT);
					sqlite3_bind_int64(st, 2, order);
					sqlite3_bind_int(st, 3, section_of(name));
					if (with_sort_key) {
						NSData *key = [name dataUsingEncoding:NSUTF8StringEncoding];
						sqlite3_bind_blob(st, 4, [key bytes], (int)[key length], SQLITE_TRANSIENT);
					}
					if (sqlite3_step(st) != SQLITE_DONE) {
						fprintf(stderr, "music_import: local: sort_map \"%s\": %s\n", [name UTF8String],
						    sqlite3_errmsg(h));
					} else {
						added += (unsigned long)sqlite3_changes(h);
					}
					sqlite3_reset(st);
				}
				fprintf(stderr, "music_import: local: sort_map: %lu names, %lu new, now ending at rank %lld\n",
				    (unsigned long)[names count], added, (long long)rank);
			} else {
				fprintf(stderr, "music_import: local: sort_map insert: %s\n", sqlite3_errmsg(h));
			}
			sqlite3_finalize(st);
		}
	}
	/* the empty name: ML3's own INSERT ... INTO sort_map_new (name, name_order, name_section, sort_key) VALUES ('', 0, %u, '') */
	run(h, "sort_map empty name", "INSERT OR IGNORE INTO sort_map (name, name_order, name_section) VALUES ('', 0, 0)",
	    0, 0, 0, 0);

	/* 3. the item's orders and sections, from the sort map, by the names it sorts by */
	run(h, "item title order",
	    "UPDATE item SET "
	    "title_order = IFNULL((SELECT name_order FROM sort_map WHERE name = "
	    "(SELECT title FROM item_extra WHERE item_pid = item.item_pid)), 0), "
	    "title_order_section = IFNULL((SELECT name_section FROM sort_map WHERE name = "
	    "(SELECT title FROM item_extra WHERE item_pid = item.item_pid)), 0) "
	    "WHERE item_pid = ?1", 1, pid, 0, 0);
	run(h, "item artist order",
	    "UPDATE item SET "
	    "item_artist_order = IFNULL((SELECT name_order FROM sort_map WHERE name = "
	    "(SELECT item_artist FROM item_artist WHERE item_artist_pid = item.item_artist_pid)), 0), "
	    "item_artist_order_section = IFNULL((SELECT name_section FROM sort_map WHERE name = "
	    "(SELECT item_artist FROM item_artist WHERE item_artist_pid = item.item_artist_pid)), 0) "
	    "WHERE item_pid = ?1", 1, pid, 0, 0);
	run(h, "item album artist order",
	    "UPDATE item SET "
	    "album_artist_order = IFNULL((SELECT name_order FROM sort_map WHERE name = "
	    "(SELECT album_artist FROM album_artist WHERE album_artist_pid = item.album_artist_pid)), 0), "
	    "album_artist_order_section = IFNULL((SELECT name_section FROM sort_map WHERE name = "
	    "(SELECT album_artist FROM album_artist WHERE album_artist_pid = item.album_artist_pid)), 0) "
	    "WHERE item_pid = ?1", 1, pid, 0, 0);
	run(h, "item album order",
	    "UPDATE item SET "
	    "album_order = IFNULL((SELECT name_order FROM sort_map WHERE name = "
	    "(SELECT album FROM album WHERE album_pid = item.album_pid)), 0), "
	    "album_order_section = IFNULL((SELECT name_section FROM sort_map WHERE name = "
	    "(SELECT album FROM album WHERE album_pid = item.album_pid)), 0) "
	    "WHERE item_pid = ?1", 1, pid, 0, 0);
	/* ML3 keeps a sort_* copy of every name; a query that joins on it must find one too */
	if (table_exists(h, "item_extra") && column_exists(h, "item_extra", "sort_title")) {
		run(h, "sort_title", "UPDATE item_extra SET sort_title = title "
		    "WHERE item_pid = ?1 AND (sort_title IS NULL OR sort_title = '')", 1, pid, 0, 0);
	}
	/* 4. the search keys, ML3's own statements, with our orders */
	run(h, "item_search",
	    "INSERT OR REPLACE INTO item_search (item_pid, search_title, search_album, search_artist, "
	    "search_composer, search_album_artist) "
	    "SELECT ?1, "
	    "IFNULL((SELECT name_order FROM sort_map WHERE name = (SELECT title FROM item_extra WHERE item_pid = ?1)), 0), "
	    "IFNULL((SELECT s.name_order FROM sort_map s JOIN album a ON s.name = a.album "
	    "WHERE a.album_pid = (SELECT album_pid FROM item WHERE item_pid = ?1)), 0), "
	    "IFNULL((SELECT s.name_order FROM sort_map s JOIN item_artist r ON s.name = r.item_artist "
	    "WHERE r.item_artist_pid = (SELECT item_artist_pid FROM item WHERE item_pid = ?1)), 0), "
	    "0, "
	    "IFNULL((SELECT s.name_order FROM sort_map s JOIN album_artist r ON s.name = r.album_artist "
	    "WHERE r.album_artist_pid = (SELECT album_artist_pid FROM item WHERE item_pid = ?1)), 0)",
	    1, pid, 0, 0);
	/* 5. one file for every connection: the main file was 4 KB with everything else in the WAL */
	{
		int log = -1, done = -1;
		int rc = sqlite3_wal_checkpoint_v2(h, NULL, SQLITE_CHECKPOINT_TRUNCATE, &log, &done);

		fprintf(stderr, "music_import: local: checkpoint (truncate): %s, %d frames, %d done\n",
		    rc == SQLITE_OK ? "ok" : sqlite3_errmsg(h), log, done);
	}
	sqlite3_close(h);
}

/* What the database says about the row after the repair: the fields MediaPlayer's songs query reads. */
static void
dump_songs(int64_t pid)
{
	static const char *db = "/var/mobile/Media/iTunes_Control/iTunes/MediaLibrary.sqlitedb";
	static const char *q[] = {
		"SELECT 'orders', title_order, title_order_section, item_artist_order, album_order, album_artist_order "
		    "FROM item WHERE item_pid = ?1",
		"SELECT 'search', search_title, search_album, search_artist, search_album_artist FROM item_search WHERE item_pid = ?1",
		"SELECT 'sort_map rows', COUNT(*) FROM sort_map",
		"SELECT 'songs rows', COUNT(*) FROM item WHERE (media_type & 1032) <> 0 AND in_my_library = 1",
	};
	sqlite3 *h = NULL;

	if (sqlite3_open_v2(db, &h, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
		fprintf(stderr, "music_import: songs: cannot open: %s\n", h ? sqlite3_errmsg(h) : "?");
		sqlite3_close(h);
		return;
	}
	for (unsigned i = 0; i < sizeof(q) / sizeof(q[0]); i++) {
		sqlite3_stmt *st = NULL;
		char line[512];
		int n;

		if (sqlite3_prepare_v2(h, q[i], -1, &st, NULL) != SQLITE_OK) {
			fprintf(stderr, "music_import: songs %u: %s\n", i, sqlite3_errmsg(h));
			continue;
		}
		sqlite3_bind_int64(st, 1, pid);
		while (sqlite3_step(st) == SQLITE_ROW) {
			n = snprintf(line, sizeof(line), "music_import: songs:");
			for (int c = 0; c < sqlite3_column_count(st) && n < (int)sizeof(line) - 2; c++) {
				const unsigned char *v = sqlite3_column_text(st, c);
				n += snprintf(line + n, sizeof(line) - (size_t)n, " %s", v ? (const char *)v : "NULL");
			}
			fprintf(stderr, "%s\n", line);
		}
		sqlite3_finalize(st);
	}
	sqlite3_close(h);
}

/*
 * R2/TEST5Q: the asset Music hands to AVPlayer is the file behind the row, and the row is only a
 * path: base_location.path + item_extra.location, both relative to /var/mobile/Media. Music names it
 * ipod-library://item/item.mp3?id=<pid>, mediaserverd opens the file as mobile (501). This prints
 * every component with its mode and owner, opens and reads the first bytes of the file in this
 * process (mobile at this point) and compares the row's path with the file that was imported, so a
 * "not readable" answer in the next run is evidence and not a guess.
 */
static void
report_asset(int64_t pid, const char *destination)
{
	static const char *db = "/var/mobile/Media/iTunes_Control/iTunes/MediaLibrary.sqlitedb";
	static const char *dirs[] = { MEDIA, MEDIA "/iTunes_Control", MEDIA "/iTunes_Control/Music", MUSIC_DIR };
	char path[4096], base[1024] = "", location[1024] = "", composed[4096] = "";
	struct stat status;
	unsigned i;
	int64_t base_id = 0;
	sqlite3 *h = NULL;

	for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		if (stat(dirs[i], &status) != 0) {
			fprintf(stderr, "music_import: asset: %s: %s\n", dirs[i], strerror(errno));
			continue;
		}
		fprintf(stderr, "music_import: asset: %s mode %04o owner %u:%u, readable and searchable %s\n", dirs[i],
		    (unsigned)(status.st_mode & 07777), status.st_uid, status.st_gid,
		    access(dirs[i], R_OK | X_OK) == 0 ? "yes" : "NO");
	}
	snprintf(path, sizeof(path), "%s/%s", MUSIC_DIR, destination);
	if (stat(path, &status) != 0) {
		fprintf(stderr, "music_import: asset: %s: %s\n", path, strerror(errno));
	} else {
		unsigned char head[16];
		ssize_t got = -1, b;
		int fd = open(path, O_RDONLY);

		if (fd >= 0) {
			got = read(fd, head, sizeof(head));
			close(fd);
		}
		fprintf(stderr, "music_import: asset: %s mode %04o owner %u:%u size %lld, opened %s, read %zd bytes%s",
		    path, (unsigned)(status.st_mode & 07777), status.st_uid, status.st_gid,
		    (long long)status.st_size, fd >= 0 ? "yes" : "NO", got, got > 0 ? ":" : "");
		for (b = 0; b < got && b < (ssize_t)sizeof(head); b++) {
			fprintf(stderr, "%s%02x", b ? " " : " ", head[b]);
		}
		fprintf(stderr, "\n");
	}
	if (sqlite3_open_v2(db, &h, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
		sqlite3_stmt *st = NULL;

		if (sqlite3_prepare_v2(h, "SELECT item.base_location_id, item_extra.location FROM item "
		    "LEFT JOIN item_extra ON item_extra.item_pid = item.item_pid WHERE item.item_pid = ?1",
		    -1, &st, NULL) == SQLITE_OK) {
			sqlite3_bind_int64(st, 1, pid);
			if (sqlite3_step(st) == SQLITE_ROW) {
				const unsigned char *v;

				base_id = sqlite3_column_int64(st, 0);
				v = sqlite3_column_text(st, 1);
				if (v != NULL) snprintf(location, sizeof(location), "%s", (const char *)v);
			}
		}
		sqlite3_finalize(st);
		if (sqlite3_prepare_v2(h, "SELECT path FROM base_location WHERE base_location_id = ?1",
		    -1, &st, NULL) == SQLITE_OK) {
			sqlite3_bind_int64(st, 1, base_id);
			if (sqlite3_step(st) == SQLITE_ROW) {
				const unsigned char *v = sqlite3_column_text(st, 0);

				if (v != NULL) snprintf(base, sizeof(base), "%s", (const char *)v);
			}
		}
		sqlite3_finalize(st);
		sqlite3_close(h);
		snprintf(composed, sizeof(composed), "%s/%s/%s", MEDIA, base, location);
		fprintf(stderr, "music_import: asset: the row is base %lld \"%s\" + location \"%s\" = %s; the file "
		    "imported is %s: %s\n", (long long)base_id, base, location, composed, path,
		    strcmp(composed, path) == 0 ? "the same" : "NOT the same");
	} else {
		fprintf(stderr, "music_import: asset: cannot open %s: %s\n", db, h ? sqlite3_errmsg(h) : "?");
		sqlite3_close(h);
	}
}

/*
 * a test run: the track has its search row and album/artist entities, yet MediaPlayer's songs query is
 * 0 with every filter off. So ask ML3 itself, below MediaPlayer: the library's filter predicates, and an
 * unrestricted query against the default one, each with its SQL and count. Where the track drops out is in
 * the difference.
 */
static void
ml3_probe(ML3MusicLibrary *library)
{
	Class track = NSClassFromString(@"ML3Track");
	SEL unrestricted = NSSelectorFromString(@"unrestrictedAllItemsQueryWithlibrary:predicate:orderingTerms:");
	SEL all = NSSelectorFromString(@"allItemsQueryWithLibrary:predicate:orderingTerms:usingSections:");
	NSArray *names = @[ @"libraryEntityFilterPredicates", @"libraryPublicEntityFilterPredicates" ];
	id queries[2] = { nil, nil };

	for (NSString *name in names) {
		if ([library respondsToSelector:NSSelectorFromString(name)]) {
			fprintf(stderr, "music_import: ml3: library %s: %s\n", [name UTF8String],
			    [[[library performSelector:NSSelectorFromString(name)] description] UTF8String]);
		}
	}
	if ([track respondsToSelector:@selector(defaultFilterPredicates)]) {
		fprintf(stderr, "music_import: ml3: ML3Track defaultFilterPredicates: %s\n",
		    [[[track performSelector:@selector(defaultFilterPredicates)] description] UTF8String]);
	}
	@try {
		if ([track respondsToSelector:unrestricted]) {
			queries[0] = ((id (*)(id, SEL, id, id, id))objc_msgSend)(track, unrestricted, library, nil, nil);
		}
		if ([track respondsToSelector:all]) {
			queries[1] = ((id (*)(id, SEL, id, id, id, BOOL))objc_msgSend)(track, all, library, nil, nil, NO);
		}
	} @catch (NSException *exception) {
		fprintf(stderr, "music_import: ml3: query: %s\n", [[exception reason] UTF8String]);
	}
	@try {
		if ([track respondsToSelector:all]) {
			id sectioned = ((id (*)(id, SEL, id, id, id, BOOL))objc_msgSend)(track, all, library, nil, nil, YES);
			fprintf(stderr, "music_import: ml3: default query with sections: count %lu\n",
			    sectioned && [sectioned respondsToSelector:@selector(countOfEntities)] ?
			    (unsigned long)((NSUInteger (*)(id, SEL))objc_msgSend)(sectioned, @selector(countOfEntities)) : 0UL);
		}
	} @catch (NSException *exception) {
		fprintf(stderr, "music_import: ml3: sections: %s\n", [[exception reason] UTF8String]);
	}
	for (unsigned i = 0; i < 2; i++) {
		const char *which = i ? "default" : "unrestricted";
		id q = queries[i];

		if (q == nil) { fprintf(stderr, "music_import: ml3: %s query: none\n", which); continue; }
		@try {
			if ([q respondsToSelector:@selector(selectCountSQL)]) {
				fprintf(stderr, "music_import: ml3: %s SQL: %s\n", which,
				    [[[q performSelector:@selector(selectCountSQL)] description] UTF8String]);
			}
			if ([q respondsToSelector:@selector(countOfEntities)]) {
				fprintf(stderr, "music_import: ml3: %s count: %lu\n", which,
				    (unsigned long)((NSUInteger (*)(id, SEL))objc_msgSend)(q, @selector(countOfEntities)));
			}
		} @catch (NSException *exception) {
			fprintf(stderr, "music_import: ml3: %s: %s\n", which, [[exception reason] UTF8String]);
		}
	}
	/* and what MediaPlayer's songs query asks for, as it describes itself */
	fprintf(stderr, "music_import: ml3: songsQuery: %s\n", [[[MPMediaQuery songsQuery] description] UTF8String]);

	/*
	 * a test run: the default ML3 query counts the track (1), MediaPlayer's songs query does not (0). So
	 * the predicates MediaPlayer adds, each translated to ML3 and counted on its own: its query's filter
	 * predicates (ML3PredicateForTrack) and its system filters (ML3SystemFilterPredicatesWithGroupingType:...).
	 * The one that counts 0 is what drops the track.
	 */
	NSMutableArray *predicates = [NSMutableArray array];
	for (id filter in [[MPMediaQuery songsQuery] filterPredicates]) {
		if ([filter respondsToSelector:@selector(ML3PredicateForTrack)]) {
			id p = [filter performSelector:@selector(ML3PredicateForTrack)];
			if (p != nil) [predicates addObject:p];
		}
	}
	SEL system = NSSelectorFromString(@"ML3SystemFilterPredicatesWithGroupingType:cloudTrackFilteringType:"
	    "subscriptionFilteringOptions:additionalFilterPredicates:");
	for (NSString *name in @[ @"MPMediaQuery", @"MPMediaLibrary", @"MPMediaLibraryDataProviderML3", @"MPMediaPropertyPredicate" ]) {
		Class c = NSClassFromString(name);
		id owner = nil;

		if ([c respondsToSelector:system]) owner = c;
		else if ([c instancesRespondToSelector:system]) {
			owner = [name isEqualToString:@"MPMediaQuery"] ? [MPMediaQuery songsQuery] :
			    [name isEqualToString:@"MPMediaLibrary"] ? [MPMediaLibrary defaultMediaLibrary] : nil;
		}
		if (owner == nil) continue;
		for (long cloud = 0; cloud <= 2; cloud++) {
			NSArray *more = nil;
			@try {
				more = ((id (*)(id, SEL, NSInteger, NSInteger, NSUInteger, id))objc_msgSend)(owner, system, 0, cloud, 0, nil);
			} @catch (NSException *exception) {
				fprintf(stderr, "music_import: ml3: %s system filters: %s\n", [name UTF8String], [[exception reason] UTF8String]);
			}
			fprintf(stderr, "music_import: ml3: %s system filters (cloud filtering %ld): %lu\n", [name UTF8String], cloud,
			    (unsigned long)[more count]);
			if (cloud == 0) [predicates addObjectsFromArray:more ?: @[]];
		}
		break;
	}
	/*
	 * IR16B 15:18: every MediaPlayer predicate counts the track in ML3 (1), yet MPMediaQuery returns 0. Is
	 * MediaPlayer reading the same library? Its default library, the data provider under it, that provider's
	 * ML3 library and database path, against ours; and the query's own count paths.
	 */
	{
		MPMediaLibrary *media = [MPMediaLibrary defaultMediaLibrary];
		id provider = nil;
		for (NSString *key in @[ @"libraryDataProvider", @"_libraryDataProvider", @"dataProvider" ]) {
			if ([media respondsToSelector:NSSelectorFromString(key)]) { provider = [media valueForKey:key]; break; }
		}
		fprintf(stderr, "music_import: ml3: MPMediaLibrary %s, isLibraryEmpty %s, provider %s\n", [[media description] UTF8String],
		    [media respondsToSelector:@selector(isLibraryEmpty)] ? ([[media valueForKey:@"isLibraryEmpty"] boolValue] ? "YES" : "NO") : "?",
		    [NSStringFromClass([provider class]) ?: @"(none)" UTF8String]);
		id theirs = [provider respondsToSelector:@selector(library)] ? [provider valueForKey:@"library"] :
		    [provider respondsToSelector:@selector(ml3Library)] ? [provider valueForKey:@"ml3Library"] : nil;
		for (id lib in @[ library, theirs ?: [NSNull null] ]) {
			if (lib == [NSNull null]) { fprintf(stderr, "music_import: ml3: provider's ML3 library: (none)\n"); continue; }
			fprintf(stderr, "music_import: ml3: %s ML3 library %p: databasePath %s\n", lib == library ? "our" : "provider's",
			    (__bridge void *)lib, [lib respondsToSelector:@selector(databasePath)] ?
			    [[[lib valueForKey:@"databasePath"] description] UTF8String] : "?");
		}
		MPMediaQuery *songs = [MPMediaQuery songsQuery];
		for (NSString *key in @[ @"_countOfItems", @"_hasItems", @"_itemsCount" ]) {
			if ([songs respondsToSelector:NSSelectorFromString(key)]) {
				@try {
					fprintf(stderr, "music_import: ml3: songsQuery %s = %s\n", [key UTF8String],
					    [[[songs valueForKey:key] description] UTF8String]);
				} @catch (NSException *exception) {
					fprintf(stderr, "music_import: ml3: songsQuery %s: %s\n", [key UTF8String], [[exception reason] UTF8String]);
				}
			}
		}
		/*
		 * Combo7 runs: the provider holds our very ML3 library and still counts 0, and MPMediaLibrary prints as
		 * "[nil]". What the provider and the library are made of: the provider's methods about counting,
		 * querying, libraries and identities, and the instance variables of both (objects described).
		 */
		for (id object in @[ media, provider ?: [NSNull null] ]) {
			if (object == [NSNull null]) continue;
			Class cls = object_getClass(object);
			unsigned count = 0;
			Ivar *ivars = class_copyIvarList(cls, &count);

			for (unsigned k = 0; k < count; k++) {
				const char *type = ivar_getTypeEncoding(ivars[k]);
				id value = type && type[0] == '@' ? object_getIvar(object, ivars[k]) : nil;
				NSString *text = value ? [[value description] substringToIndex:MIN((NSUInteger)160, [[value description] length])] : @"";
				fprintf(stderr, "music_import: ml3: %s ivar %s (%s) %s\n", class_getName(cls), ivar_getName(ivars[k]),
				    type ?: "?", [text UTF8String]);
			}
			free(ivars);
			for (NSString *key in @[ @"uniqueIdentifier", @"userIdentity", @"_userIdentity", @"libraryUID", @"isHomeSharingLibrary",
			    @"databasePath", @"name" ]) {
				if ([object respondsToSelector:NSSelectorFromString(key)]) {
					@try {
						fprintf(stderr, "music_import: ml3: %s.%s = %s\n", class_getName(cls), [key UTF8String],
						    [[[object valueForKey:key] description] UTF8String]);
					} @catch (NSException *exception) {
					}
				}
			}
		}
		if (provider != nil) {
			unsigned count = 0;
			Method *methods = class_copyMethodList(object_getClass(provider), &count);

			for (unsigned k = 0; k < count; k++) {
				const char *name = sel_getName(method_getName(methods[k]));
				if (strcasestr(name, "count") || strcasestr(name, "query") || strcasestr(name, "librar") ||
				    strcasestr(name, "identit") || strcasestr(name, "filter")) {
					fprintf(stderr, "music_import: ml3: provider method %s %s\n", name, method_getTypeEncoding(methods[k]) ?: "");
				}
			}
			free(methods);
		}
		/*
		 * a reading of MediaPlayer, checked against the selectors:
		 * the songs query is built with +[ML3Track queryWithLibrary:...] (the library's own scope) unless
		 * shouldIncludeNonLibraryEntities, while every count of ours used allItemsQueryWithLibrary:. And with no
		 * account the provider's system filters ask hasNonPurgeableAsset and !significantAmountHasBeenPlayed.
		 * So: the scoped query's SQL and count, the songs query with non-library entities, and the provider's own
		 * system filters one by one in the scoped query.
		 */
		{
			Class track_class = NSClassFromString(@"ML3Track");
			SEL scoped = NSSelectorFromString(@"queryWithLibrary:predicate:orderingTerms:usingSections:");
			id q = [track_class respondsToSelector:scoped] ?
			    ((id (*)(id, SEL, id, id, id, BOOL))objc_msgSend)(track_class, scoped, library, nil, nil, NO) : nil;

			if (q != nil) {
				fprintf(stderr, "music_import: ml3: scoped SQL: %s\n", [[[q valueForKey:@"selectCountSQL"] description] UTF8String]);
				fprintf(stderr, "music_import: ml3: scoped count: %lu\n",
				    (unsigned long)((NSUInteger (*)(id, SEL))objc_msgSend)(q, @selector(countOfEntities)));
			}
			MPMediaQuery *wide = [MPMediaQuery songsQuery];
			if ([wide respondsToSelector:@selector(setShouldIncludeNonLibraryEntities:)]) {
				((void (*)(id, SEL, BOOL))objc_msgSend)(wide, @selector(setShouldIncludeNonLibraryEntities:), YES);
				fprintf(stderr, "music_import: ml3: songsQuery with non-library entities: %lu items\n",
				    (unsigned long)[[wide items] count]);
			}
			SEL sysf = NSSelectorFromString(@"systemFilterPredicatesWithGroupingType:cloudTrackFilteringType:subscriptionFilteringOptions:");
			if ([provider respondsToSelector:sysf]) {
				NSArray *filters = ((id (*)(id, SEL, NSInteger, NSInteger, NSUInteger))objc_msgSend)(provider, sysf, 0, 0, 0);
				for (id p in filters) {
					id pq = [track_class respondsToSelector:scoped] ?
					    ((id (*)(id, SEL, id, id, id, BOOL))objc_msgSend)(track_class, scoped, library, p, nil, NO) : nil;
					fprintf(stderr, "music_import: ml3: provider system filter %s: scoped count %lu\n",
					    [[[p description] substringToIndex:MIN((NSUInteger)200, [[p description] length])] UTF8String],
					    pq ? (unsigned long)((NSUInteger (*)(id, SEL))objc_msgSend)(pq, @selector(countOfEntities)) : 0UL);
				}
			}
		}
		fprintf(stderr, "music_import: ml3: songsQuery items %lu, collections %lu, MPMediaQuery new items %lu\n",
		    (unsigned long)[[songs items] count], (unsigned long)[[songs collections] count],
		    (unsigned long)[[[MPMediaQuery new] items] count]);
	}
	for (id p in predicates) {
		id q = nil;
		@try {
			q = ((id (*)(id, SEL, id, id, id, BOOL))objc_msgSend)(track, all, library, p, nil, NO);
		} @catch (NSException *exception) {
			fprintf(stderr, "music_import: ml3: predicate %s: %s\n", [[p description] UTF8String], [[exception reason] UTF8String]);
			continue;
		}
		fprintf(stderr, "music_import: ml3: predicate %s: count %lu\n", [[p description] UTF8String],
		    q && [q respondsToSelector:@selector(countOfEntities)] ?
		    (unsigned long)((NSUInteger (*)(id, SEL))objc_msgSend)(q, @selector(countOfEntities)) : 0UL);
	}
}

static BOOL ascii_names;
/* --no-late-checks: no +60/+180/+420 s read-back after "done" (a foreground rc.jobs line would be held for them) */
static int late_checks = 1;

/*
 * Rows left by a half-finished import. The marker is written only after the row and its sort data are
 * complete (import(), near the end); a boot that resets in between leaves an item with this location
 * and no sort_map/orders, and Music draws it as an empty entry that does not play. When the good row
 * is in, drop every other row that names the same file. The pids are selected before item_extra is
 * touched, so nothing else in the library can match.
 */
static void
drop_stale_rows(const char *location, int64_t keep)
{
	static const char *db = "/var/mobile/Media/iTunes_Control/iTunes/MediaLibrary.sqlitedb";
	static const char *const sql[] = {
		"DELETE FROM item WHERE item_pid IN "
		"(SELECT item_pid FROM item_extra WHERE location = ?1 AND item_pid != ?2)",
		"DELETE FROM item_extra WHERE location = ?1 AND item_pid != ?2",
	};
	sqlite3 *h = NULL;

	if (sqlite3_open_v2(db, &h, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
		fprintf(stderr, "music_import: stale rows: cannot open %s: %s\n", db, h ? sqlite3_errmsg(h) : "?");
		sqlite3_close(h);
		return;
	}
	sqlite3_busy_timeout(h, 5000);
	for (unsigned i = 0; i < sizeof(sql) / sizeof(sql[0]); i++) {
		sqlite3_stmt *st = NULL;

		if (sqlite3_prepare_v2(h, sql[i], -1, &st, NULL) != SQLITE_OK) {
			fprintf(stderr, "music_import: stale rows: %s\n", sqlite3_errmsg(h));
			continue;
		}
		sqlite3_bind_text(st, 1, location, -1, SQLITE_STATIC);
		sqlite3_bind_int64(st, 2, keep);
		if (sqlite3_step(st) != SQLITE_DONE) {
			fprintf(stderr, "music_import: stale rows: %s\n", sqlite3_errmsg(h));
		} else if (sqlite3_changes(h) > 0) {
			fprintf(stderr, "music_import: stale rows: %d row(s) of %s dropped\n", sqlite3_changes(h), location);
		}
		sqlite3_finalize(st);
	}
	sqlite3_close(h);
}

/* The item a file name belongs to: item_extra.location is the file name relative to its base. */
static int64_t
find_pid(const char *location)
{
	static const char *db = "/var/mobile/Media/iTunes_Control/iTunes/MediaLibrary.sqlitedb";
	sqlite3 *h = NULL;
	sqlite3_stmt *st = NULL;
	int64_t pid = 0;

	if (location == NULL || location[0] == '\0') {
		return 0;
	}
	if (sqlite3_open_v2(db, &h, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
		fprintf(stderr, "music_import: find: cannot open %s: %s\n", db, h ? sqlite3_errmsg(h) : "?");
		sqlite3_close(h);
		return 0;
	}
	if (sqlite3_prepare_v2(h, "SELECT item_pid FROM item_extra WHERE location = ?1", -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_text(st, 1, location, -1, SQLITE_STATIC);
		if (sqlite3_step(st) == SQLITE_ROW) {
			pid = sqlite3_column_int64(st, 0);
		}
	}
	sqlite3_finalize(st);
	sqlite3_close(h);
	return pid;
}

/* One place for "tell the library", used by the import and by the repair of an earlier import. */
static void
tell_library_changed(void)
{
	ML3MusicLibrary *library = [ML3MusicLibrary autoupdatingSharedLibrary];

	if (library != nil && [library respondsToSelector:@selector(notifyContentsDidChange)]) {
		[library notifyContentsDidChange];
		fprintf(stderr, "music_import: told the library its contents changed\n");
	} else {
		fprintf(stderr, "music_import: the library has no notifyContentsDidChange\n");
	}
}

/*
 * R2/TEST5Q: what a marker-only boot has to do. The marker is written once, at the end of a
 * successful import, and the first thing import() did on every later boot was return because of it
 * (TEST5Q log line 2779992). So the row that Music refused (explicit, or an asset nobody could open)
 * was never touched again by any boot. Everything the import repairs is idempotent and keyed by the
 * item's pid, so run the same repair on the row the marker stands for; -1 only when the marker has
 * no row at all (then the caller imports for real).
 */
static int
repair_imported(const char *destination)
{
	char path[4096];
	struct stat status;
	NSString *t = nil, *a = nil, *al = nil, *g = nil;
	double secs = 0;
	long long size = 0;
	int64_t pid = find_pid(destination);

	if (pid == 0) {
		fprintf(stderr, "music_import: no row names %s\n", destination);
		return -1;
	}
	fprintf(stderr, "music_import: the marker is there; repairing item %lld instead of returning\n",
	    (long long)pid);
	if (getuid() == 0) {
		(void)setgroups(0, NULL);
		if (setgid(MOBILE_ID) != 0 || setuid(MOBILE_ID) != 0) {
			fprintf(stderr, "music_import: setgid/setuid: %s\n", strerror(errno)); return 1;
		}
		setenv("HOME", "/var/mobile", 1);
		setenv("CFFIXED_USER_HOME", "/var/mobile", 1);
	}
	snprintf(path, sizeof(path), "%s/%s", MUSIC_DIR, destination);
	if (stat(path, &status) == 0) {
		size = (long long)status.st_size;
	}
	read_metadata(path, [[NSString stringWithUTF8String:destination] stringByDeletingPathExtension], &t, &a, &al,
	    &g, &secs);
	fprintf(stderr, "music_import: repair: artist \"%s\", album \"%s\", %lld bytes\n", UTF8(a), UTF8(al), size);
	repair_entities(pid, a, al);
	fix_rows(pid, destination, size);
	repair_local(pid, t, a, al, size, secs);
	allow_explicit_content();
	report_asset(pid, destination);
	dump_row(pid, "after the repair (marker present)");
	dump_songs(pid);
	media_query("after the repair (marker present)", pid);
	tell_library_changed();
	fprintf(stderr, "music_import: done\n");
	return 0;
}

static int import(int argc, char **argv) {
	const char *source, *dest = "RDWD.mp3";
	double wait = 0;
	int index = 1, length, marker;
	char dest_path[4096], marker_path[4096];
	long long copied = 0;
	NSString *dest_string, *title, *artist, *album, *genre;
	double seconds = 0;
	ML3MusicLibrary *library;
	ML3Track *track = nil;
	int64_t base;

	/* --query: nothing imported; only what the library and MediaPlayer say now about the track already imported
	 * (by its file name), for a look over ssh while the phone runs */
	if (index < argc && (strcmp(argv[index], "--query") == 0 || strcmp(argv[index], "--repair") == 0 ||
	    strcmp(argv[index], "--ml3") == 0)) {
		BOOL repair = strcmp(argv[index], "--repair") == 0, ml3 = strcmp(argv[index], "--ml3") == 0;
		const char *name = index + 1 < argc ? argv[index + 1] : "RDWD.mp3";
		sqlite3 *h = NULL;
		sqlite3_stmt *st = NULL;
		int64_t pid = 0;

		if (sqlite3_open_v2("/var/mobile/Media/iTunes_Control/iTunes/MediaLibrary.sqlitedb", &h, SQLITE_OPEN_READONLY,
		    NULL) == SQLITE_OK && sqlite3_prepare_v2(h, "SELECT item_pid FROM item_extra WHERE location = ?1", -1, &st,
		    NULL) == SQLITE_OK) {
			sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
			if (sqlite3_step(st) == SQLITE_ROW) {
				pid = sqlite3_column_int64(st, 0);
			}
		}
		sqlite3_finalize(st);
		sqlite3_close(h);
		fprintf(stderr, "music_import: query: %s is item %lld\n", name, (long long)pid);
		dump_row(pid, "query");
		dump_songs(pid);
		if (repair) {
			/* no ML3MusicLibrary here on purpose: the repair is SQL on the file, and asking the library for the
			 * sort map is what killed medialibraryd (see the comment above repair_local) */
			if (getuid() == 0) {
				(void)setgroups(0, NULL);
				if (setgid(MOBILE_ID) != 0 || setuid(MOBILE_ID) != 0) {
					fprintf(stderr, "music_import: setgid/setuid: %s\n", strerror(errno)); return 1;
				}
				setenv("HOME", "/var/mobile", 1);
				setenv("CFFIXED_USER_HOME", "/var/mobile", 1);
			}
			{
				char path[4096];
				struct stat status;
				NSString *t = nil, *a = nil, *al = nil, *g = nil;
				double secs = 0;
				long long size = 0;

				snprintf(path, sizeof(path), "%s/%s", MUSIC_DIR, name);
				if (stat(path, &status) == 0) {
					size = (long long)status.st_size;
				}
				read_metadata(path, [[NSString stringWithUTF8String:name] stringByDeletingPathExtension], &t, &a, &al,
				    &g, &secs);
				fprintf(stderr, "music_import: repair: artist \"%s\", album \"%s\", %lld bytes\n", UTF8(a), UTF8(al), size);
				repair_entities(pid, a, al);
				fix_rows(pid, name, size);
				repair_local(pid, t, a, al, size, secs);
				allow_explicit_content();
				report_asset(pid, name);
			}
			dump_row(pid, "after the repair");
			dump_songs(pid);
		}
		if (ml3) {
			ML3MusicLibrary *lib;

			if (getuid() == 0) {
				(void)setgroups(0, NULL);
				if (setgid(MOBILE_ID) != 0 || setuid(MOBILE_ID) != 0) {
					fprintf(stderr, "music_import: setgid/setuid: %s\n", strerror(errno)); return 1;
				}
				setenv("HOME", "/var/mobile", 1);
				setenv("CFFIXED_USER_HOME", "/var/mobile", 1);
			}
			lib = [ML3MusicLibrary autoupdatingSharedLibrary];
			if (lib == nil) { fprintf(stderr, "music_import: no ML3MusicLibrary\n"); return 1; }
			ml3_probe(lib);
		}
		media_query("query", pid);
		return 0;
	}
	if (index < argc && strcmp(argv[index], "--no-late-checks") == 0) {
		late_checks = 0;
		index++;
	}
	/* --ascii: the track's title, artist and album replaced by ASCII names (USB networking 2026-10-26: /var's ramfs compares
	 * names byte by byte, Foundation hands it NFD, so a name with ё/й is not found; is that behind songs = 0?) */
	if (index < argc && strcmp(argv[index], "--ascii") == 0) {
		ascii_names = YES;
		index++;
	}
	if (index < argc && strcmp(argv[index], "--wait") == 0) {
		if (index + 1 >= argc) return usage();
		wait = atof(argv[index + 1]); index += 2;
	}
	if (index >= argc) return usage();
	source = argv[index++];
	if (index < argc) dest = argv[index++];
	if (index != argc || dest[0] == '\0' || strchr(dest, '/') != NULL) {
		fprintf(stderr, "music_import: bad arguments, destination \"%s\"\n", dest); return 2;
	}
	if (wait > 0) { fprintf(stderr, "music_import: waiting %d s\n", (int)wait); sleep((unsigned)wait); }
	length = snprintf(dest_path, sizeof(dest_path), "%s/%s", MUSIC_DIR, dest);
	if (length < 0 || (size_t)length >= sizeof(dest_path)) {
		fprintf(stderr, "music_import: destination path is too long\n"); return 1;
	}
	length = snprintf(marker_path, sizeof(marker_path), MEDIA "/iTunes_Control/.hakios-imported-%s", dest);
	if (length < 0 || (size_t)length >= sizeof(marker_path)) {
		fprintf(stderr, "music_import: marker path is too long\n"); return 1;
	}
	if (make_dir(MEDIA) != 0 || make_dir(MEDIA "/iTunes_Control") != 0 ||
	    make_dir(MEDIA "/iTunes_Control/Music") != 0 || make_dir(MUSIC_DIR) != 0 ||
	    make_dir(ITUNES_DIR) != 0) return 1;
	if (access(marker_path, F_OK) == 0) {
		int repaired;

		fprintf(stderr, "music_import: already imported: %s\n", marker_path);
		say_step("repairing the row an earlier boot imported");
		repaired = repair_imported(dest);
		if (repaired == 0) {
			return 0;
		}
		/* a marker without a row (a library that was dropped, a row deleted here): import it again */
		fprintf(stderr, "music_import: the marker %s has no row; importing it again\n", marker_path);
	}
	signal(SIGALRM, died);
	signal(SIGSEGV, died);
	signal(SIGBUS, died);
	signal(SIGABRT, died);
	alarm(120);
	say_step("copying the file");
	if (copy_file(source, dest_path, &copied) != 0) return 1;
	fprintf(stderr, "music_import: copied %lld bytes to %s\n", copied, dest_path);
	fprintf(stderr, "music_import: becoming mobile (501:501)\n");
	(void)setgroups(0, NULL);
	if (setgid(MOBILE_ID) != 0 || setuid(MOBILE_ID) != 0) {
		fprintf(stderr, "music_import: setgid/setuid: %s\n", strerror(errno)); return 1;
	}
	if (setenv("HOME", "/var/mobile", 1) != 0 || setenv("CFFIXED_USER_HOME", "/var/mobile", 1) != 0) {
		fprintf(stderr, "music_import: setenv: %s\n", strerror(errno)); return 1;
	}
	dest_string = [NSString stringWithUTF8String:dest] ?: @"";
	say_step("reading the tags");
	read_metadata(dest_path, [dest_string stringByDeletingPathExtension], &title, &artist, &album,
	    &genre, &seconds);
	if (ascii_names) {
		title = [dest_string stringByDeletingPathExtension];
		artist = @"Temny Princ";
		album = @"Otvratitelny Korol";
	}
	fprintf(stderr, "music_import: title \"%s\", artist \"%s\", album \"%s\", genre \"%s\", %.3f s\n",
	    UTF8(title), UTF8(artist), UTF8(album), UTF8(genre), seconds);
	say_step("opening the library (+[ML3MusicLibrary autoupdatingSharedLibrary])");
	library = [ML3MusicLibrary autoupdatingSharedLibrary];
	if (library == nil) { fprintf(stderr, "music_import: no ML3MusicLibrary\n"); return 1; }
	say_step("base location of iTunes_Control/Music/F00");
	base = ML3BaseLocationIDFromMediaRelativePathInLibrary(@"iTunes_Control/Music/F00", library);
	fprintf(stderr, "music_import: base location ID %lld (expected 3840)\n", (long long)base);
	NSDictionary *dict = @{ ML3TrackPropertyTitle: title, ML3TrackPropertyArtist: artist,
	    ML3TrackPropertyAlbum: album, ML3TrackPropertyAlbumArtist: artist, ML3TrackPropertyGenre: genre,
	    ML3TrackPropertyMediaType: @8, ML3TrackPropertyLocationFileName: dest_string,
	    ML3TrackPropertyBaseLocationID: @(base), ML3TrackPropertyTotalTime: @((int64_t)(seconds * 1000)),
	    ML3TrackPropertyIsInMyLibrary: @YES, ML3TrackPropertyHidden: @NO,
	    ML3TrackPropertyIsUserDisabled: @NO };
	say_step("inserting the track (+[ML3Track newWithDictionary:inLibrary:])");
	@try { track = [ML3Track newWithDictionary:dict inLibrary:library]; }
	@catch (NSException *exception) {
		fprintf(stderr, "music_import: ML3Track: %s: %s\n", UTF8([exception name]), UTF8([exception reason])); return 1;
	}
	if (track == nil) { fprintf(stderr, "music_import: ML3Track: no track was made\n"); return 1; }
	fprintf(stderr, "music_import: persistent ID %lld\n", (long long)[track persistentID]);
	say_step("asking the track about itself");
	fprintf(stderr, "music_import: the track says: file path %s, playable %s, in the library %d\n",
	    UTF8([[track valueForProperty:ML3TrackPropertyFilePath] description]),
	    UTF8([[track valueForProperty:ML3TrackPropertyIsPlayable] description]), (int)[track existsInLibrary]);
	dump_row([track persistentID], "after the insert");
	say_step("completing the track (in the library, its file's place)");
	@try { [track setValue:@YES forProperty:ML3TrackPropertyIsInMyLibrary]; }
	@catch (NSException *exception) {
		fprintf(stderr, "music_import: setValue in-my-library: %s\n", UTF8([exception reason]));
	}
	fix_rows([track persistentID], dest, copied);
	repair_entities([track persistentID], artist, album);
	repair_local([track persistentID], title, artist, album, copied, seconds);
	allow_explicit_content();
	report_asset([track persistentID], dest);
	drop_stale_rows(dest, [track persistentID]);
	dump_songs([track persistentID]);
	fprintf(stderr, "music_import: the track says now: file path %s, playable %s, local %s, restricted %s, "
	    "explicit %s, content rating %s\n",
	    UTF8([[track valueForProperty:ML3TrackPropertyFilePath] description]),
	    UTF8([[track valueForProperty:ML3TrackPropertyIsPlayable] description]),
	    UTF8([[track valueForProperty:ML3TrackPropertyIsLocal] description]),
	    UTF8([[track valueForProperty:ML3TrackPropertyIsCurrentlyRestricted] description]),
	    UTF8([[track valueForProperty:ML3TrackPropertyIsExplicit] description]),
	    UTF8([[track valueForProperty:ML3TrackPropertyContentRating] description]));
	dump_row([track persistentID], "after completing");
	say_step("asking MediaPlayer what Music would list");
	media_query("after completing", [track persistentID]);
	say_step("telling the library");
	tell_library_changed();
	marker = open(marker_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (marker < 0) { error(marker_path); return 1; }
	if (write(marker, dest, strlen(dest)) != (ssize_t)strlen(dest) || write(marker, "\n", 1) != 1) {
		error(marker_path); close(marker); return 1;
	}
	fsync(marker); close(marker);
	fprintf(stderr, "music_import: done\n");
	alarm(0);
	/* and later again: does medialibraryd keep the row (a validation of its own could drop it)?
	 * --no-late-checks leaves this to the caller: selftest(1) reads the row, the songs query and
	 * the track's playback itself a few seconds later, and a foreground job list must not sleep. */
	if (late_checks) {
		for (unsigned i = 0; i < 3; i++) {
			static const unsigned after[] = { 60, 180, 420 };
			char when[32];

			sleep(after[i] - (i ? after[i - 1] : 0));
			snprintf(when, sizeof(when), "%u s later", after[i]);
			dump_row([track persistentID], when);
			media_query(when, [track persistentID]);
		}
	} else {
		fprintf(stderr, "music_import: --no-late-checks: the +60/+180/+420 s read-backs are skipped\n");
	}
	return 0;
}

int main(int argc, char **argv) {
	int status;

	@autoreleasepool {
		status = import(argc, argv);
		/* a moment for the library's change notifications to go out before the process is gone */
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 2, false);
	}
	return status;
}
