/*
 * music_import: put one MP3 into the Music library so the stock Music app lists it; usage:
 * music_import [--wait SECONDS] SOURCE.mp3 [DEST_NAME] (defaults 0 and RDWD.mp3). Root copies
 * and hands the file to mobile, which reads the tags and inserts an ML3Track; a marker makes a
 * rerun a no-op.
 */
#import <Foundation/Foundation.h>
#import <MediaPlayer/MediaPlayer.h>
#import <objc/message.h>
#include <signal.h>
#include <sqlite3.h>
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
#define UTF8(s) ((s) ? [(s) UTF8String] : "(none)")

static int usage(void) {
	fprintf(stderr, "music_import: usage: music_import [--wait SECONDS] SOURCE.mp3 [DEST_NAME] | --query [NAME] | --repair [NAME] | --ml3 [NAME]\n"); return 2;
}
static int error(const char *path) {
	fprintf(stderr, "music_import: %s: %s\n", path, strerror(errno)); return -1;
}
/* One directory, made 0755 and given to mobile when it is not there yet. */
static int make_dir(const char *path) {
	struct stat status;

	if (stat(path, &status) == 0) {
		if (S_ISDIR(status.st_mode)) return 0;
		fprintf(stderr, "music_import: %s is not a directory\n", path); return -1;
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
fix_rows(int64_t pid, long long file_size)
{
	static const char *db = "/var/mobile/Media/iTunes_Control/iTunes/MediaLibrary.sqlitedb";
	const char *statements[] = {
		"INSERT OR IGNORE INTO base_location (base_location_id, path) VALUES (3840, 'iTunes_Control/Music/F00')",
		"UPDATE item SET in_my_library = 1, date_added = CASE date_added WHEN 0 THEN ?2 ELSE date_added END WHERE item_pid = ?1",
		"UPDATE item_extra SET file_size = ?3 WHERE item_pid = ?1 AND file_size = 0",
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
 * IR16B (15:18): every MediaPlayer predicate counts the track in ML3, yet MediaPlayer's own count is 0, and the
 * track's title_order (and every *_order) is 0 while updateSortMap says NO: its names never reached the sort map
 * (ML3's import session does that, _prepareTrackSortData; a plain ML3Track insert does not). Songs are listed in
 * title order with sections, which ties a track to its sort_map entry. So: the names into the sort map by ML3's
 * own call, the orders and sections and search keys from it, and the WAL checkpointed (the main file was 4 KB and
 * everything else in the WAL) so that every connection sees the same database.
 */
static void
repair_sort(ML3MusicLibrary *library, int64_t pid, NSString *title, NSString *artist, NSString *album)
{
	static const char *db = "/var/mobile/Media/iTunes_Control/iTunes/MediaLibrary.sqlitedb";
	static const char *orders =
	    "UPDATE item SET "
	    "title_order = IFNULL((SELECT name_order FROM sort_map WHERE name = ?2), 0), "
	    "title_order_section = IFNULL((SELECT name_section FROM sort_map WHERE name = ?2), 0), "
	    "item_artist_order = IFNULL((SELECT name_order FROM sort_map WHERE name = ?3), 0), "
	    "item_artist_order_section = IFNULL((SELECT name_section FROM sort_map WHERE name = ?3), 0), "
	    "album_artist_order = IFNULL((SELECT name_order FROM sort_map WHERE name = ?3), 0), "
	    "album_artist_order_section = IFNULL((SELECT name_section FROM sort_map WHERE name = ?3), 0), "
	    "album_order = IFNULL((SELECT name_order FROM sort_map WHERE name = ?4), 0), "
	    "album_order_section = IFNULL((SELECT name_section FROM sort_map WHERE name = ?4), 0) "
	    "WHERE item_pid = ?1";
	static const char *search =
	    "UPDATE item_search SET "
	    "search_title = IFNULL((SELECT name_order FROM sort_map WHERE name = ?2), 0), "
	    "search_artist = IFNULL((SELECT name_order FROM sort_map WHERE name = ?3), 0), "
	    "search_album_artist = IFNULL((SELECT name_order FROM sort_map WHERE name = ?3), 0), "
	    "search_album = IFNULL((SELECT name_order FROM sort_map WHERE name = ?4), 0) "
	    "WHERE item_pid = ?1";
	NSArray *names = @[ title ?: @"", artist ?: @"", album ?: @"" ];
	sqlite3 *h = NULL;

	if (pid == 0) return;
	for (id target in @[ library, NSClassFromString(@"ML3SortMap") ?: [NSNull null] ]) {
		if (target == [NSNull null]) continue;
		if ([target respondsToSelector:@selector(insertStringsIntoSortMap:)]) {
			@try {
				fprintf(stderr, "music_import: sort: %s insertStringsIntoSortMap says %d\n", [NSStringFromClass([target class]) UTF8String],
				    (int)((BOOL (*)(id, SEL, id))objc_msgSend)(target, @selector(insertStringsIntoSortMap:), names));
			} @catch (NSException *exception) {
				fprintf(stderr, "music_import: sort: insertStringsIntoSortMap: %s\n", [[exception reason] UTF8String]);
			}
			break;
		}
		fprintf(stderr, "music_import: sort: %s has no insertStringsIntoSortMap:\n", [NSStringFromClass([target class]) UTF8String]);
	}
	if (sqlite3_open_v2(db, &h, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
		fprintf(stderr, "music_import: sort: cannot open %s: %s\n", db, h ? sqlite3_errmsg(h) : "?");
		sqlite3_close(h);
		return;
	}
	sqlite3_busy_timeout(h, 5000);
	{
		sqlite3_stmt *st = NULL;
		long long rows = -1;
		if (sqlite3_prepare_v2(h, "SELECT COUNT(*) FROM sort_map WHERE name IN (?1, ?2, ?3)", -1, &st, NULL) == SQLITE_OK) {
			for (int i = 0; i < 3; i++) sqlite3_bind_text(st, i + 1, [names[i] UTF8String], -1, SQLITE_TRANSIENT);
			if (sqlite3_step(st) == SQLITE_ROW) rows = sqlite3_column_int64(st, 0);
		}
		sqlite3_finalize(st);
		fprintf(stderr, "music_import: sort: %lld of the 3 names in sort_map\n", rows);
	}
	for (int k = 0; k < 2; k++) {
		sqlite3_stmt *st = NULL;
		int rc = sqlite3_prepare_v2(h, k ? search : orders, -1, &st, NULL);

		if (rc == SQLITE_OK) {
			sqlite3_bind_int64(st, 1, pid);
			for (int i = 0; i < 3; i++) sqlite3_bind_text(st, i + 2, [names[i] UTF8String], -1, SQLITE_TRANSIENT);
			rc = sqlite3_step(st);
		}
		fprintf(stderr, "music_import: sort: %s: %s (%d changed)\n", k ? "search keys" : "orders",
		    rc == SQLITE_DONE ? "done" : sqlite3_errmsg(h), sqlite3_changes(h));
		sqlite3_finalize(st);
	}
	{
		int log = -1, done = -1;
		int rc = sqlite3_wal_checkpoint_v2(h, NULL, SQLITE_CHECKPOINT_TRUNCATE, &log, &done);
		fprintf(stderr, "music_import: sort: checkpoint (truncate): %s, %d frames, %d done\n",
		    rc == SQLITE_OK ? "ok" : sqlite3_errmsg(h), log, done);
	}
	sqlite3_close(h);
	if ([library respondsToSelector:@selector(notifyContentsDidChange)]) [library notifyContentsDidChange];
}

/*
 * Live run 2026-10-25 13:5x: the track's item row is complete but item_search has no row and title_order is 0.
 * The search keys are sort_map name orders, which ML3 fills in its sort map update; a plain ML3Track insert
 * skipped it. So: ask the library for the update, then the same item_search insert ML3 itself carries
 * (strings of MusicLibrary), for every column, for tracks still missing a row. Idempotent.
 */
static void
repair_search(ML3MusicLibrary *library)
{
	static const char *db = "/var/mobile/Media/iTunes_Control/iTunes/MediaLibrary.sqlitedb";
	static const char *sql =
	    "INSERT OR IGNORE INTO item_search (item_pid, search_title, search_album, search_artist, search_album_artist) "
	    "SELECT e.item_pid, IFNULL((SELECT name_order FROM sort_map WHERE name = e.title), 0), "
	    "IFNULL((SELECT s.name_order FROM sort_map s JOIN album a ON s.name = a.album WHERE a.album_pid = i.album_pid), 0), "
	    "IFNULL((SELECT s.name_order FROM sort_map s JOIN item_artist r ON s.name = r.item_artist "
	    "WHERE r.item_artist_pid = i.item_artist_pid), 0), "
	    "IFNULL((SELECT s.name_order FROM sort_map s JOIN album_artist r ON s.name = r.album_artist "
	    "WHERE r.album_artist_pid = i.album_artist_pid), 0) "
	    "FROM item_extra e JOIN item i ON i.item_pid = e.item_pid";
	sqlite3 *h = NULL;
	char *message = NULL;

	/* live 14:03: updateSortMap said NO and title_order stayed 0; MediaLibrary.migrator (which our boot never
	 * runs) validates the database, and the ordering languages come before any sort order */
	if ([library respondsToSelector:@selector(updateOrderingLanguagesForCurrentLanguage)]) {
		[library updateOrderingLanguagesForCurrentLanguage];
		fprintf(stderr, "music_import: repair: ordering languages updated\n");
	}
	if ([library respondsToSelector:@selector(validateDatabase)]) {
		fprintf(stderr, "music_import: repair: validateDatabase says %d\n", (int)[library validateDatabase]);
	} else fprintf(stderr, "music_import: repair: the library has no validateDatabase\n");
	if ([library respondsToSelector:@selector(setNeedsToUpdateSortMap:)]) {
		[library setNeedsToUpdateSortMap:YES];
	}
	if ([library respondsToSelector:@selector(updateSortMap)]) {
		fprintf(stderr, "music_import: repair: updateSortMap says %d\n", (int)[library updateSortMap]);
	} else fprintf(stderr, "music_import: repair: the library has no updateSortMap\n");
	if (sqlite3_open_v2(db, &h, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK) {
		fprintf(stderr, "music_import: repair: cannot open %s: %s\n", db, h ? sqlite3_errmsg(h) : "?");
		sqlite3_close(h);
		return;
	}
	sqlite3_busy_timeout(h, 5000);
	if (sqlite3_exec(h, sql, NULL, NULL, &message) == SQLITE_OK) {
		fprintf(stderr, "music_import: repair: item_search rows added %d\n", sqlite3_changes(h));
	} else fprintf(stderr, "music_import: repair: item_search: %s\n", message ? message : "?");
	sqlite3_free(message);
	sqlite3_close(h);
	if ([library respondsToSelector:@selector(notifyContentsDidChange)]) {
		[library notifyContentsDidChange];
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
		if (repair) {
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
			{
				char path[4096];
				NSString *t = nil, *a = nil, *al = nil, *g = nil;
				double secs = 0;

				snprintf(path, sizeof(path), "%s/%s", MUSIC_DIR, name);
				read_metadata(path, [[NSString stringWithUTF8String:name] stringByDeletingPathExtension], &t, &a, &al,
				    &g, &secs);
				fprintf(stderr, "music_import: repair: artist \"%s\", album \"%s\"\n", UTF8(a), UTF8(al));
				repair_entities(pid, a, al);
				repair_search(lib);
				repair_sort(lib, pid, t, a, al);
			}
			dump_row(pid, "after the repair");
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
		fprintf(stderr, "music_import: already imported: %s\n", marker_path); return 0;
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
	fix_rows([track persistentID], copied);
	repair_entities([track persistentID], artist, album);
	repair_search(library);
	repair_sort(library, [track persistentID], title, artist, album);
	fprintf(stderr, "music_import: the track says now: file path %s, playable %s, local %s, restricted %s\n",
	    UTF8([[track valueForProperty:ML3TrackPropertyFilePath] description]),
	    UTF8([[track valueForProperty:ML3TrackPropertyIsPlayable] description]),
	    UTF8([[track valueForProperty:ML3TrackPropertyIsLocal] description]),
	    UTF8([[track valueForProperty:ML3TrackPropertyIsCurrentlyRestricted] description]));
	dump_row([track persistentID], "after completing");
	say_step("asking MediaPlayer what Music would list");
	media_query("after completing", [track persistentID]);
	say_step("telling the library");
	if ([library respondsToSelector:@selector(notifyContentsDidChange)]) {
		[library notifyContentsDidChange];
		fprintf(stderr, "music_import: told the library its contents changed\n");
	} else fprintf(stderr, "music_import: the library has no notifyContentsDidChange\n");
	marker = open(marker_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (marker < 0) { error(marker_path); return 1; }
	if (write(marker, dest, strlen(dest)) != (ssize_t)strlen(dest) || write(marker, "\n", 1) != 1) {
		error(marker_path); close(marker); return 1;
	}
	fsync(marker); close(marker);
	fprintf(stderr, "music_import: done\n");
	/* and later again: does medialibraryd keep the row (a validation of its own could drop it)? */
	alarm(0);
	for (unsigned i = 0; i < 3; i++) {
		static const unsigned after[] = { 60, 180, 420 };
		char when[32];

		sleep(after[i] - (i ? after[i - 1] : 0));
		snprintf(when, sizeof(when), "%u s later", after[i]);
		dump_row([track persistentID], when);
		media_query(when, [track persistentID]);
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
