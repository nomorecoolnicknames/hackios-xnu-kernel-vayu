/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 OOSEX GANG
 *
 * HakiosMP3 -- a second player beside Filza, so that "this track does not play" can be
 * told apart as Filza's problem or the system's (AVFoundation / mediaserverd / the audio
 * driver).
 *
 * THE LOG IS THE PRODUCT. Every action and every NSError (domain, code,
 * localizedDescription, userInfo) is printed as "mp3play: ..." through NSLog and stderr.
 * A player that returns YES to -play while currentTime stands still is the symptom being
 * hunted, so its clock is printed one second after play and every five seconds after
 * that -- not on every 0.25 s UI tick.
 *
 * NO UIApplicationSceneManifest in mp3_app.plist, as in triangle_app: the application
 * delegate makes its own window because this port has no scene delegate to hand it to.
 *
 * Entry points for testing (the launch URL beats MP3PLAY_FILE, which beats argv; the winner is
 * logged):
 *   open hakiosmp3://play?path=%2Fvar%2Fmobile%2FDocuments%2Ftrack.mp3
 *   MP3PLAY_FILE=/var/mobile/Documents/track.mp3 HakiosMP3
 *   HakiosMP3 --play /var/mobile/Documents/track.mp3
 */
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <AVFoundation/AVFoundation.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static NSString *const kDocumentsDirectory = @"/var/mobile/Documents";
static NSString *const kDownloadsDirectory = @"/var/mobile/Media/Downloads";
static const NSTimeInterval kTickInterval = 0.25;
static const NSTimeInterval kSlowLogInterval = 5.0;

/*
 * Where the lines go. NSLog is os_log, which this port's console does not collect, and an
 * application's stderr is /dev/null once SpringBoard starts it (a test run: zero lines).
 * So also /dev/console, if this user may write it (the console log this port captures), and a
 * file that ssh can read: /var/mobile/Library/Logs/HakiosMP3.log.
 */
#define LOG_FILE "/var/mobile/Library/Logs/HakiosMP3.log"
static int console_fd = -1, file_fd = -1, console_errno;

static void
open_sinks(void)
{
	console_fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_NONBLOCK);
	console_errno = errno;
	file_fd = open(LOG_FILE, O_WRONLY | O_APPEND | O_CREAT, 0644);
}

static void
say(NSString *format, ...)
{
	va_list arguments;
	NSString *line;
	const char *text;
	va_start(arguments, format);
	line = [[NSString alloc] initWithFormat:[@"mp3play: " stringByAppendingString:format] arguments:arguments];
	va_end(arguments);
	NSLog(@"%@", line);
	text = [[line stringByAppendingString:@"\n"] UTF8String];
	fputs(text, stderr);
	if (console_fd >= 0) (void)write(console_fd, text, strlen(text));
	if (file_fd >= 0) (void)write(file_fd, text, strlen(text));
}

/* The domain and code ARE the diagnosis: AVFoundation and mediaserverd can both say
 * "operation failed" and mean different things. userInfo carries the underlying error. */
static void
say_error(NSString *what, NSError *error)
{
	NSError *under;
	if (error == nil) { say(@"%@: no NSError", what); return; }
	say(@"%@: domain=%@ code=%ld localizedDescription=%@ userInfo=%@", what, error.domain, (long)error.code, error.localizedDescription, error.userInfo);
	under = error.userInfo[NSUnderlyingErrorKey];
	if (under != nil && under != error) say(@"%@: underlying domain=%@ code=%ld localizedDescription=%@ userInfo=%@", what, under.domain, (long)under.code, under.localizedDescription, under.userInfo);
}

static NSString *
error_text(NSError *error)
{
	return error == nil ? @"unknown error (no NSError)" : [NSString stringWithFormat:@"%@ (%@ %ld)", error.localizedDescription, error.domain, (long)error.code];
}

static NSString *
clock_text(double seconds)
{
	unsigned total;
	/* also catches NaN: a duration must never become a plausible 0:00 */
	if (!(seconds >= 0) || seconds > 359999.0) seconds = 0;
	total = (unsigned)(seconds + 0.5);
	return [NSString stringWithFormat:@"%u:%02u", total / 60, total % 60];
}

/* ------------------------------------------------------------------------ the audio session */

static NSString *
route_text(AVAudioSessionRouteDescription *route)
{
	NSMutableArray *outputs = [NSMutableArray array];
	for (AVAudioSessionPortDescription *port in route.outputs) [outputs addObject:[NSString stringWithFormat:@"%@/%@", port.portType, port.portName]];
	return outputs.count != 0 ? [outputs componentsJoinedByString:@", "] : @"(no outputs)";
}

static void
say_session(NSString *what)
{
	AVAudioSession *session = [AVAudioSession sharedInstance];
	say(@"audio: %@: category %@, outputs [%@], sampleRate %.0f Hz, outputVolume %.3f, IOBufferDuration %.4f s", what, session.category, route_text(session.currentRoute), session.sampleRate, session.outputVolume, session.IOBufferDuration);
}

static BOOL
setup_audio_session(NSString *why)
{
	AVAudioSession *session = [AVAudioSession sharedInstance];
	NSError *error = nil;
	BOOL category_ok, active_ok;

	say(@"audio: setting up the session (%@)", why);
	category_ok = [session setCategory:AVAudioSessionCategoryPlayback error:&error];
	if (category_ok) say(@"audio: setCategory(Playback) = YES"); else say_error(@"audio: setCategory(Playback) = NO", error);
	error = nil;
	active_ok = [session setActive:YES error:&error];
	if (active_ok) say(@"audio: setActive(YES) = YES"); else say_error(@"audio: setActive(YES) = NO", error);
	say_session(@"after setup");
	return category_ok && active_ok;
}

/* ------------------------------------------------------------------------------------- files */

static NSInteger
compare_rows(id left, id right, void *context)
{
	NSDictionary *a = (NSDictionary *)left, *b = (NSDictionary *)right;
	NSComparisonResult result = [a[@"name"] caseInsensitiveCompare:b[@"name"]];
	(void)context;
	return result != NSOrderedSame ? result : [a[@"path"] compare:b[@"path"]];
}

static NSString *scan_summary;          /* what the last scan found, per directory, for the screen */

/*
 * The walk is readdir(3) and stat(2) on the bytes as they are on disk, not NSFileManager:
 * Foundation decomposes names on the way to the kernel (fileSystemRepresentation), and this
 * port's /var (ramfs) used to compare them byte for byte, so NSDirectoryEnumerator silently
 * skipped every name with a precomposed letter (ё, й, é) and fileExistsAtPath: said NO for it
 * (2026-10-26, a test run). Every entry is logged with what was decided about it.
 */
static void
scan_dir(const char *dir, NSString *label, int depth, NSMutableArray *found, NSSet *extensions, unsigned *raw)
{
	DIR *handle = opendir(dir);
	struct dirent *entry;

	if (handle == NULL) {
		say(@"scan: opendir %s: %s", dir, strerror(errno));
		scan_summary = [scan_summary stringByAppendingFormat:@" %@: %s;", label, strerror(errno)];
		return;
	}
	while ((entry = readdir(handle)) != NULL) {
		char path[1024];
		struct stat st;
		NSString *name, *extension;

		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
		(*raw)++;
		snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
		if (stat(path, &st) != 0) { say(@"scan: [%s] stat: %s -> skip", path, strerror(errno)); continue; }
		if (S_ISDIR(st.st_mode)) {
			if (depth < 4) scan_dir(path, label, depth + 1, found, extensions, raw);
			else say(@"scan: [%s] directory deeper than 4 -> skip", path);
			continue;
		}
		name = [NSString stringWithUTF8String:entry->d_name];
		extension = [name.pathExtension lowercaseString];
		if (!S_ISREG(st.st_mode) || name == nil || ![extensions containsObject:extension]) {
			say(@"scan: [%s] %s, extension [%@] -> skip", path, S_ISREG(st.st_mode) ? "file" : "not a file", extension);
			continue;
		}
		say(@"scan: [%s] %lld bytes -> listed", path, (long long)st.st_size);
		[found addObject:@{ @"path": [NSString stringWithUTF8String:path], @"name": name, @"directory": label,
		    @"size": @((unsigned long long)st.st_size) }];
	}
	closedir(handle);
}

static NSArray *
scan_one(NSString *directory, NSString *label)
{
	NSMutableArray *found = [NSMutableArray array];
	NSSet *extensions = [NSSet setWithObjects:@"mp3", @"m4a", @"aac", @"wav", @"caf", @"aiff", nil];
	unsigned raw = 0;

	scan_dir(directory.UTF8String, label, 0, found, extensions, &raw);
	say(@"scan: %@: %u entries seen, %lu playable file(s)", directory, raw, (unsigned long)found.count);
	scan_summary = [scan_summary stringByAppendingFormat:@" %@: %lu;", label, (unsigned long)found.count];
	return found;
}

static NSArray *
scan_all(void)
{
	NSMutableArray *all = [NSMutableArray array];
	scan_summary = @"";
	say(@"scan: as uid %d, HOME %@", (int)getuid(), NSHomeDirectory());
	[all addObjectsFromArray:scan_one(kDocumentsDirectory, @"Documents")];
	[all addObjectsFromArray:scan_one(kDownloadsDirectory, @"Downloads")];
	NSArray *sorted = [all sortedArrayUsingFunction:compare_rows context:NULL];
	say(@"scan: %lu playable file(s) in the two directories", (unsigned long)sorted.count);
	return sorted;
}

/* ------------------------------------------------------------------------ launch requests */

static NSString *
path_from_url(NSURL *url)
{
	NSURLComponents *components;
	NSString *path = nil;

	if (url == nil) return nil;
	say(@"url: received %@ (scheme %@, host %@)", url.absoluteString, url.scheme, url.host);
	if (url.isFileURL) return url.path;
	if (![[url.scheme lowercaseString] isEqualToString:@"hakiosmp3"]) { say(@"url: ignoring scheme %@; this app answers hakiosmp3://play?path=...", url.scheme); return nil; }
	components = [NSURLComponents componentsWithURL:url resolvingAgainstBaseURL:NO];
	for (NSURLQueryItem *item in components.queryItems) if ([item.name isEqualToString:@"path"]) { path = item.value; break; }
	if (path.length == 0) { say(@"url: %@ has no non-empty path query item", url.absoluteString); return nil; }
	if ([path hasPrefix:@"file://"]) { NSURL *file_url = [NSURL URLWithString:path]; if (file_url.isFileURL) path = file_url.path; }
	say(@"url: decoded path %@", path);
	return path;
}

static NSString *
path_from_arguments(NSArray *arguments)
{
	NSUInteger index;
	for (index = 1; index < arguments.count; index++) {
		NSString *argument = arguments[index];
		if (([argument isEqualToString:@"--play"] || [argument isEqualToString:@"-play"]) && index + 1 < arguments.count) { say(@"argv: %@ %@", argument, arguments[index + 1]); return arguments[index + 1]; }
	}
	/* A bare absolute path is how a shell test usually starts a program, and no SpringBoard argument begins with '/'. */
	for (index = 1; index < arguments.count; index++) if ([arguments[index] hasPrefix:@"/"]) { say(@"argv: bare path %@", arguments[index]); return arguments[index]; }
	return nil;
}

/* --------------------------------------------------------------------------------- the UI */

@interface MP3ViewController : UIViewController <UITableViewDataSource, UITableViewDelegate, AVAudioPlayerDelegate>
@property (nonatomic, strong) UITableView *table;
@property (nonatomic, strong) UIView *panel;
@property (nonatomic, strong) UILabel *trackLabel, *positionLabel, *statusLabel;
@property (nonatomic, strong) UIButton *playButton, *rescanButton;
@property (nonatomic, strong) UIRefreshControl *refreshControl;
@property (nonatomic, strong) NSArray *files;
@property (nonatomic, strong) AVAudioPlayer *player;
@property (nonatomic, strong) NSTimer *tickTimer;
@property (nonatomic, copy) NSString *currentPath;
@property (nonatomic, assign) NSTimeInterval playWallStart, lastSlowLogWall;
@property (nonatomic, assign) BOOL oneSecondLogged;
- (void)playPath:(NSString *)path reason:(NSString *)reason;
- (void)showStatus:(NSString *)text;
@end

@implementation MP3ViewController

- (UILabel *)labelWithFont:(UIFont *)font color:(UIColor *)color
{
	UILabel *label = [[UILabel alloc] init];
	label.font = font; label.textColor = color; label.lineBreakMode = NSLineBreakByTruncatingMiddle;
	return label;
}

- (void)loadView
{
	UIView *root = [[UIView alloc] initWithFrame:UIScreen.mainScreen.bounds];
	say(@"ui: building views");
	root.backgroundColor = UIColor.systemBackgroundColor;
	self.view = root;
	self.table = [[UITableView alloc] initWithFrame:CGRectZero style:UITableViewStylePlain];
	self.table.dataSource = self; self.table.delegate = self; self.table.rowHeight = 52.0;
	[root addSubview:self.table];
	self.refreshControl = [[UIRefreshControl alloc] init];
	[self.refreshControl addTarget:self action:@selector(rescan:) forControlEvents:UIControlEventValueChanged];
	self.table.refreshControl = self.refreshControl;
	self.panel = [[UIView alloc] initWithFrame:CGRectZero];
	self.panel.backgroundColor = UIColor.secondarySystemBackgroundColor;
	[root addSubview:self.panel];
	self.trackLabel = [self labelWithFont:[UIFont boldSystemFontOfSize:15] color:UIColor.labelColor];
	self.trackLabel.text = @"No track";
	self.positionLabel = [self labelWithFont:[UIFont monospacedDigitSystemFontOfSize:13 weight:UIFontWeightRegular] color:UIColor.secondaryLabelColor];
	self.positionLabel.text = @"0:00 / 0:00";
	self.playButton = [UIButton buttonWithType:UIButtonTypeSystem];
	[self.playButton setTitle:@"Play" forState:UIControlStateNormal];
	self.playButton.titleLabel.font = [UIFont boldSystemFontOfSize:17];
	[self.playButton addTarget:self action:@selector(togglePlay:) forControlEvents:UIControlEventTouchUpInside];
	self.rescanButton = [UIButton buttonWithType:UIButtonTypeSystem];
	[self.rescanButton setTitle:@"Rescan" forState:UIControlStateNormal];
	[self.rescanButton addTarget:self action:@selector(rescan:) forControlEvents:UIControlEventTouchUpInside];
	self.statusLabel = [self labelWithFont:[UIFont systemFontOfSize:11] color:UIColor.secondaryLabelColor];
	self.statusLabel.text = @"Scanning..."; self.statusLabel.numberOfLines = 1;
	self.statusLabel.adjustsFontSizeToFitWidth = YES; self.statusLabel.minimumScaleFactor = 0.6;
	for (UIView *view in @[self.trackLabel, self.positionLabel, self.playButton, self.rescanButton, self.statusLabel]) [self.panel addSubview:view];
}

/* Frames, not constraints: the panel is a fixed 140 pt and the safe-area insets are the only geometry UIKit has to provide. Relaid out on every rotation/inset change. */
- (void)viewDidLayoutSubviews
{
	[super viewDidLayoutSubviews];
	UIEdgeInsets safe = self.view.safeAreaInsets;
	CGRect bounds = self.view.bounds;
	CGFloat width = bounds.size.width, inner = width - 24.0;
	self.panel.frame = CGRectMake(0, bounds.size.height - safe.bottom - 140.0, width, 140.0);
	self.table.frame = CGRectMake(0, safe.top, width, self.panel.frame.origin.y - safe.top);
	self.trackLabel.frame = CGRectMake(12, 8, inner, 22);
	self.positionLabel.frame = CGRectMake(12, 30, inner, 20);
	self.playButton.frame = CGRectMake(12, 58, 96, 36);
	self.rescanButton.frame = CGRectMake(116, 58, 96, 36);
	self.statusLabel.frame = CGRectMake(12, 102, inner, 18);
}

- (void)viewDidLoad
{
	NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
	[super viewDidLoad];
	say(@"ui: viewDidLoad");
	[center addObserver:self selector:@selector(sessionInterrupted:) name:AVAudioSessionInterruptionNotification object:nil];
	[center addObserver:self selector:@selector(sessionRouteChanged:) name:AVAudioSessionRouteChangeNotification object:nil];
	[center addObserver:self selector:@selector(mediaServicesWereReset:) name:AVAudioSessionMediaServicesWereResetNotification object:nil];
	[self rescan:nil];
}

- (void)viewDidAppear:(BOOL)animated
{
	static BOOL said;
	[super viewDidAppear:animated];
	if (!said) { said = YES; say(@"ui: table %@, panel %@, safeAreaInsets top %.0f bottom %.0f", NSStringFromCGRect(self.table.frame), NSStringFromCGRect(self.panel.frame), self.view.safeAreaInsets.top, self.view.safeAreaInsets.bottom); }
}

- (void)dealloc { [self.tickTimer invalidate]; [[NSNotificationCenter defaultCenter] removeObserver:self]; }
- (void)showStatus:(NSString *)text { say(@"status: %@", text); self.statusLabel.text = text; }

- (void)rescan:(id)sender
{
	NSString *who = sender == nil ? @"launch" : sender == self.refreshControl ? @"pull to refresh" : @"Rescan button";
	say(@"scan: rescan requested by %@", who);
	self.files = scan_all();
	[self.table reloadData];
	[self.refreshControl endRefreshing];
	[self showStatus:[NSString stringWithFormat:@"%lu file(s):%@ log %s", (unsigned long)self.files.count, scan_summary, LOG_FILE]];
	if (self.files.count == 0) {
		UILabel *empty = [self labelWithFont:[UIFont systemFontOfSize:15] color:UIColor.secondaryLabelColor];
		empty.numberOfLines = 0; empty.textAlignment = NSTextAlignmentCenter;
		empty.text = [NSString stringWithFormat:@"No audio files found in\n%@\n%@\n\n%@", kDocumentsDirectory, kDownloadsDirectory, scan_summary];
		self.table.backgroundView = empty;
	} else {
		self.table.backgroundView = nil;
	}
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section { (void)tableView; (void)section; return (NSInteger)self.files.count; }

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath
{
	UITableViewCell *cell = [tableView dequeueReusableCellWithIdentifier:@"MP3FileCell"];
	NSDictionary *row;
	if (cell == nil) cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:@"MP3FileCell"];
	row = self.files[(NSUInteger)indexPath.row];
	cell.textLabel.text = row[@"name"];
	cell.detailTextLabel.text = [NSString stringWithFormat:@"%@ · %.1f KB", row[@"directory"], [row[@"size"] unsignedLongLongValue] / 1024.0];
	return cell;
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath
{
	NSDictionary *row = self.files[(NSUInteger)indexPath.row];
	[tableView deselectRowAtIndexPath:indexPath animated:YES];
	say(@"ui: row %ld tapped: %@", (long)indexPath.row, row[@"path"]);
	[self playPath:row[@"path"] reason:@"row tap"];
}

/* --------------------------------------------------------------------------------- playback */

- (void)stopTickTimer { if (self.tickTimer != nil) { say(@"timer: stopping the 0.25 s tick timer"); [self.tickTimer invalidate]; self.tickTimer = nil; } }

- (void)startTickTimer
{
	if (self.tickTimer != nil) return;
	say(@"timer: starting the 0.25 s tick timer");
	self.tickTimer = [NSTimer timerWithTimeInterval:kTickInterval target:self selector:@selector(tick:) userInfo:nil repeats:YES];
	self.tickTimer.tolerance = 0.02;
	[[NSRunLoop mainRunLoop] addTimer:self.tickTimer forMode:NSRunLoopCommonModes];
}

- (void)stopPlayerWithReason:(NSString *)reason
{
	AVAudioPlayer *player = self.player;
	if (player != nil) {
		say(@"player: stop (%@): playing %@ at %.3f of %.3f s, path %@", reason, player.playing ? @"YES" : @"NO", player.currentTime, player.duration, self.currentPath);
		[player stop]; [player setDelegate:nil]; self.player = nil;
	} else say(@"player: nothing to stop (%@)", reason);
	[self stopTickTimer];
	self.currentPath = nil; self.trackLabel.text = @"No track"; self.positionLabel.text = @"0:00 / 0:00";
	[self.playButton setTitle:@"Play" forState:UIControlStateNormal];
}

- (void)updatePlayButton { [self.playButton setTitle:(self.player != nil && self.player.playing) ? @"Pause" : @"Play" forState:UIControlStateNormal]; }
- (void)updatePositionLabel { AVAudioPlayer *p = self.player; self.positionLabel.text = [NSString stringWithFormat:@"%@ / %@", clock_text(p != nil ? p.currentTime : 0.0), clock_text(p != nil ? p.duration : 0.0)]; }

- (void)notePlaybackStarted:(NSString *)what
{
	NSTimeInterval now = [NSDate timeIntervalSinceReferenceDate];
	self.playWallStart = now; self.lastSlowLogWall = now; self.oneSecondLogged = NO;
	[self startTickTimer];
	say(@"player: %@ began; the +1 s check and a line every %.0f s follow", what, kSlowLogInterval);
}

- (void)playPath:(NSString *)path reason:(NSString *)reason
{
	NSFileManager *manager = [NSFileManager defaultManager];
	NSError *error = nil;
	NSDictionary *attributes;
	unsigned long long size = 0;
	BOOL exists, readable, prepared, started;
	AVAudioPlayer *player;

	if (path.length == 0) { say(@"play: %@ gave an empty path", reason); [self showStatus:@"No path to play"]; return; }
	say(@"play: request (%@): %@", reason, path);
	[self stopPlayerWithReason:[NSString stringWithFormat:@"new request (%@)", reason]];
	exists = [manager fileExistsAtPath:path];
	readable = [manager isReadableFileAtPath:path];
	attributes = [manager attributesOfItemAtPath:path error:&error];
	if (attributes != nil) size = [attributes[NSFileSize] unsignedLongLongValue]; else say_error([NSString stringWithFormat:@"play: attributes of %@", path], error);
	{
		struct stat st;
		int raw = stat(path.UTF8String, &st);
		say(@"play: stat of the raw path: %s, %lld bytes", raw == 0 ? "ok" : strerror(errno), raw == 0 ? (long long)st.st_size : -1LL);
		if (raw == 0 && size == 0) size = (unsigned long long)st.st_size;
	}
	say(@"play: %@ exists %@, readable %@, size %llu bytes (%.1f KB), extension %@", path, exists ? @"YES" : @"NO", readable ? @"YES" : @"NO", size, size / 1024.0, [path.pathExtension lowercaseString]);
	error = nil;
	player = [[AVAudioPlayer alloc] initWithContentsOfURL:[NSURL fileURLWithPath:path] error:&error];
	if (player == nil) {
		/* the same file read by its on-disk bytes (fopen of UTF8String, no decomposition): if this
		 * plays, the URL path failed on the name, not on the audio */
		NSData *data = nil;
		FILE *file;
		say_error(@"player: initWithContentsOfURL FAILED", error);
		if ((file = fopen(path.UTF8String, "rb")) != NULL) {
			NSMutableData *bytes = [NSMutableData dataWithLength:(NSUInteger)size];
			size_t got = fread(bytes.mutableBytes, 1, (size_t)size, file);
			fclose(file);
			bytes.length = got;
			data = bytes;
			say(@"player: read %zu of %llu bytes by the raw path; trying initWithData", got, size);
		} else {
			say(@"player: fopen of the raw path: %s", strerror(errno));
		}
		error = nil;
		player = data != nil ? [[AVAudioPlayer alloc] initWithData:data error:&error] : nil;
		if (player == nil) { say_error(@"player: initWithData FAILED", error); [self showStatus:[NSString stringWithFormat:@"Cannot open: %@", error_text(error)]]; return; }
		say(@"player: initWithData worked: the URL path failed on the file NAME (normalization), not on the audio");
	}
	say(@"player: duration %.3f s, %lu channel(s), format %@", player.duration, (unsigned long)player.numberOfChannels, player.format != nil ? [player.format description] : @"(no format)");
	self.player = player; self.currentPath = path; self.trackLabel.text = path.lastPathComponent;
	[self updatePlayButton]; [self updatePositionLabel];
	/* before prepare, so a file short enough to finish inside -play still reports it */
	[player setDelegate:self];
	say(@"player: delegate = %@, volume %.2f", NSStringFromClass([self class]), player.volume);
	prepared = [player prepareToPlay];
	say(@"player: prepareToPlay = %@", prepared ? @"YES" : @"NO");
	started = [player play];
	say(@"player: play = %@, isPlaying %@, currentTime %.3f", started ? @"YES" : @"NO", player.playing ? @"YES" : @"NO", player.currentTime);
	[self updatePlayButton];
	if (started) { [self notePlaybackStarted:@"play"]; [self showStatus:[NSString stringWithFormat:@"Playing %@", path.lastPathComponent]]; }
	else [self showStatus:@"play returned NO"];
}

- (void)togglePlay:(id)sender
{
	AVAudioPlayer *player = self.player;
	BOOL started;
	(void)sender;
	if (player == nil) { say(@"player: Play/Pause tapped with no track loaded"); [self showStatus:@"No track: tap a file first"]; return; }
	if (player.playing) {
		[player pause];
		say(@"player: pause = YES, isPlaying %@, currentTime %.3f", player.playing ? @"YES" : @"NO", player.currentTime);
		[self showStatus:@"Paused"];
	} else {
		if (player.duration > 0 && player.currentTime >= player.duration - 0.05) { player.currentTime = 0; say(@"player: was at the end, rewound to 0 before replay"); }
		started = [player play];
		say(@"player: play (resume) = %@, isPlaying %@, currentTime %.3f", started ? @"YES" : @"NO", player.playing ? @"YES" : @"NO", player.currentTime);
		if (started) { [self notePlaybackStarted:@"resume"]; [self showStatus:@"Playing"]; } else [self showStatus:@"play returned NO"];
	}
	[self updatePlayButton]; [self updatePositionLabel];
}

/* The 5 s line uses WALL time and prints the player's clock beside it, so a player whose isPlaying says YES while currentTime does not advance is visible as a difference. */
- (void)tick:(NSTimer *)timer
{
	AVAudioPlayer *player = self.player;
	NSTimeInterval wall = [NSDate timeIntervalSinceReferenceDate];
	(void)timer;
	if (player == nil) { [self stopTickTimer]; return; }
	[self updatePositionLabel];
	if (!self.oneSecondLogged && self.playWallStart > 0 && wall - self.playWallStart >= 1.0) {
		self.oneSecondLogged = YES;
		say(@"+1.0 s check: currentTime %.3f s, isPlaying %@, duration %.3f s, path %@", player.currentTime, player.playing ? @"YES" : @"NO", player.duration, self.currentPath);
	}
	if (player.playing && wall - self.lastSlowLogWall >= kSlowLogInterval) {
		self.lastSlowLogWall = wall;
		say(@"+%.1f s: currentTime %.3f s of %.3f s, isPlaying YES%s", wall - self.playWallStart, player.currentTime, player.duration, player.currentTime < 0.001 ? " (the clock has not moved)" : "");
	}
}

- (void)audioPlayerDidFinishPlaying:(AVAudioPlayer *)player successfully:(BOOL)success
{
	say(@"delegate: didFinishPlaying successfully %@, currentTime %.3f of %.3f, path %@", success ? @"YES" : @"NO", player.currentTime, player.duration, self.currentPath);
	[self stopTickTimer]; [self updatePlayButton]; [self updatePositionLabel];
	[self showStatus:success ? @"Finished" : @"Finished with a decode error"];
}

- (void)audioPlayerDecodeErrorDidOccur:(AVAudioPlayer *)player error:(NSError *)error
{
	say(@"delegate: decode error after %.3f of %.3f s, path %@", player.currentTime, player.duration, self.currentPath);
	say_error(@"delegate: audioPlayerDecodeErrorDidOccur", error);
	[self stopTickTimer]; [self updatePlayButton];
	[self showStatus:[NSString stringWithFormat:@"Decode error: %@", error_text(error)]];
}

/* ---------------------------------------------------------------------- session notifications */

- (void)sessionInterrupted:(NSNotification *)note
{
	NSDictionary *info = note.userInfo;
	NSNumber *type = info[AVAudioSessionInterruptionTypeKey], *options = info[AVAudioSessionInterruptionOptionKey];
	say(@"audio: AVAudioSessionInterruptionNotification: type %@ (%@), options %@, userInfo %@", type, [type unsignedIntegerValue] == AVAudioSessionInterruptionTypeBegan ? @"began" : [type unsignedIntegerValue] == AVAudioSessionInterruptionTypeEnded ? @"ended" : @"other", options, info);
	if (type != nil && [type unsignedIntegerValue] == AVAudioSessionInterruptionTypeBegan) { [self updatePlayButton]; [self showStatus:@"Audio session interrupted"]; }
	else if (type != nil && [type unsignedIntegerValue] == AVAudioSessionInterruptionTypeEnded) {
		BOOL resume = options != nil && ([options unsignedIntegerValue] & AVAudioSessionInterruptionOptionShouldResume) != 0;
		say(@"audio: interruption ended, should resume %@; the player is left as it is", resume ? @"YES" : @"NO");
		[self showStatus:resume ? @"Interruption ended (resume available)" : @"Interruption ended"];
	}
}

- (void)sessionRouteChanged:(NSNotification *)note
{
	NSDictionary *info = note.userInfo;
	NSNumber *reason = info[AVAudioSessionRouteChangeReasonKey];
	AVAudioSessionRouteDescription *previous = info[AVAudioSessionRouteChangePreviousRouteKey];
	say(@"audio: AVAudioSessionRouteChangeNotification: reason %@, previous [%@], current [%@], userInfo %@", reason, route_text(previous), route_text([[AVAudioSession sharedInstance] currentRoute]), info);
	say_session(@"after route change");
	[self showStatus:[NSString stringWithFormat:@"Audio route changed (reason %@)", reason]];
}

- (void)mediaServicesWereReset:(NSNotification *)note
{
	say(@"audio: AVAudioSessionMediaServicesWereResetNotification, userInfo %@", note.userInfo);
	[self stopPlayerWithReason:@"media services were reset"];
	setup_audio_session(@"after media services reset");
	[self showStatus:@"Media services reset: player dropped, session re-set"];
}

@end

/* --------------------------------------------------------------------------- the application */

@interface MP3AppDelegate : NSObject <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@property (nonatomic, strong) MP3ViewController *controller;
@end

@implementation MP3AppDelegate

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options
{
	NSProcessInfo *process = [NSProcessInfo processInfo];
	NSBundle *bundle = [NSBundle mainBundle];
	NSURL *launch_url = options[UIApplicationLaunchOptionsURLKey];
	NSString *environment_path = process.environment[@"MP3PLAY_FILE"];
	NSString *path = nil, *source = nil;

	(void)application;
	say(@"app: didFinishLaunchingWithOptions: pid %d, uid %d, euid %d", (int)getpid(), (int)getuid(), (int)geteuid());
	say(@"app: bundle path %@", bundle.bundlePath);
	say(@"app: bundle id %@, name %@, version %@ (%@)", bundle.bundleIdentifier, [bundle objectForInfoDictionaryKey:@"CFBundleName"], [bundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"], [bundle objectForInfoDictionaryKey:@"CFBundleVersion"]);
	say(@"app: arguments %@, MP3PLAY_FILE %@", process.arguments, environment_path.length != 0 ? environment_path : @"(unset or empty)");
	setup_audio_session(@"launch");
	self.controller = [[MP3ViewController alloc] init];
	self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
	[self.window setRootViewController:self.controller];
	[self.window makeKeyAndVisible];
	[self.controller loadViewIfNeeded];
	say(@"app: window key and visible, %g x %g points", self.window.bounds.size.width, self.window.bounds.size.height);
	if ([launch_url isKindOfClass:[NSURL class]]) { path = path_from_url(launch_url); source = @"launch URL"; }
	else if (launch_url != nil) say(@"app: UIApplicationLaunchOptionsURLKey is not an NSURL: %@", launch_url);
	if (path == nil && environment_path.length != 0) { path = environment_path; source = @"MP3PLAY_FILE"; }
	if (path == nil) { path = path_from_arguments(process.arguments); source = @"launch argument"; }
	if (path != nil) { say(@"app: direct play from %@ (URL beats MP3PLAY_FILE beats argv)", source); [self.controller playPath:path reason:source]; }
	else say(@"app: no launch URL, MP3PLAY_FILE or --play argument; waiting for a row tap");
	return YES;
}

- (BOOL)application:(UIApplication *)application openURL:(NSURL *)url options:(NSDictionary *)options
{
	NSString *path;
	(void)application;
	say(@"app: openURL %@, options %@", url.absoluteString, options);
	path = path_from_url(url);
	if (path == nil) { [self.controller showStatus:@"URL had no usable path"]; return NO; }
	[self.controller playPath:path reason:@"openURL"];
	return YES;
}

- (void)applicationDidEnterBackground:(UIApplication *)application { (void)application; say(@"app: didEnterBackground; category Playback and UIBackgroundModes=audio should keep the player running"); }
- (void)applicationWillEnterForeground:(UIApplication *)application { (void)application; say(@"app: willEnterForeground"); }

@end

int
main(int argc, char *argv[])
{
	setvbuf(stderr, NULL, _IONBF, 0);
	open_sinks();
	say(@"start: HakiosMP3, pid %d, uid %d, argc %d; /dev/console %s, %s %s", (int)getpid(), (int)getuid(), argc,
	    console_fd >= 0 ? "open" : strerror(console_errno), LOG_FILE, file_fd >= 0 ? "open" : "NOT writable");
	@autoreleasepool {
		return UIApplicationMain(argc, argv, nil, NSStringFromClass([MP3AppDelegate class]));
	}
}
