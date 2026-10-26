/*
 * video_test: what video can this system decode, play and encode, with no app in between?
 *
 * usage: video_test [--wait SECONDS] FILE...
 *        video_test [--wait SECONDS] encoders
 *
 * As mobile (the user an app runs as), for every FILE, each step named in the log, under a watchdog:
 *   1. info: the first video track's size, frame rate, codec and data rate
 *   2. decode hw / decode sw: AVAssetReader feeds up to 300 compressed frames to a VTDecompressionSession
 *      asked to require hardware, then to disable it; frames, errors, time and fps are printed
 *   3. player: AVPlayer plays the file muted for min(duration, 12) s; a video output counts delivered
 *      pixel buffers, then the item's access and error logs are printed
 *   4. encode: H.264 and HEVC, hardware-only and software-only, 120 frames of a 1080p gradient
 *   5. encoders: HEVC and H.264, 1080p and 720p, each encoder specification, with and without RealTime,
 *      60 frames each; one line per combination says how it went
 */
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <VideoToolbox/VideoToolbox.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MOBILE_ID	501
/*
 * The hardware keys of VideoToolbox are macOS-only in the iOS 16.5 SDK (API_UNAVAILABLE(ios)); their values are
 * the plain strings below, which VideoToolbox looks up by value, so they are used as strings here.
 */
#define PART_ALARM	120

static const char *volatile step = "start";

static void
say_step(const char *what)
{
	step = what;
	fprintf(stderr, "video_test: step: %s\n", what);
}

static void
died(int sig)
{
	char line[160];
	int n = snprintf(line, sizeof(line), "video_test: %s in step \"%s\"\n",
	    sig == SIGALRM ? "TIMED OUT" : sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : "SIGABRT", step);

	write(2, line, n > 0 && n < (int)sizeof(line) ? (size_t)n : sizeof(line) - 1);
	_exit(sig == SIGALRM ? 3 : 4);
}

static void
info(NSURL *url)
{
	AVURLAsset *asset = [AVURLAsset URLAssetWithURL:url options:nil];
	AVAssetTrack *track = [[asset tracksWithMediaType:AVMediaTypeVideo] firstObject];
	CMFormatDescriptionRef format;
	FourCharCode codec;
	CGSize size;
	float fps, rate;

	say_step("info: AVAssetTrack");
	if (track == nil) {
		fprintf(stderr, "video_test: info: no video track\n");
		return;
	}
	format = (__bridge CMFormatDescriptionRef)[track.formatDescriptions firstObject];
	if (format == NULL) {
		fprintf(stderr, "video_test: info: no format description\n");
		return;
	}
	size = track.naturalSize;
	fps = track.nominalFrameRate;
	rate = track.estimatedDataRate;
	codec = CMFormatDescriptionGetMediaSubType(format);
	fprintf(stderr, "video_test: info: %.0fx%.0f, %.3f fps, codec '%c%c%c%c', %.0f bit/s\n",
	    size.width, size.height, fps, (char)(codec >> 24), (char)(codec >> 16), (char)(codec >> 8),
	    (char)codec, rate);
}

typedef struct {
	unsigned long frames;
	unsigned long errors;
} DecodeCount;

static void
decode_callback(void *refcon, void *source, OSStatus status, VTDecodeInfoFlags flags,
    CVImageBufferRef image, CMTime pts, CMTime duration)
{
	DecodeCount *count = refcon;

	(void)source; (void)flags; (void)pts; (void)duration;
	if (status != noErr || image == NULL)
		count->errors++;
	else
		count->frames++;
}

static void
decode(NSURL *url, int hardware)
{
	const char *mode = hardware ? "hw" : "sw";
	AVURLAsset *asset = [AVURLAsset URLAssetWithURL:url options:nil];
	AVAssetTrack *track = [[asset tracksWithMediaType:AVMediaTypeVideo] firstObject];
	VTDecompressionOutputCallbackRecord callback;
	VTDecompressionSessionRef session = NULL;
	AVAssetReader *reader;
	AVAssetReaderTrackOutput *output;
	CMFormatDescriptionRef format;
	NSDictionary *spec, *attrs;
	DecodeCount count = { 0, 0 };
	NSError *error = nil;
	double start, elapsed;
	OSStatus s;
	int n;

	if (track == nil) {
		fprintf(stderr, "video_test: decode %s: no video track\n", mode);
		return;
	}
	format = (__bridge CMFormatDescriptionRef)[track.formatDescriptions firstObject];
	if (format == NULL) {
		fprintf(stderr, "video_test: decode %s: no format description\n", mode);
		return;
	}
	say_step(hardware ? "decode hw: AVAssetReader" : "decode sw: AVAssetReader");
	reader = [AVAssetReader assetReaderWithAsset:asset error:&error];
	if (reader == nil) {
		fprintf(stderr, "video_test: decode %s: AVAssetReader: %s\n", mode,
		    error != nil ? [[error description] UTF8String] : "failed");
		return;
	}
	output = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track outputSettings:nil];
	if (output == nil) {
		fprintf(stderr, "video_test: decode %s: AVAssetReaderTrackOutput failed\n", mode);
		return;
	}
	[reader addOutput:output];
	if (![reader startReading]) {
		fprintf(stderr, "video_test: decode %s: startReading: %s\n", mode,
		    reader.error != nil ? [[reader.error description] UTF8String] : "failed");
		return;
	}
	if (hardware) {
		spec = @{ @"RequireHardwareAcceleratedVideoDecoder": @YES };
	} else {
		spec = @{ @"EnableHardwareAcceleratedVideoDecoder": @NO };
	}
	attrs = @{ (__bridge NSString *)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) };
	callback.decompressionOutputRefCon = &count;
	callback.decompressionOutputCallback = decode_callback;
	say_step(hardware ? "decode hw: VTDecompressionSessionCreate" : "decode sw: VTDecompressionSessionCreate");
	s = VTDecompressionSessionCreate(kCFAllocatorDefault, format, (__bridge CFDictionaryRef)spec,
	    (__bridge CFDictionaryRef)attrs, &callback, &session);
	if (s != noErr) {
		fprintf(stderr, "video_test: decode %s: VTDecompressionSessionCreate: %d\n", mode, (int)s);
		[reader cancelReading];
		return;
	}
	say_step(hardware ? "decode hw: 300 frames" : "decode sw: 300 frames");
	start = CFAbsoluteTimeGetCurrent();
	for (n = 0; n < 300; n++) {
		CMSampleBufferRef sample = [output copyNextSampleBuffer];
		VTDecodeInfoFlags info_flags = 0;

		if (sample == NULL)
			break;
		s = VTDecompressionSessionDecodeFrame(session, sample, 0, NULL, &info_flags);
		CFRelease(sample);
		if (s != noErr) {
			fprintf(stderr, "video_test: decode %s: VTDecompressionSessionDecodeFrame: %d after %d samples\n",
			    mode, (int)s, n);
			count.errors++;
			break;
		}
	}
	VTDecompressionSessionWaitForAsynchronousFrames(session);
	elapsed = CFAbsoluteTimeGetCurrent() - start;
	if (reader.status == AVAssetReaderStatusFailed)
		fprintf(stderr, "video_test: decode %s: reader failed: %s\n", mode,
		    reader.error != nil ? [[reader.error description] UTF8String] : "unknown");
	fprintf(stderr, "video_test: decode %s: %lu frames, %lu errors, %.2f s, %.1f fps\n", mode,
	    count.frames, count.errors, elapsed, elapsed > 0 ? count.frames / elapsed : 0.0);
	{
		CFTypeRef hw = NULL;
		OSStatus ps = VTSessionCopyProperty(session,
		    CFSTR("UsingHardwareAcceleratedVideoDecoder"), kCFAllocatorDefault, &hw);

		if (ps == noErr && hw != NULL && CFGetTypeID(hw) == CFBooleanGetTypeID())
			fprintf(stderr, "video_test: decode %s: UsingHardwareAcceleratedVideoDecoder: %s\n",
			    mode, CFBooleanGetValue(hw) ? "yes" : "no");
		else
			fprintf(stderr, "video_test: decode %s: hardware property unavailable (%d)\n", mode, (int)ps);
		if (hw != NULL)
			CFRelease(hw);
	}
	[reader cancelReading];
	VTDecompressionSessionInvalidate(session);
	CFRelease(session);
}

static void
player(NSURL *url)
{
	AVURLAsset *asset = [AVURLAsset URLAssetWithURL:url options:nil];
	AVPlayerItem *item = [AVPlayerItem playerItemWithAsset:asset];
	AVPlayer *avplayer = [AVPlayer playerWithPlayerItem:item];
	NSDictionary *attrs = @{ (__bridge NSString *)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange) };
	AVPlayerItemVideoOutput *output = [[AVPlayerItemVideoOutput alloc] initWithPixelBufferAttributes:attrs];
	AVPlayerItemAccessLogEvent *access_event;
	double duration = CMTimeGetSeconds(asset.duration);
	double seconds = duration > 0 && duration < 12 ? duration : 12;
	double start, elapsed;
	unsigned long buffers = 0;

	say_step("player: AVPlayerItemVideoOutput");
	[item addOutput:output];
	avplayer.volume = 0; avplayer.muted = YES;
	[avplayer play];
	start = CFAbsoluteTimeGetCurrent();
	say_step("player: playing");
	for (double t = 0; t < seconds; t += 0.5) {
		CMTime item_time;

		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, false);
		item_time = item.currentTime;
		if (CMTIME_IS_VALID(item_time) && [output hasNewPixelBufferForItemTime:item_time]) {
			CVPixelBufferRef pixel = [output copyPixelBufferForItemTime:item_time itemTimeForDisplay:NULL];

			if (pixel != NULL) {
				buffers++;
				CVPixelBufferRelease(pixel);
			}
		}
	}
	elapsed = CFAbsoluteTimeGetCurrent() - start;
	[avplayer pause];
	fprintf(stderr, "video_test: player: status %ld (%s), error %s, %lu buffers in %.2f s, %.1f fps\n",
	    (long)item.status,
	    item.status == AVPlayerItemStatusReadyToPlay ? "readyToPlay" :
	    item.status == AVPlayerItemStatusFailed ? "failed" : "unknown",
	    item.error != nil ? [[item.error description] UTF8String] : "none", buffers, elapsed,
	    elapsed > 0 ? buffers / elapsed : 0.0);
	access_event = item.accessLog.events.lastObject;
	if (access_event != nil)
		fprintf(stderr, "video_test: player: access log: %ld dropped, indicated %.0f bit/s, observed %.0f bit/s\n",
		    (long)access_event.numberOfDroppedVideoFrames, access_event.indicatedBitrate,
		    access_event.observedBitrate);
	else
		fprintf(stderr, "video_test: player: access log: no events\n");
	fprintf(stderr, "video_test: player: error log: %lu events\n", (unsigned long)item.errorLog.events.count);
	for (AVPlayerItemErrorLogEvent *event in item.errorLog.events)
		fprintf(stderr, "video_test: player: error log: status %ld, domain %s, comment %s\n",
		    (long)event.errorStatusCode, event.errorDomain != nil ? [event.errorDomain UTF8String] : "",
		    event.errorComment != nil ? [event.errorComment UTF8String] : "");
	[item removeOutput:output]; avplayer = nil; item = nil; output = nil;
}

typedef struct {
	unsigned long frames;
	unsigned long bytes;
	unsigned long errors;
} EncodeCount;

static void
encode_callback(void *refcon, void *source, OSStatus status, VTEncodeInfoFlags flags,
    CMSampleBufferRef sample)
{
	EncodeCount *count = refcon;
	CMBlockBufferRef block;

	(void)source; (void)flags;
	if (status != noErr || sample == NULL) {
		count->errors++;
		return;
	}
	count->frames++;
	block = CMSampleBufferGetDataBuffer(sample);
	if (block != NULL)
		count->bytes += (unsigned long)CMBlockBufferGetDataLength(block);
}

static void
fill_gradient(CVPixelBufferRef pixel, int frame)
{
	uint8_t *luma, *chroma;
	size_t stride, chroma_stride, chroma_height, width, height, x, y;

	CVPixelBufferLockBaseAddress(pixel, 0);
	width = CVPixelBufferGetWidth(pixel); height = CVPixelBufferGetHeight(pixel);
	luma = CVPixelBufferGetBaseAddressOfPlane(pixel, 0);
	stride = CVPixelBufferGetBytesPerRowOfPlane(pixel, 0);
	for (y = 0; y < height; y++) {
		uint8_t *row = luma + y * stride;

		for (x = 0; x < width; x++)
			row[x] = (uint8_t)((x + y + (size_t)frame * 4) & 0xff);
	}
	chroma = CVPixelBufferGetBaseAddressOfPlane(pixel, 1);
	chroma_stride = CVPixelBufferGetBytesPerRowOfPlane(pixel, 1);
	chroma_height = CVPixelBufferGetHeightOfPlane(pixel, 1);
	for (y = 0; y < chroma_height; y++)
		memset(chroma + y * chroma_stride, 128, chroma_stride);
	CVPixelBufferUnlockBaseAddress(pixel, 0);
}

static void
encode(CMVideoCodecType codec, int hardware)
{
	const char *name = codec == kCMVideoCodecType_HEVC ? "hevc" : "h264";
	const char *mode = hardware ? "hw" : "sw";
	NSDictionary *spec, *pool_attrs;
	VTCompressionSessionRef session = NULL;
	CVPixelBufferPoolRef pool = NULL;
	EncodeCount count = { 0, 0, 0 };
	double start, elapsed;
	OSStatus s;
	CVReturn cv;
	int frame;

	if (hardware) {
		spec = @{ @"RequireHardwareAcceleratedVideoEncoder": @YES };
	} else {
		spec = @{ @"EnableHardwareAcceleratedVideoEncoder": @NO };
	}
	pool_attrs = @{ (__bridge NSString *)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
	    (__bridge NSString *)kCVPixelBufferWidthKey: @1920, (__bridge NSString *)kCVPixelBufferHeightKey: @1080,
	    (__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey: @{} };
	say_step("encode: VTCompressionSessionCreate");
	s = VTCompressionSessionCreate(kCFAllocatorDefault, 1920, 1080, codec, (__bridge CFDictionaryRef)spec,
	    (__bridge CFDictionaryRef)pool_attrs, kCFAllocatorDefault, encode_callback, &count, &session);
	if (s != noErr) {
		fprintf(stderr, "video_test: encode %s %s: VTCompressionSessionCreate: %d\n", name, mode, (int)s);
		return;
	}
	s = VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanFalse);
	if (s != noErr)
		fprintf(stderr, "video_test: encode %s %s: RealTime: %d\n", name, mode, (int)s);
	s = VTSessionSetProperty(session, kVTCompressionPropertyKey_AverageBitRate, (__bridge CFTypeRef)@8000000);
	if (s != noErr)
		fprintf(stderr, "video_test: encode %s %s: AverageBitRate: %d\n", name, mode, (int)s);
	cv = CVPixelBufferPoolCreate(kCFAllocatorDefault, NULL, (__bridge CFDictionaryRef)pool_attrs, &pool);
	if (cv != kCVReturnSuccess || pool == NULL) {
		fprintf(stderr, "video_test: encode %s %s: CVPixelBufferPoolCreate: %d\n", name, mode, (int)cv);
		VTCompressionSessionInvalidate(session);
		CFRelease(session);
		return;
	}
	say_step("encode: 120 frames");
	start = CFAbsoluteTimeGetCurrent();
	for (frame = 0; frame < 120; frame++) {
		CVPixelBufferRef pixel = NULL;

		cv = CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pixel);
		if (cv != kCVReturnSuccess || pixel == NULL) {
			fprintf(stderr, "video_test: encode %s %s: CVPixelBufferPoolCreatePixelBuffer: %d at frame %d\n",
			    name, mode, (int)cv, frame);
			count.errors++;
			break;
		}
		fill_gradient(pixel, frame);
		s = VTCompressionSessionEncodeFrame(session, pixel, CMTimeMake(frame, 30), CMTimeMake(1, 30),
		    NULL, NULL, NULL);
		CVPixelBufferRelease(pixel);
		if (s != noErr) {
			fprintf(stderr, "video_test: encode %s %s: VTCompressionSessionEncodeFrame: %d at frame %d\n",
			    name, mode, (int)s, frame);
			count.errors++;
			break;
		}
	}
	say_step("encode: complete frames");
	s = VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
	if (s != noErr)
		fprintf(stderr, "video_test: encode %s %s: VTCompressionSessionCompleteFrames: %d\n", name, mode, (int)s);
	elapsed = CFAbsoluteTimeGetCurrent() - start;
	fprintf(stderr, "video_test: encode %s %s: %lu frames, %lu errors, %lu bytes, %.2f s, %.1f fps\n",
	    name, mode, count.frames, count.errors, count.bytes, elapsed,
	    elapsed > 0 ? count.frames / elapsed : 0.0);
	{
		CFTypeRef hw = NULL;
		OSStatus ps = VTSessionCopyProperty(session,
		    CFSTR("UsingHardwareAcceleratedVideoEncoder"), kCFAllocatorDefault, &hw);

		if (ps == noErr && hw != NULL && CFGetTypeID(hw) == CFBooleanGetTypeID())
			fprintf(stderr, "video_test: encode %s %s: UsingHardwareAcceleratedVideoEncoder: %s\n",
			    name, mode, CFBooleanGetValue(hw) ? "yes" : "no");
		else
			fprintf(stderr, "video_test: encode %s %s: hardware property unavailable (%d)\n",
			    name, mode, (int)ps);
		if (hw != NULL)
			CFRelease(hw);
	}
	CVPixelBufferPoolRelease(pool);
	VTCompressionSessionInvalidate(session);
	CFRelease(session);
}

/*
 * encoders: the encoder capability matrix.  HEVC and H.264 at 1080p and 720p, with no specification, the enable
 * and require hardware specifications and the HEVC vcp encoder id, with and without RealTime: every combination
 * gets a fresh session, 60 frames of a moving gradient go through it, and one line says whether the session was
 * created, how many frames came back, how long a frame took and whether the first sample carried parameter sets.
 * The specification keys are strings for the reason given at the top of the file.
 */
typedef struct {
	unsigned long frames;
	unsigned long errors;
	long paramsets;		/* -1 until the first sample, then its parameter set count, or -2 if the query failed */
} EncoderCount;

static void
encoders_callback(void *refcon, void *source, OSStatus status, VTEncodeInfoFlags flags,
    CMSampleBufferRef sample)
{
	EncoderCount *count = refcon;
	CMFormatDescriptionRef format;
	size_t sets = 0;
	OSStatus s;

	(void)source; (void)flags;
	if (status != noErr || sample == NULL) {
		count->errors++;
		return;
	}
	count->frames++;
	if (count->paramsets != -1)
		return;
	format = CMSampleBufferGetFormatDescription(sample);
	if (format == NULL) {
		count->paramsets = 0;
		return;
	}
	if (CMFormatDescriptionGetMediaSubType(format) == kCMVideoCodecType_HEVC)
		s = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(format, 0, NULL, NULL, &sets, NULL);
	else
		s = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(format, 0, NULL, NULL, &sets, NULL);
	count->paramsets = s == noErr ? (long)sets : -2;
}

/*
 * The watchdog handler reads the step string after the function that set it has returned, so the text lives in a
 * static buffer and only complete strings are handed to say_step.
 */
static void
encoders_step(const char *phase, const char *name, int width, int height, const char *spec_name, int realtime)
{
	static char what[160];
	char line[160];

	snprintf(line, sizeof(line), "encoders %s: %s %dx%d %s realtime %s", phase, name, width, height,
	    spec_name, realtime ? "yes" : "no");
	strlcpy(what, line, sizeof(what));
	say_step(what);
}

static void
encoders_line(const char *name, int width, int height, const char *spec_name, int realtime,
    OSStatus create, unsigned long frames, unsigned long errors, double elapsed, long paramsets)
{
	char params[32];

	if (paramsets >= 0)
		snprintf(params, sizeof(params), "%ld", paramsets);
	else if (paramsets == -1)
		snprintf(params, sizeof(params), "no-sample");
	else
		snprintf(params, sizeof(params), "query-failed");
	fprintf(stderr, "video_test: encoders: codec %s, size %dx%d, spec %s, realtime %s, create %d, "
	    "frames %lu, errors %lu, mean %.2f ms/frame, params %s\n",
	    name, width, height, spec_name, realtime ? "yes" : "no", (int)create, frames, errors,
	    frames > 0 ? elapsed * 1000.0 / (double)frames : 0.0, params);
}

static void
encoders_case(CMVideoCodecType codec, const char *name, int width, int height, const char *spec_name,
    NSDictionary *spec, int realtime)
{
	const char *rt = realtime ? "yes" : "no";
	NSDictionary *attrs;
	VTCompressionSessionRef session = NULL;
	EncoderCount count = { 0, 0, -1 };
	NSNumber *bitrate;
	double start, elapsed;
	OSStatus s;
	CVReturn cv;
	int frame;

	attrs = @{ (__bridge NSString *)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
	    (__bridge NSString *)kCVPixelBufferWidthKey: @(width),
	    (__bridge NSString *)kCVPixelBufferHeightKey: @(height),
	    (__bridge NSString *)kCVPixelBufferIOSurfacePropertiesKey: @{} };
	bitrate = @(width >= 1920 ? 8000000 : 4000000);
	encoders_step("VTCompressionSessionCreate", name, width, height, spec_name, realtime);
	s = VTCompressionSessionCreate(kCFAllocatorDefault, (int32_t)width, (int32_t)height, codec,
	    (__bridge CFDictionaryRef)spec, (__bridge CFDictionaryRef)attrs, kCFAllocatorDefault,
	    encoders_callback, &count, &session);
	if (s != noErr || session == NULL) {
		encoders_line(name, width, height, spec_name, realtime, s, 0, 0, 0.0, -1);
		return;
	}
	if (realtime) {
		s = VTSessionSetProperty(session, kVTCompressionPropertyKey_RealTime, kCFBooleanTrue);
		if (s != noErr)
			fprintf(stderr, "video_test: encoders: %s %dx%d %s realtime %s: RealTime: %d\n",
			    name, width, height, spec_name, rt, (int)s);
	}
	s = VTSessionSetProperty(session, kVTCompressionPropertyKey_ExpectedFrameRate, (__bridge CFTypeRef)@30);
	if (s != noErr)
		fprintf(stderr, "video_test: encoders: %s %dx%d %s realtime %s: ExpectedFrameRate: %d\n",
		    name, width, height, spec_name, rt, (int)s);
	s = VTSessionSetProperty(session, kVTCompressionPropertyKey_AverageBitRate, (__bridge CFTypeRef)bitrate);
	if (s != noErr)
		fprintf(stderr, "video_test: encoders: %s %dx%d %s realtime %s: AverageBitRate: %d\n",
		    name, width, height, spec_name, rt, (int)s);
	encoders_step("60 frames", name, width, height, spec_name, realtime);
	start = CFAbsoluteTimeGetCurrent();
	for (frame = 0; frame < 60; frame++) {
		CVPixelBufferRef pixel = NULL;

		cv = CVPixelBufferCreate(kCFAllocatorDefault, (size_t)width, (size_t)height,
		    kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, (__bridge CFDictionaryRef)attrs, &pixel);
		if (cv != kCVReturnSuccess || pixel == NULL) {
			fprintf(stderr, "video_test: encoders: %s %dx%d %s realtime %s: CVPixelBufferCreate: %d at frame %d\n",
			    name, width, height, spec_name, rt, (int)cv, frame);
			count.errors++;
			break;
		}
		fill_gradient(pixel, frame);
		s = VTCompressionSessionEncodeFrame(session, pixel, CMTimeMake(frame, 30), CMTimeMake(1, 30),
		    NULL, NULL, NULL);
		CVPixelBufferRelease(pixel);
		if (s != noErr) {
			fprintf(stderr, "video_test: encoders: %s %dx%d %s realtime %s: VTCompressionSessionEncodeFrame: %d at frame %d\n",
			    name, width, height, spec_name, rt, (int)s, frame);
			count.errors++;
			break;
		}
	}
	encoders_step("complete frames", name, width, height, spec_name, realtime);
	s = VTCompressionSessionCompleteFrames(session, kCMTimeInvalid);
	if (s != noErr)
		fprintf(stderr, "video_test: encoders: %s %dx%d %s realtime %s: VTCompressionSessionCompleteFrames: %d\n",
		    name, width, height, spec_name, rt, (int)s);
	elapsed = CFAbsoluteTimeGetCurrent() - start;
	encoders_line(name, width, height, spec_name, realtime, noErr, count.frames, count.errors, elapsed,
	    count.paramsets);
	VTCompressionSessionInvalidate(session);
	CFRelease(session);
}

static void
encoders(void)
{
	static const CMVideoCodecType codecs[] = { kCMVideoCodecType_HEVC, kCMVideoCodecType_H264 };
	static const char *const codec_names[] = { "hevc", "h264" };
	static const struct { int width, height; } sizes[] = { { 1920, 1080 }, { 1280, 720 } };
	static const char *const spec_names[] = { "none", "enable-hw", "require-hw", "encoder-id" };
	size_t c, z, k;
	int realtime;

	for (c = 0; c < sizeof(codecs) / sizeof(codecs[0]); c++) {
		for (z = 0; z < sizeof(sizes) / sizeof(sizes[0]); z++) {
			int specs = codecs[c] == kCMVideoCodecType_HEVC ? 4 : 3;

			for (k = 0; k < (size_t)specs; k++) {
				NSDictionary *spec = nil;

				if (k == 1)
					spec = @{ @"EnableHardwareAcceleratedVideoEncoder": @YES };
				else if (k == 2)
					spec = @{ @"RequireHardwareAcceleratedVideoEncoder": @YES };
				else if (k == 3)
					spec = @{ @"EncoderID": @"com.apple.videotoolbox.videoencoder.hevc.vcp" };
				for (realtime = 0; realtime < 2; realtime++) {
					alarm(PART_ALARM);
					encoders_case(codecs[c], codec_names[c], sizes[z].width, sizes[z].height,
					    spec_names[k], spec, realtime);
				}
			}
		}
	}
	alarm(0);
}

int
main(int argc, char **argv)
{
	int i = 1;
	double wait = 0;
	int f;

	if (i + 1 < argc && strcmp(argv[i], "--wait") == 0) {
		wait = atof(argv[i + 1]);
		i += 2;
	}
	if (i >= argc) {
		fprintf(stderr, "usage: video_test [--wait SECONDS] FILE...\n"
		    "       video_test [--wait SECONDS] encoders\n");
		return 2;
	}
	if (wait > 0)
		sleep((unsigned)wait);
	if (getuid() == 0 && (setgid(MOBILE_ID) != 0 || setuid(MOBILE_ID) != 0)) {
		fprintf(stderr, "video_test: cannot become mobile\n");
		return 1;
	}
	setenv("HOME", "/var/mobile", 1);
	setenv("CFFIXED_USER_HOME", "/var/mobile", 1);
	signal(SIGALRM, died);
	signal(SIGSEGV, died);
	signal(SIGBUS, died);
	signal(SIGABRT, died);
	@autoreleasepool {
		if (i < argc && strcmp(argv[i], "encoders") == 0) {
			alarm(PART_ALARM);
			encoders();
			alarm(0);
			fprintf(stderr, "video_test: done\n");
			return 0;
		}
		for (f = i; f < argc; f++) {
			NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[f]]];

			fprintf(stderr, "video_test: === %s ===\n", argv[f]);
			alarm(PART_ALARM);
			info(url);
			alarm(PART_ALARM);
			decode(url, 1);
			alarm(PART_ALARM);
			decode(url, 0);
			alarm(PART_ALARM);
			player(url);
			alarm(0);
		}
		alarm(PART_ALARM);
		encode(kCMVideoCodecType_H264, 1);
		alarm(PART_ALARM);
		encode(kCMVideoCodecType_H264, 0);
		alarm(PART_ALARM);
		encode(kCMVideoCodecType_HEVC, 1);
		alarm(PART_ALARM);
		encode(kCMVideoCodecType_HEVC, 0);
		alarm(0);
	}
	fprintf(stderr, "video_test: done\n");
	return 0;
}
