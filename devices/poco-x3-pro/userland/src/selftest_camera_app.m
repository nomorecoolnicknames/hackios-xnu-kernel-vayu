/*
 * selftest_camera_app: the camera part of the overnight self-test, as an APPLICATION.
 *
 * It has to be an application: on this port the frames are handed out by libhakios.media.dylib's
 * FigCaptureDevice inside mediaserverd, and the capture stack gives a session frames only while its
 * client is a foreground application (userland/src/camera_avtest.m: "a program like this one is not
 * given any (its session is interrupted at once). Camera.app is the test for those"). A daemon
 * cannot take the photograph the self-test is asked for; a UIApplication can.
 *
 * It is opened by SpringBoard from selftest(1) through the URL scheme below:
 *   hakiostest://camera?run=<boot's seconds>
 * and leaves everything it makes in /var/mobile/Media/Selftest:
 *   still-<run>.jpg|heic   one photograph through AVCapturePhotoOutput
 *   video-<run>.mov        five seconds through AVCaptureMovieFileOutput
 *   camera-<run>.result    key=value: what was made, its size, and whether the movie has moov
 *
 * It exits by itself when it is done (an application that lingers would keep the camera and the
 * screen for the next stage). The lines it writes are prefixed "SELFTEST: camera-app:" in the log
 * at /var/mobile/Library/Logs/SelftestCamera.log, which selftest(1) prints when the check fails;
 * the console may not be writable for uid 501, so the file is the one that is counted on.
 *
 * The result file is what selftest(1) reads; it verifies the files again itself (size, JPEG/HEIC
 * magic, the movie's atoms) and does not take the app's word for PASS.
 */
#import <UIKit/UIKit.h>
#import <AVFoundation/AVFoundation.h>

#include "selftest_util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RESULT_DIR	"/var/mobile/Media/Selftest"
#define LOG_PATH	"/var/mobile/Library/Logs/SelftestCamera.log"
#define STILL_AFTER	0.5
#define MOVIE_SECONDS	5.0
#define WATCHDOG_SECONDS 45.0

static int		console_fd = -1;
static int		log_fd = -1;
static long long	run_id;
static NSString *	result_path;
static NSString *	still_path;
static NSString *	video_path;

static void
app_say(NSString *format, ...)
{
	va_list arguments;
	NSString *line;
	char text[2048];

	va_start(arguments, format);
	line = [[NSString alloc] initWithFormat:format arguments:arguments];
	va_end(arguments);
	snprintf(text, sizeof(text), "SELFTEST: camera-app: %s\n", [line UTF8String]);
	if (console_fd >= 0) {
		(void)write(console_fd, text, strlen(text));
	}
	if (log_fd >= 0) {
		(void)write(log_fd, text, strlen(text));
	}
	fputs(text, stderr);
}

static void
ensure_dirs(const char *path)
{
	char copy[PATH_MAX];

	if (path == NULL || strlen(path) >= sizeof(copy)) {
		return;
	}
	strcpy(copy, path);
	for (char *p = copy + 1; *p != '\0'; p++) {
		if (*p == '/') {
			*p = '\0';
			(void)mkdir(copy, 0755);
			*p = '/';
		}
	}
	(void)mkdir(copy, 0755);
}

static void
open_sinks(void)
{
	ensure_dirs(RESULT_DIR);
	ensure_dirs("/var/mobile/Library/Logs");
	console_fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_NONBLOCK);
	log_fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
}

/* ---- the result file ------------------------------------------------------------------------ */

static NSMutableString *result_text;

static void
result_set(NSString *key, NSString *format, ...)
{
	va_list arguments;
	NSString *value;

	va_start(arguments, format);
	value = [[NSString alloc] initWithFormat:format arguments:arguments];
	va_end(arguments);
	[result_text appendFormat:@"%@=%@\n", key, value];
}

static void
result_write(void)
{
	NSError *error = nil;

	if (![result_text writeToFile:result_path atomically:NO encoding:NSUTF8StringEncoding error:&error]) {
		app_say(@"the result file %@ cannot be written: %@", result_path,
		    error != nil ? [error localizedDescription] : @"(no error)");
	} else {
		app_say(@"the result file %@ is written", result_path);
	}
}

/* ---- the application ------------------------------------------------------------------------ */

@interface CameraTest : NSObject <UIApplicationDelegate, AVCapturePhotoCaptureDelegate,
    AVCaptureFileOutputRecordingDelegate>
@property (nonatomic, strong) UIWindow *window;
@property (nonatomic, strong) UIViewController *controller;
@property (nonatomic, strong) AVCaptureSession *session;
@property (nonatomic, strong) AVCapturePhotoOutput *photoOutput;
@property (nonatomic, strong) AVCaptureMovieFileOutput *movieOutput;
@property (nonatomic, strong) AVCaptureVideoPreviewLayer *preview;
@property (nonatomic, assign) BOOL started;
@property (nonatomic, assign) BOOL finished;
@property (nonatomic, assign) BOOL stillWritten;
@end

@implementation CameraTest

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options
{
	NSURL *url = options[UIApplicationLaunchOptionsURLKey];

	(void)application;
	open_sinks();
	run_id = (long long)time(NULL);
	if ([url isKindOfClass:[NSURL class]]) {
		[self takeRunFromURL:url];
	}
	result_path = [NSString stringWithFormat:@"%s/camera-%lld.result", RESULT_DIR, run_id];
	still_path = [NSString stringWithFormat:@"%s/still-%lld.jpg", RESULT_DIR, run_id];
	video_path = [NSString stringWithFormat:@"%s/video-%lld.mov", RESULT_DIR, run_id];
	result_text = [NSMutableString string];
	[self begin_result];
	app_say(@"start: pid %d, uid %d, run %lld, result %@", (int)getpid(), (int)getuid(), run_id, result_path);
	app_say(@"the application is %@ (%@), camera authorization %ld",
	    [[NSBundle mainBundle] bundleIdentifier], [[NSBundle mainBundle] bundlePath],
	    (long)[AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo]);

	self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
	self.controller = [[UIViewController alloc] init];
	self.controller.view.backgroundColor = [UIColor blackColor];
	{
		UILabel *label = [[UILabel alloc] initWithFrame:self.controller.view.bounds];

		label.text = @"selftest camera";
		label.textColor = [UIColor whiteColor];
		label.textAlignment = NSTextAlignmentCenter;
		label.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
		[self.controller.view addSubview:label];
	}
	self.window.rootViewController = self.controller;
	[self.window makeKeyAndVisible];
	app_say(@"the window is key and visible (%.0f x %.0f points)", self.window.bounds.size.width,
	    self.window.bounds.size.height);
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(WATCHDOG_SECONDS * NSEC_PER_SEC)),
	    dispatch_get_main_queue(), ^{
		if (!self.finished) {
			[self finish:NO reason:[NSString stringWithFormat:@"the watchdog fired after %.0f s",
			    WATCHDOG_SECONDS]];
		}
	});
	[self ask_for_camera];
	return YES;
}

- (BOOL)application:(UIApplication *)application openURL:(NSURL *)url options:(NSDictionary *)options
{
	(void)application;
	(void)options;
	app_say(@"openURL %@ (a run already in progress keeps its own id)", [url absoluteString]);
	[self takeRunFromURL:url];
	return YES;
}

- (void)takeRunFromURL:(NSURL *)url
{
	NSURLComponents *parts = [NSURLComponents componentsWithURL:url resolvingAgainstBaseURL:NO];
	NSString *value = nil;

	for (NSURLQueryItem *item in [parts queryItems]) {
		if ([[item name] isEqualToString:@"run"]) {
			value = [item value];
		}
	}
	if ([value length] > 0) {
		run_id = [value longLongValue];
	}
	app_say(@"the URL %@ asks for run %lld", [url absoluteString], run_id);
}

- (void)begin_result
{
	[result_text setString:@""];
	result_set(@"run", @"%lld", run_id);
	result_set(@"pid", @"%d", (int)getpid());
	result_set(@"started", @"%lld", (long long)time(NULL));
	result_set(@"device", @"%@", [[UIDevice currentDevice] name] ?: @"(none)");
	result_set(@"authorization", @"%ld",
	    (long)[AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo]);
}

- (void)ask_for_camera
{
	long status = (long)[AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];

	if (status == AVAuthorizationStatusAuthorized) {
		app_say(@"the camera is authorized");
		[self setUpSession];
		return;
	}
	if (status == AVAuthorizationStatusNotDetermined) {
		app_say(@"the camera authorization is not determined: asking");
		[AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo completionHandler:^(BOOL granted) {
			app_say(@"the camera authorization was asked for: %s", granted ? "granted" : "DENIED");
			dispatch_async(dispatch_get_main_queue(), ^{
				if (granted) {
					[self setUpSession];
				} else {
					[self finish:NO reason:@"the camera permission is denied"];
				}
			});
		}];
		return;
	}
	[self finish:NO reason:[NSString stringWithFormat:@"camera authorization status %ld (denied or restricted)",
	    status]];
}

- (void)setUpSession
{
	AVCaptureDevice *device;
	AVCaptureDeviceInput *input;
	NSError *error = nil;
	AVCaptureSession *session;
	AVCapturePhotoOutput *photo;
	AVCaptureMovieFileOutput *movie;

	if (self.started) {
		return;
	}
	self.started = YES;
	device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
	if (device == nil) {
		[self finish:NO reason:@"AVFoundation lists no video device"];
		return;
	}
	app_say(@"the device: %@ (%@), position %ld, %lu formats, active %@", device.localizedName,
	    device.uniqueID, (long)device.position, (unsigned long)device.formats.count,
	    device.activeFormat.description);
	result_set(@"capture_device", @"%@", device.localizedName);
	input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&error];
	if (input == nil) {
		[self finish:NO reason:[NSString stringWithFormat:@"no device input: %@",
		    error != nil ? [error localizedDescription] : @"(no error)"]];
		return;
	}
	session = [[AVCaptureSession alloc] init];
	if ([session canSetSessionPreset:AVCaptureSessionPresetPhoto]) {
		session.sessionPreset = AVCaptureSessionPresetPhoto;
		app_say(@"the session preset: photo");
	} else if ([session canSetSessionPreset:AVCaptureSessionPresetHigh]) {
		session.sessionPreset = AVCaptureSessionPresetHigh;
		app_say(@"the session preset: photo is not accepted, high is used");
	} else {
		app_say(@"the session preset: neither photo nor high is accepted");
	}
	if (![session canAddInput:input]) {
		[self finish:NO reason:@"the session will not take the video input"];
		return;
	}
	[session addInput:input];
	photo = [[AVCapturePhotoOutput alloc] init];
	if (![session canAddOutput:photo]) {
		[self finish:NO reason:@"the session will not take an AVCapturePhotoOutput"];
		return;
	}
	[session addOutput:photo];
	app_say(@"the photo output: available codecs %@, still image stabilization %ld",
	    photo.availablePhotoCodecTypes, (long)photo.isHighResolutionCaptureEnabled);
	movie = [[AVCaptureMovieFileOutput alloc] init];
	if ([session canAddOutput:movie]) {
		[session addOutput:movie];
		app_say(@"the movie output: added (the 5 s recording is next after the still)");
	} else {
		app_say(@"the movie output: the session will NOT take it (the recording cannot run)");
	}
	self.session = session;
	self.photoOutput = photo;
	self.movieOutput = movie;
	self.preview = [AVCaptureVideoPreviewLayer layerWithSession:session];
	self.preview.frame = self.controller.view.bounds;
	self.preview.videoGravity = AVLayerVideoGravityResizeAspectFill;
	[self.controller.view.layer addSublayer:self.preview];
	[[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(sessionFailed:)
	    name:AVCaptureSessionRuntimeErrorNotification object:session];
	[[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(sessionInterrupted:)
	    name:AVCaptureSessionWasInterruptedNotification object:session];
	[[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(sessionResumed:)
	    name:AVCaptureSessionInterruptionEndedNotification object:session];
	dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
		[session startRunning];
		dispatch_async(dispatch_get_main_queue(), ^{
			[self sessionStarted];
		});
	});
}

- (void)sessionStarted
{
	app_say(@"startRunning returned: isRunning=%d", self.session.isRunning);
	if (!self.session.isRunning) {
		[self finish:NO reason:@"the session did not start"];
		return;
	}
	result_set(@"session_running", @"1");
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(STILL_AFTER * NSEC_PER_SEC)),
	    dispatch_get_main_queue(), ^{
		[self takeStill];
	});
}

- (void)sessionFailed:(NSNotification *)note
{
	NSError *error = [note userInfo][AVCaptureSessionErrorKey];

	app_say(@"the session failed: %@", error != nil ? [error description] : @"(no error)");
	[self finish:NO reason:@"the session failed (see the log)"];
}

- (void)sessionInterrupted:(NSNotification *)note
{
	app_say(@"the session was interrupted: %@ (it needs a foreground application; this is the failure "
	    "camera_avtest.m warns about)", [note userInfo]);
	[self finish:NO reason:@"the session was interrupted"];
}

- (void)sessionResumed:(NSNotification *)note
{
	(void)note;
	app_say(@"the session interruption ended");
}

- (void)takeStill
{
	AVCapturePhotoSettings *settings;
	NSArray *codecs = self.photoOutput.availablePhotoCodecTypes;
	BOOL jpeg = [codecs containsObject:AVVideoCodecTypeJPEG];

	if (jpeg) {
		@try {
			settings = [AVCapturePhotoSettings photoSettingsWithFormat:@{ AVVideoCodecKey: AVVideoCodecTypeJPEG }];
		} @catch (NSException *exception) {
			app_say(@"a JPEG photograph is refused (%@); the default settings are asked for instead",
			    [exception reason]);
			settings = [AVCapturePhotoSettings photoSettings];
		}
	} else {
		/* the default is a HEIF picture; the extension has to say so, the bytes do not lie */
		still_path = [NSString stringWithFormat:@"%s/still-%lld.heic", RESULT_DIR, run_id];
		settings = [AVCapturePhotoSettings photoSettings];
	}
	app_say(@"the photograph: settings codec %@ (available: %@)", jpeg ? @"JPEG" : @"default", codecs);
	@try {
		[self.photoOutput capturePhotoWithSettings:settings delegate:self];
	} @catch (NSException *exception) {
		[self finish:NO reason:[NSString stringWithFormat:@"capturePhotoWithSettings: %@: %@",
		    [exception name], [exception reason]]];
	}
}

- (void)captureOutput:(AVCapturePhotoOutput *)output didFinishProcessingPhoto:(AVCapturePhoto *)photo
    error:(NSError *)error
{
	(void)output;
	/* the capture stack's queue is not the main one; the result file and the state machine are */
	dispatch_async(dispatch_get_main_queue(), ^{
		[self handlePhoto:photo error:error];
	});
}

- (void)handlePhoto:(AVCapturePhoto *)photo error:(NSError *)error
{
	NSData *data;
	struct stat status;

	if (error != nil) {
		app_say(@"the photograph came back with an error: %@", [error description]);
	}
	data = [photo fileDataRepresentation];
	if (data == nil) {
		[self finish:NO reason:[NSString stringWithFormat:@"the photograph has no data: %@",
		    error != nil ? [error localizedDescription] : @"(no error)"]];
		return;
	}
	if (![data writeToFile:still_path options:0 error:&error]) {
		[self finish:NO reason:[NSString stringWithFormat:@"the photograph cannot be written: %@",
		    error != nil ? [error localizedDescription] : @"(no error)"]];
		return;
	}
	memset(&status, 0, sizeof(status));
	(void)stat([still_path fileSystemRepresentation], &status);
	{
		char why[256];
		const char *kind = st_image_kind([still_path fileSystemRepresentation], why, sizeof(why));

		app_say(@"the photograph: %@, %lld bytes, %s", still_path, (long long)status.st_size, kind);
		result_set(@"still", @"%@", still_path);
		result_set(@"still_size", @"%lld", (long long)status.st_size);
		result_set(@"still_kind", @"%s", kind);
	}
	self.stillWritten = status.st_size > 0;
	[self startMovie];
}

- (void)startMovie
{
	if (self.movieOutput == nil) {
		[self finish:NO reason:@"there is no movie output to record with"];
		return;
	}
	if ([self.movieOutput isRecording]) {
		app_say(@"a recording is already running");
		return;
	}
	app_say(@"the movie: recording %.1f s to %@", MOVIE_SECONDS, video_path);
	@try {
		[self.movieOutput startRecordingToOutputFileURL:[NSURL fileURLWithPath:video_path]
		    recordingDelegate:self];
	} @catch (NSException *exception) {
		[self finish:NO reason:[NSString stringWithFormat:@"startRecordingToOutputFileURL: %@: %@",
		    [exception name], [exception reason]]];
		return;
	}
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(MOVIE_SECONDS * NSEC_PER_SEC)),
	    dispatch_get_main_queue(), ^{
		if ([self.movieOutput isRecording]) {
			app_say(@"the movie: %.1f s are up, stopping", MOVIE_SECONDS);
			[self.movieOutput stopRecording];
		}
	});
}

- (void)captureOutput:(AVCaptureFileOutput *)output didFinishRecordingToOutputFileAtURL:(NSURL *)url
    fromConnections:(NSArray *)connections error:(NSError *)error
{
	(void)output;
	(void)connections;
	dispatch_async(dispatch_get_main_queue(), ^{
		[self handleMovieAtURL:url error:error];
	});
}

- (void)handleMovieAtURL:(NSURL *)url error:(NSError *)error
{
	struct stat status;
	char atoms[256] = "", why[256] = "";
	int has_moov = 0;
	long long size = 0;
	BOOL success = YES;
	id flag = [error userInfo][AVErrorRecordingSuccessfullyFinishedKey];

	if ([flag isKindOfClass:[NSNumber class]]) {
		success = [flag boolValue];
	}
	if (error != nil) {
		app_say(@"the movie ended with an error (successfully finished %s): %@", success ? "yes" : "NO",
		    [error description]);
	}
	memset(&status, 0, sizeof(status));
	(void)stat([url fileSystemRepresentation], &status);
	result_set(@"video", @"%@", url.path);
	result_set(@"video_size", @"%lld", (long long)status.st_size);
	if (st_atoms([url fileSystemRepresentation], atoms, sizeof(atoms), &has_moov, &size, why,
	    sizeof(why)) == 0) {
		app_say(@"the movie: %@, %lld bytes, atoms %s, moov %s", url.path, (long long)status.st_size,
		    atoms, has_moov ? "present" : "MISSING");
		result_set(@"video_atoms", @"%s", atoms);
		result_set(@"video_moov", @"%d", has_moov ? 1 : 0);
	} else {
		app_say(@"the movie: %@, %lld bytes, the atoms cannot be read: %s", url.path,
		    (long long)status.st_size, why);
		result_set(@"video_atoms", @"(unreadable)");
		result_set(@"video_moov", @"0");
	}
	{
		struct stat still_status;
		int still_ok;

		memset(&still_status, 0, sizeof(still_status));
		(void)stat([still_path fileSystemRepresentation], &still_status);
		still_ok = self.stillWritten && still_status.st_size > 4096 && status.st_size > 4096 && has_moov;
		[self finish:still_ok reason:[NSString stringWithFormat:
		    @"still %lld bytes, movie %lld bytes, moov %s", (long long)still_status.st_size,
		    (long long)status.st_size, has_moov ? "present" : "MISSING"]];
	}
}

- (void)finish:(BOOL)passed reason:(NSString *)reason
{
	if (self.finished) {
		return;
	}
	self.finished = YES;
	app_say(@"finish: %s -- %@", passed ? "PASS" : "FAIL", reason);
	result_set(@"result", @"%s", passed ? "PASS" : "FAIL");
	result_set(@"reason", @"%@", reason);
	result_set(@"ended", @"%lld", (long long)time(NULL));
	result_write();
	if (self.session != nil && self.session.isRunning) {
		[self.session stopRunning];
	}
	/* the camera belongs to the next stage: leave now rather than linger in the background */
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)),
	    dispatch_get_main_queue(), ^{
		app_say(@"exiting");
		fflush(stderr);
		exit(passed ? 0 : 1);
	});
}

@end

int
main(int argc, char *argv[])
{
	setvbuf(stderr, NULL, _IONBF, 0);
	@autoreleasepool {
		return UIApplicationMain(argc, argv, nil, NSStringFromClass([CameraTest class]));
	}
}
