/*
 * libnfcrestore.dylib: stop nfcd's NFCC firmware-restore retry loop on vayu (NXP PN553).
 *
 * WHAT HAPPENS
 * ------------
 * nfcd opens the PN553 through the AppleStockholm bridge (vayu_nfc_ios=2).  The stock NXP
 * stack then sends the proprietary PROP_SET_CONFIG for the HSU baud rate,
 *
 *     2F 03 08 10 00 00 5A 05 DC 02 04
 *
 * and the bridge rejects every 2Fxx except 2F02, so LibNfc prints "Configure HSU baud rate
 * failed!", the stack does not start (NFDriverLoadStack status 0x10) and nfcd treats the
 * NFCC as one that needs a firmware restore.  There is no restore path on this phone:
 *
 *   - com.apple.nfrestore is not one of the jobs in /redwood/LaunchDaemons, so the lookup
 *     fails with error 3 (No such process) before any helper runs;
 *   - even with the job, nfrestore_service would read
 *     /usr/standalone/firmware/nfrestore/firmware (a string in its binary), and
 *     sysimage/select-system.py deliberately leaves usr/standalone/ out of the system image;
 *   - the bridge refuses SetDownloadRequest(1), so the PN553 is never put in download mode.
 *
 * The restore therefore always fails, and -[NFDriverWrapper scheduleNFCCRecoveryOnQueue:
 * onSuccess:] asks -[NFDriverWrapper nextRetryTime] how long to wait.  That method indexes a
 * table with the wrapper's retry index; entry 0 is 0 seconds, so the retry is a busy loop
 * (669 524 "Restoring NFCC" lines and 334 848 "Retrying recovery in 0.000000 seconds" lines
 * in the TEST5Q log; the first iterations are 1.7-2.0 ms apart).
 *
 * WHAT THIS DOES
 * --------------
 * Two exits that do not need firmware, both private methods of nfcd 20F66:
 *
 *   -[NFDriverWrapper nextRetryTime] returns 86400 seconds.  This is the value the
 *   scheduler prints as "Retrying recovery in %lf seconds"; after the first failure the
 *   next attempt is a day away instead of at once, whatever entry point the timer uses.
 *
 *   -[NFDriverWrapper _attemptNFCCRecovery] returns nil, the same value its own
 *   "Skipping NFCC recovery" branch returns.  If the timer does enter recovery through this
 *   method, it no longer calls +[NFRestoreService restoreNFCC:] and no longer produces a
 *   failure that schedules another attempt.  If the entry point is different, the first
 *   hook still stops the busy loop.  Its log is limited to the first eight calls, so a tight
 *   caller cannot replace one flood with another.
 *
 * Both are process-local replacements: class_copyMethodList returns only the class's own
 * methods, so an inherited method is never replaced; nothing on disk is modified.  Removing
 * the INJECT line from userland/build.sh reverts it on the next boot.
 *
 * The dylib is loaded by the kernel's kern.exec_inject path (etc/inject.conf), the same way
 * libhakios/libbt reach SpringBoard and bluetoothd.
 */
#import <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>

#define NF_RETRY_SECONDS        86400.0
#define NF_SKIP_LOGS            8

static unsigned int nf_skipped;

static id
nf_skip_attempt(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	if (nf_skipped++ < NF_SKIP_LOGS) {
		fprintf(stderr, "libnfcrestore: -[NFDriverWrapper _attemptNFCCRecovery] skipped "
		    "(PN553: no nfrestore service/firmware; the bridge refuses download mode)\n");
	}
	return nil;
}

static double
nf_slow_retry(id self, SEL _cmd)
{
	(void)self;
	(void)_cmd;
	return NF_RETRY_SECONDS;
}

/*
 * Replace a method only when it is implemented by this class itself.  class_copyMethodList
 * returns the class's own methods and not the ones it inherits; replacing an inherited method
 * would change every subclass, which is not what this shim is for.
 */
static Method
nf_own_method(Class cls, SEL selector)
{
	unsigned int count = 0;
	Method *list = class_copyMethodList(cls, &count);
	Method found = NULL;

	for (unsigned int i = 0; i < count; i++) {
		if (method_getName(list[i]) == selector) {
			found = list[i];
			break;
		}
	}
	free(list);
	return found;
}

static void
nf_replace(const char *class_name, const char *selector_name, IMP replacement, const char *what)
{
	Class cls = objc_getClass(class_name);
	Method method;

	if (cls == Nil) {
		fprintf(stderr, "libnfcrestore: class %s not found; %s NOT installed\n", class_name, what);
		return;
	}
	method = nf_own_method(cls, sel_registerName(selector_name));
	if (method == NULL) {
		fprintf(stderr, "libnfcrestore: -[%s %s] is not implemented by the class itself; "
		    "%s NOT installed\n", class_name, selector_name, what);
		return;
	}
	method_setImplementation(method, replacement);
	fprintf(stderr, "libnfcrestore: -[%s %s] replaced: %s\n", class_name, selector_name, what);
}

__attribute__((constructor))
static void
nf_restore_shim_init(void)
{
	nf_replace("NFDriverWrapper", "_attemptNFCCRecovery", (IMP)nf_skip_attempt,
	    "no restore attempt");
	nf_replace("NFDriverWrapper", "nextRetryTime", (IMP)nf_slow_retry,
	    "retry delay 86400 s");
}
