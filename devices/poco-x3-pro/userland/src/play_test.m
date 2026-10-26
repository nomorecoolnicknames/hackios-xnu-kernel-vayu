/*
 * play_test: does this system decode and play an MP3 at all, with no app in between?
 *
 * usage: play_test [--wait SECONDS] FILE [SECONDS_TO_PLAY]
 *
 * As mobile (the user an app runs as), in two steps, each named in the log, under a watchdog:
 *   1. decode: ExtAudioFile opens FILE and reads its first two seconds as 16-bit PCM; prints the file's format
 *      and the peak of what was decoded (0 = the decoder gave silence or nothing)
 *   2. play: AVAudioPlayer plays FILE for SECONDS_TO_PLAY (default 20) on the speaker, reporting where it is
 *      every two seconds; the kernel's own lines (vayu-audio: iOS plays …, peak) say whether it was heard
 * Run 0301: ringtones played, a tester's MP3 did not, neither in Filza nor in Music.
 */
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MOBILE_ID	501

static const char *volatile step = "start";

static void
say_step(const char *what)
{
	step = what;
	fprintf(stderr, "play_test: step: %s\n", what);
}

static void
died(int sig)
{
	char line[160];
	int n = snprintf(line, sizeof(line), "play_test: %s in step \"%s\"\n",
	    sig == SIGALRM ? "TIMED OUT" : sig == SIGSEGV ? "SIGSEGV" : sig == SIGBUS ? "SIGBUS" : "SIGABRT", step);

	write(2, line, n > 0 && n < (int)sizeof(line) ? (size_t)n : sizeof(line) - 1);
	_exit(sig == SIGALRM ? 3 : 4);
}

static void
decode(NSURL *url)
{
	ExtAudioFileRef file = NULL;
	AudioStreamBasicDescription in = { 0 }, out = { 0 };
	UInt32 size = sizeof(in);
	OSStatus s;
	static int16_t pcm[4096 * 2];
	UInt64 frames = 0;
	int peak = 0;

	say_step("decode: ExtAudioFileOpenURL");
	s = ExtAudioFileOpenURL((__bridge CFURLRef)url, &file);
	if (s != noErr) {
		fprintf(stderr, "play_test: ExtAudioFileOpenURL: %d\n", (int)s);
		return;
	}
	ExtAudioFileGetProperty(file, kExtAudioFileProperty_FileDataFormat, &size, &in);
	fprintf(stderr, "play_test: file format '%c%c%c%c', %.0f Hz, %u channels\n", (char)(in.mFormatID >> 24),
	    (char)(in.mFormatID >> 16), (char)(in.mFormatID >> 8), (char)in.mFormatID, in.mSampleRate,
	    (unsigned)in.mChannelsPerFrame);
	out = (AudioStreamBasicDescription) { .mSampleRate = in.mSampleRate, .mFormatID = kAudioFormatLinearPCM,
		.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked, .mBytesPerPacket = 4,
		.mFramesPerPacket = 1, .mBytesPerFrame = 4, .mChannelsPerFrame = 2, .mBitsPerChannel = 16 };
	say_step("decode: client format (16-bit PCM)");
	s = ExtAudioFileSetProperty(file, kExtAudioFileProperty_ClientDataFormat, sizeof(out), &out);
	if (s != noErr) {
		fprintf(stderr, "play_test: client format: %d (no converter for this format?)\n", (int)s);
		ExtAudioFileDispose(file);
		return;
	}
	say_step("decode: reading two seconds");
	while (frames < (UInt64)(2 * in.mSampleRate)) {
		AudioBufferList list = { .mNumberBuffers = 1,
			.mBuffers = { { .mNumberChannels = 2, .mDataByteSize = sizeof(pcm), .mData = pcm } } };
		UInt32 n = sizeof(pcm) / 4;

		s = ExtAudioFileRead(file, &n, &list);
		if (s != noErr || n == 0) {
			fprintf(stderr, "play_test: ExtAudioFileRead: %d after %llu frames\n", (int)s, frames);
			break;
		}
		for (UInt32 i = 0; i < n * 2; i++) {
			int v = pcm[i] < 0 ? -pcm[i] : pcm[i];
			peak = v > peak ? v : peak;
		}
		frames += n;
	}
	fprintf(stderr, "play_test: decoded %llu frames, peak %d of 32768%s\n", frames, peak,
	    peak == 0 ? " (SILENCE: the decoder gave nothing)" : "");
	ExtAudioFileDispose(file);
}

static void
play(NSURL *url, double seconds)
{
	NSError *error = nil;
	AVAudioPlayer *player;

	say_step("play: audio session (playback)");
	[[AVAudioSession sharedInstance] setCategory:AVAudioSessionCategoryPlayback error:&error];
	if (error != nil) {
		fprintf(stderr, "play_test: setCategory: %s\n", [[error description] UTF8String]);
		error = nil;
	}
	[[AVAudioSession sharedInstance] setActive:YES error:&error];
	if (error != nil) {
		fprintf(stderr, "play_test: setActive: %s\n", [[error description] UTF8String]);
		error = nil;
	}
	say_step("play: AVAudioPlayer");
	player = [[AVAudioPlayer alloc] initWithContentsOfURL:url error:&error];
	if (player == nil) {
		fprintf(stderr, "play_test: AVAudioPlayer: %s\n", [[error description] UTF8String]);
		return;
	}
	fprintf(stderr, "play_test: duration %.1f s, prepare %d, play %d\n", player.duration, (int)[player prepareToPlay],
	    (int)[player play]);
	for (double t = 0; t < seconds; t += 2) {
		CFRunLoopRunInMode(kCFRunLoopDefaultMode, 2, false);
		fprintf(stderr, "play_test: +%.0f s: playing %d at %.1f s\n", t + 2, (int)player.playing, player.currentTime);
	}
	[player stop];
	say_step("play: done");
}

int
main(int argc, char **argv)
{
	int i = 1;
	double wait = 0, seconds = 20;

	if (i + 1 < argc && strcmp(argv[i], "--wait") == 0) {
		wait = atof(argv[i + 1]);
		i += 2;
	}
	if (i >= argc) {
		fprintf(stderr, "usage: play_test [--wait SECONDS] FILE [SECONDS_TO_PLAY]\n");
		return 2;
	}
	if (i + 1 < argc) {
		seconds = atof(argv[i + 1]);
	}
	if (wait > 0) {
		sleep((unsigned)wait);
	}
	if (getuid() == 0 && (setgid(MOBILE_ID) != 0 || setuid(MOBILE_ID) != 0)) {
		fprintf(stderr, "play_test: cannot become mobile\n");
		return 1;
	}
	setenv("HOME", "/var/mobile", 1);
	setenv("CFFIXED_USER_HOME", "/var/mobile", 1);
	signal(SIGALRM, died);
	signal(SIGSEGV, died);
	signal(SIGBUS, died);
	signal(SIGABRT, died);
	@autoreleasepool {
		NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[i]]];

		alarm(60);
		decode(url);
		alarm((unsigned)seconds + 60);
		play(url, seconds);
		alarm(0);
	}
	fprintf(stderr, "play_test: done\n");
	return 0;
}
