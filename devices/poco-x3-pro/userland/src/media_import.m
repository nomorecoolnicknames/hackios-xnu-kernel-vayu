/*
 * media_import: put files into the Files app's "On My iPhone" and images into Photos; usage:
 * media_import [--wait SECONDS] [--files FILE...] [--photos IMAGE_OR_VIDEO...]. Each list runs until the
 * next option and at least one list is needed. Root waits, becomes mobile, then does the work.
 */
#import <Foundation/Foundation.h>
#import <Photos/Photos.h>
#import <objc/message.h>
#include <sys/wait.h>
#include <errno.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/stat.h>

extern char **environ;

#define MOBILE_ID	501
#define UTF8(s) ((s) ? [(s) UTF8String] : "(none)")

static int usage(void) {
	fprintf(stderr, "media_import: usage: media_import [--wait SECONDS] [--files FILE...] [--photos IMAGE...]\n"); return 2;
}
/* Everything below runs as mobile so the containers and the photo library are the right ones. */
static int become_mobile(void) {
	fprintf(stderr, "media_import: becoming mobile (501:501)\n");
	(void)setgroups(0, NULL);
	if (setgid(MOBILE_ID) != 0 || setuid(MOBILE_ID) != 0) {
		fprintf(stderr, "media_import: setgid/setuid: %s\n", strerror(errno)); return -1;
	}
	if (setenv("HOME", "/var/mobile", 1) != 0 || setenv("CFFIXED_USER_HOME", "/var/mobile", 1) != 0) {
		fprintf(stderr, "media_import: setenv: %s\n", strerror(errno)); return -1;
	}
	return 0;
}
/* One file into "On My iPhone", under its own name; an existing copy is left alone. */
static int import_file(NSFileManager *manager, NSString *directory, const char *path) {
	NSString *source = [NSString stringWithUTF8String:path];
	NSString *dest;
	NSError *error = nil;

	if (source == nil) { fprintf(stderr, "media_import: %s: not UTF-8\n", path); return -1; }
	dest = [directory stringByAppendingPathComponent:[source lastPathComponent]];
	if ([manager fileExistsAtPath:dest]) {
		fprintf(stderr, "media_import: %s: already there, skipped\n", UTF8(dest)); return 0;
	}
	if (![manager copyItemAtPath:source toPath:dest error:&error]) {
		fprintf(stderr, "media_import: %s: %s\n", path, UTF8([error localizedDescription])); return -1;
	}
	fprintf(stderr, "media_import: copied %s to %s\n", path, UTF8(dest));
	return 0;
}
/*
 * Why Files lists nothing (run 005616: the copies are there, no LocalStorageFileProvider ever started): is the
 * extension known to the system at all, and what does fileproviderd say about its domains.
 */
/* a command's output into the log, its first lines, each prefixed with the command's last word */
static void
run_command(char *const args[], int keep)
{
	int pipe_fds[2];
	pid_t pid;
	posix_spawn_file_actions_t actions;
	char line[400], label[64];
	FILE *out;
	int lines = 0;

	snprintf(label, sizeof(label), "%s %s%s%s", strrchr(args[0], '/') ? strrchr(args[0], '/') + 1 : args[0], args[1] ? args[1] : "",
	    args[1] && args[2] ? " " : "", args[1] && args[2] ? "LocalStorage" : "");
	if (pipe(pipe_fds) != 0) {
		return;
	}
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], 1);
	posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], 2);
	posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
	if (posix_spawn(&pid, args[0], &actions, NULL, args, environ) != 0) {
		fprintf(stderr, "media_import: %s: cannot start it\n", label);
		close(pipe_fds[0]);
		close(pipe_fds[1]);
		posix_spawn_file_actions_destroy(&actions);
		return;
	}
	close(pipe_fds[1]);
	out = fdopen(pipe_fds[0], "r");
	while (out != NULL && fgets(line, sizeof(line), out) != NULL) {
		if (lines++ < keep) {
			fprintf(stderr, "media_import: %s: %s", label, line);
		}
	}
	if (out != NULL) {
		fclose(out);
	}
	waitpid(pid, NULL, 0);
	fprintf(stderr, "media_import: %s: %d lines\n", label, lines);
	posix_spawn_file_actions_destroy(&actions);
}

static void
files_diagnose(NSString *directory)
{
	Class extension_class = NSClassFromString(@"NSExtension");
	NSError *error = nil;
	id extension = nil;
	struct stat st;
	pid_t pid;

	if (stat([directory fileSystemRepresentation], &st) == 0) {
		fprintf(stderr, "media_import: %s: owner %u:%u, mode %o\n", UTF8(directory), st.st_uid, st.st_gid,
		    st.st_mode & 07777);
	}
	if (extension_class != Nil && [extension_class respondsToSelector:@selector(extensionWithIdentifier:error:)]) {
		extension = ((id (*)(id, SEL, id, NSError **))objc_msgSend)(extension_class,
		    @selector(extensionWithIdentifier:error:), @"com.apple.FileProvider.LocalStorage", &error);
	}
	NSString *why = extension ? @"" : [NSString stringWithFormat:@" (%@)", error];

	fprintf(stderr, "media_import: extension com.apple.FileProvider.LocalStorage: %s%s\n",
	    extension ? UTF8([extension description]) : "NOT FOUND", UTF8(why));
	/* fileproviderd's own view: its dump, the LocalStorage provider's own dump, and what it enumerates there */
	{
		char *dump[] = { "/usr/bin/fileproviderctl", "dump", NULL };
		char *dump_local[] = { "/usr/bin/fileproviderctl", "dump", "com.apple.FileProvider.LocalStorage", NULL };
		char *ls_local[] = { "/usr/bin/fileproviderctl", "ls", "com.apple.FileProvider.LocalStorage", NULL };

		run_command(dump, 200);
		run_command(dump_local, 200);
		run_command(ls_local, 60);
	}
}

static int import_files(const char **files, int count) {
	NSFileManager *manager = [NSFileManager defaultManager];
	NSURL *container = [manager containerURLForSecurityApplicationGroupIdentifier:
	    @"group.com.apple.FileProvider.LocalStorage"];
	NSString *directory;
	NSError *error = nil;
	int index, status = 0;

	if (container == nil || [container path] == nil) {
		fprintf(stderr, "media_import: no container\n"); return -1;
	}
	fprintf(stderr, "media_import: container %s\n", UTF8([container path]));
	directory = [[container path] stringByAppendingPathComponent:@"File Provider Storage"];
	if (![manager fileExistsAtPath:directory]) {
		if (![manager createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil
		    error:&error]) {
			fprintf(stderr, "media_import: %s: %s\n", UTF8(directory),
			    UTF8([error localizedDescription])); return -1;
		}
		fprintf(stderr, "media_import: made %s\n", UTF8(directory));
	}
	for (index = 0; index < count; index++) {
		if (import_file(manager, directory, files[index]) != 0) status = -1;
	}
	files_diagnose(directory);
	return status;
}
/* A creation request inside performChangesAndWait:; the block runs before the call returns. */
static int import_photo(PHPhotoLibrary *library, const char *path) {
	NSString *image = [NSString stringWithUTF8String:path];
	NSError *error = nil;
	BOOL ok;

	if (image == nil) { fprintf(stderr, "media_import: %s: not UTF-8\n", path); return -1; }
	ok = [library performChangesAndWait:^{
		NSURL *url = [NSURL fileURLWithPath:image];
		NSString *ext = [[image pathExtension] lowercaseString];

		/* a video (.mp4, .mov, .m4v) goes in as one, anything else as a picture */
		if ([ext isEqualToString:@"mp4"] || [ext isEqualToString:@"mov"] || [ext isEqualToString:@"m4v"]) {
			[PHAssetChangeRequest creationRequestForAssetFromVideoAtFileURL:url];
		} else {
			[PHAssetChangeRequest creationRequestForAssetFromImageAtFileURL:url];
		}
	} error:&error];
	if (!ok) {
		fprintf(stderr, "media_import: %s: %s\n", path, UTF8([error localizedDescription])); return -1;
	}
	fprintf(stderr, "media_import: added %s to Photos\n", path);
	return 0;
}
static int import_photos(const char **images, int count) {
	PHPhotoLibrary *library = [PHPhotoLibrary sharedPhotoLibrary];
	int index, status = 0;

	fprintf(stderr, "media_import: photo access status %d\n",
	    (int)[PHPhotoLibrary authorizationStatusForAccessLevel:PHAccessLevelReadWrite]);
	for (index = 0; index < count; index++) {
		if (import_photo(library, images[index]) != 0) status = -1;
	}
	return status;
}

static int import(int argc, char **argv) {
	const char **files = calloc((size_t)argc, sizeof(*files));
	const char **photos = calloc((size_t)argc, sizeof(*photos));
	double wait = 0;
	int index = 1, file_count = 0, photo_count = 0, bad = 0, status = 0;

	if (files == NULL || photos == NULL) {
		fprintf(stderr, "media_import: calloc: %s\n", strerror(errno)); free(files); free(photos); return 1;
	}
	while (index < argc && !bad) {
		if (strcmp(argv[index], "--wait") == 0) {
			if (index + 1 >= argc) bad = 1;
			else { wait = atof(argv[index + 1]); index += 2; }
		} else if (strcmp(argv[index], "--files") == 0) {
			index++;
			while (index < argc && strncmp(argv[index], "--", 2) != 0) files[file_count++] = argv[index++];
		} else if (strcmp(argv[index], "--photos") == 0) {
			index++;
			while (index < argc && strncmp(argv[index], "--", 2) != 0) photos[photo_count++] = argv[index++];
		} else {
			fprintf(stderr, "media_import: bad argument \"%s\"\n", argv[index]); bad = 1;
		}
	}
	if (!bad && file_count == 0 && photo_count == 0) bad = 1;
	if (bad) { free(files); free(photos); return usage(); }
	if (wait > 0) { fprintf(stderr, "media_import: waiting %d s\n", (int)wait); sleep((unsigned)wait); }
	if (become_mobile() != 0) { free(files); free(photos); return 1; }
	if (file_count > 0 && import_files(files, file_count) != 0) status = 1;
	if (photo_count > 0 && import_photos(photos, photo_count) != 0) status = 1;
	fprintf(stderr, "media_import: %s\n", status == 0 ? "done" : "finished with errors");
	free(files);
	free(photos);
	return status;
}

int main(int argc, char **argv) {
	int status;

	@autoreleasepool {
		status = import(argc, argv);
	}
	return status;
}
