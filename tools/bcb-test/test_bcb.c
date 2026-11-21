/*
 * Host tests of xnuloader/bcb.h: the writes of misc's first block the loader takes (its command alone, to
 * "boot-recovery" or cleared) and those it refuses, the block it writes itself, and the count of starts in
 * darwin's "tries" block -- also as the kernel rewrites it (bsd/dev/hakios.ufs.c ufs_tries_thread: count at 12
 * and cycles at 32 zeroed, CRC-32 at 4092 made again); what the menu reads past the command (status, the recovery
 * field, RescueParty's wipe), and boot_tries_clear_bcb=1 / boot_tries_max_cycles=K against an Android that sets
 * "boot-recovery" again at every start.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bcb.h"

static int checks, failures;

static void
check(bool ok, const char *format, ...)
{
	va_list list;

	checks++;
	if (ok) {
		return;
	}
	failures++;
	printf("FAIL: ");
	va_start(list, format);
	vprintf(format, list);
	va_end(list);
	printf("\n");
}

/* misc's first block as the phone's has it (zeros) with some bytes of its own past the control block */
static void
misc_block(uint8_t *b)
{
	memset(b, 0, BCB_BLOCK_BYTES);
	for (unsigned i = 2048; i < BCB_BLOCK_BYTES; i += 97) {
		b[i] = (uint8_t)(i * 13 + 1);
	}
	memcpy(b + 64, "recovery\n--wipe_cache\n", 22);       /* (what a recovery may leave in the recovery field) */
}

/* ABL's test (Recovery.c RecoveryInit): command cut at its 32nd byte, then AsciiStrnCmp with "boot-recovery" */
static bool
abl_boots_recovery(const uint8_t *b)
{
	char command[32];

	memcpy(command, b, sizeof(command));
	command[31] = 0;
	return strncmp(command, "boot-recovery", strlen("boot-recovery")) == 0 ||
	    strncmp(command, "boot-fastboot", strlen("boot-fastboot")) == 0;
}

static void
test_refusals(void)
{
	static const struct {
		const char *command;
		bool ok;
	} commands[] = {
		{ "boot-recovery", true },
		{ "", true },
		{ "boot-recoveryX", false },
		{ "boot-recovery\n", false },
		{ "boot-fastboot", false },
		{ "bootonce-bootloader", false },
		{ "boot-recover", false },
		{ "ffbm-01", false },
	};
	uint8_t now[BCB_BLOCK_BYTES], want[BCB_BLOCK_BYTES];

	for (unsigned k = 0; k < sizeof(commands) / sizeof(commands[0]); k++) {
		misc_block(now);
		memcpy(want, now, sizeof(want));
		memset(want, 0, BCB_COMMAND_BYTES);
		memcpy(want, commands[k].command, strlen(commands[k].command));
		check((bcb_refusal(0, BCB_BLOCK_BYTES, now, want) == NULL) == commands[k].ok, "command \"%s\": %s",
		    commands[k].command, commands[k].ok ? "refused" : "taken");
	}
	misc_block(now);
	memcpy(want, now, sizeof(want));
	bcb_compose(want, 1);
	check(bcb_refusal(0, BCB_BLOCK_BYTES, now, want) == NULL, "the composed block is refused");
	check(bcb_refusal(1, BCB_BLOCK_BYTES, now, want) != NULL, "misc's second block is taken");
	check(bcb_refusal(0, 2048, now, want) != NULL, "half a block is taken");
	check(bcb_refusal(0, 2 * BCB_BLOCK_BYTES, now, want) != NULL, "two blocks are taken");
	/* one byte past the command, anywhere: refused */
	for (unsigned at = BCB_COMMAND_BYTES; at < BCB_BLOCK_BYTES; at += 61) {
		memcpy(want, now, sizeof(want));
		bcb_compose(want, 1);
		want[at] ^= 0x40;
		check(bcb_refusal(0, BCB_BLOCK_BYTES, now, want) != NULL, "byte %u changed is taken", at);
	}
	memcpy(want, now, sizeof(want));
	bcb_compose(want, 1);
	want[BCB_BLOCK_BYTES - 1] ^= 1;
	check(bcb_refusal(0, BCB_BLOCK_BYTES, now, want) != NULL, "the block's last byte changed is taken");
	/* the command's own bytes past "boot-recovery" must be zeros */
	memcpy(want, now, sizeof(want));
	bcb_compose(want, 1);
	want[31] = 'x';
	check(bcb_refusal(0, BCB_BLOCK_BYTES, now, want) != NULL, "a byte in the command's tail is taken");
}

static void
test_compose(void)
{
	uint8_t b[BCB_BLOCK_BYTES], orig[BCB_BLOCK_BYTES];

	misc_block(b);
	memcpy(b, "ffbm-02", 7);
	memcpy(orig, b, sizeof(b));
	bcb_compose(b, 1);
	check(bcb_is_recovery(b) && abl_boots_recovery(b), "composed: not \"boot-recovery\" for ABL");
	check(memcmp(b + BCB_COMMAND_BYTES, orig + BCB_COMMAND_BYTES, BCB_BLOCK_BYTES - BCB_COMMAND_BYTES) == 0,
	    "composed: more than the command changed");
	check(!abl_boots_recovery(orig), "the test's own block boots the recovery");
	bcb_compose(b, 0);
	check(bcb_is_clear(b) && !abl_boots_recovery(b), "cleared: ABL would still boot the recovery");
	check(memcmp(b + BCB_COMMAND_BYTES, orig + BCB_COMMAND_BYTES, BCB_BLOCK_BYTES - BCB_COMMAND_BYTES) == 0,
	    "cleared: more than the command changed");
}

static void
test_tries(void)
{
	uint8_t b[4096];
	struct tries t = { .count = 7, .starts = 0x123456789aULL, .recoveries = 3 }, r;

	check(tries_crc32((const uint8_t *)"123456789", 9) == 0xcbf43926u, "CRC-32 of the check string");
	tries_make(b, &t);
	check(memcmp(b, "HKTRIES\0", 8) == 0 && b[8] == 1 && b[12] == 7 && b[16] == 0x9a && b[24] == 3,
	    "the block's fields are not where the format says");
	check(tries_read(b, &r) && r.count == 7 && r.starts == t.starts && r.recoveries == 3, "made, read back: not the same");
	b[200] ^= 1;
	check(!tries_read(b, &r) && r.count == 0 && r.starts == 0 && r.recoveries == 0, "a bit changed: taken");
	memset(b, 0, sizeof(b));
	check(!tries_read(b, &r) && r.count == 0, "an empty block: taken as a count");
	tries_make(b, &t);
	b[8] = 2;
	tries_put(b + 4092, tries_crc32(b, 4092), 4);
	check(!tries_read(b, &r), "version 2: taken");
	/* the kernel's rewrite: count zeroed, CRC made again */
	tries_make(b, &t);
	memset(b + 12, 0, 4);
	tries_put(b + 4092, tries_crc32(b, 4092), 4);
	check(tries_read(b, &r) && r.count == 0 && r.starts == t.starts && r.recoveries == 3, "the kernel's rewrite");
	t.count = 2;
	check(!tries_give_up(&t, 3), "2 of 3: given up");
	t.count = 3;
	check(tries_give_up(&t, 3), "3 of 3: not given up");
	t.count = 1000;
	check(!tries_give_up(&t, 0), "no limit: given up");
}

static void
test_fields(void)
{
	uint8_t b[BCB_BLOCK_BYTES];
	char text[97], small[5];

	memset(b, 0, sizeof(b));
	check(bcb_dirty(b) == 0 && !bcb_wipe_asked(b), "an empty block: dirty, or a wipe asked");
	/* RescueParty (AOSP RecoverySystem.rebootPromptAndWipeUserData): command, and the recovery field */
	memcpy(b, "boot-recovery", 13);
	memcpy(b + BCB_RECOVERY_AT, "recovery\n--prompt_and_wipe_data\n--reason=x\n", 44);
	check(bcb_dirty(b) == 43, "dirty: %u, not the recovery field's 43 bytes", bcb_dirty(b));
	check(bcb_wipe_asked(b), "RescueParty's --prompt_and_wipe_data not seen");
	bcb_text(b + BCB_RECOVERY_AT, BCB_RECOVERY_BYTES, text, sizeof(text));
	check(strcmp(text, "recovery|--prompt_and_wipe_data|--reason=x|") == 0, "the recovery field as text: \"%s\"", text);
	bcb_text(b, BCB_COMMAND_BYTES, small, sizeof(small));
	check(strcmp(small, "boot") == 0, "text cut to its room: \"%s\"", small);
	bcb_compose(b, 0);
	check(bcb_wipe_asked(b) && bcb_dirty(b) == 43, "clearing the command touched the recovery field");
	memset(b + BCB_RECOVERY_AT, 0, BCB_RECOVERY_BYTES);
	memcpy(b + BCB_RECOVERY_AT, "recovery\n--wipe_data\n", 21);
	check(bcb_wipe_asked(b), "--wipe_data not seen");
	memset(b + BCB_RECOVERY_AT, 0, BCB_RECOVERY_BYTES);
	memcpy(b + BCB_RECOVERY_AT, "recovery\n--wipe_cache\n", 22);
	check(!bcb_wipe_asked(b), "--wipe_cache taken for a wipe of userdata");
	/* a wipe asked past the field (bytes 832 on: stage, reserved) is not the field's */
	memset(b + BCB_RECOVERY_AT, 0, BCB_RECOVERY_BYTES);
	memcpy(b + BCB_RECOVERY_AT + BCB_RECOVERY_BYTES, "--wipe_data", 11);
	check(!bcb_wipe_asked(b), "a wipe past the recovery field taken");
	b[BCB_STATUS_AT] = 0x01;
	bcb_text(b + BCB_STATUS_AT, BCB_STATUS_BYTES, small, sizeof(small));
	check(strcmp(small, ".") == 0, "a byte not printable: \"%s\"", small);
}

static void
test_cycles(void)
{
	uint8_t b[4096];
	struct tries t = { .count = 1, .cycles = 2, .starts = 9, .recoveries = 4 }, r;

	tries_make(b, &t);
	check(b[32] == 2 && tries_read(b, &r) && r.cycles == 2 && r.count == 1, "cycles at 32: not made or read back");
	/* a block of a loader before cycles: zeros at 32 */
	memset(b + 32, 0, 4);
	tries_put(b + 4092, tries_crc32(b, 4092), 4);
	check(tries_read(b, &r) && r.cycles == 0 && r.recoveries == 4, "an older block: cycles not 0");
	/* the kernel's good start: count and cycles zeroed */
	tries_make(b, &t);
	memset(b + 12, 0, 4);
	memset(b + 32, 0, 4);
	tries_put(b + 4092, tries_crc32(b, 4092), 4);
	check(tries_read(b, &r) && r.count == 0 && r.cycles == 0 && r.starts == 9, "the kernel's good start");

	/* boot_tries= alone: as before, whatever the cycles */
	memset(&t, 0, sizeof(t));
	t.count = 3, t.cycles = 50;
	check(tries_decide(&t, 3, 0, 2, 0) == TRIES_RECOVERY, "boot_tries=3 alone: not the recovery");
	check(tries_decide(&t, 3, 1, 0, 0) == TRIES_ANDROID, "clear_bcb, no max: not Android");
	check(tries_decide(&t, 3, 1, 2, 0) == TRIES_HELD, "clear_bcb, cycles over max: not held");
	check(tries_decide(&t, 3, 1, 2, 1) == TRIES_START, "by hand: not started");
	check(tries_decide(&t, 0, 1, 2, 0) == TRIES_START, "no boot_tries: not started");
	t.count = 2, t.cycles = 1;
	check(tries_decide(&t, 3, 1, 2, 0) == TRIES_START, "2 of 3, 1 of 2 cycles: not started");
	tries_after(&t, TRIES_START, 1);
	check(t.count == 3 && t.cycles == 0 && t.starts == 1, "by hand: the trips before not forgotten");

	/*
	 * The pendulum: HakiOS never comes good, Android sets "boot-recovery" again at every start. boot_tries=3
	 * boot_tries_clear_bcb=1 boot_tries_max_cycles=2: three counted starts, Android, three, Android, then Android
	 * for good -- no more kernels, no more writes of the count.
	 */
	{
		static const int want[] = {
			TRIES_START, TRIES_START, TRIES_START, TRIES_ANDROID,
			TRIES_START, TRIES_START, TRIES_START, TRIES_ANDROID,
			TRIES_HELD, TRIES_HELD, TRIES_HELD,
		};
		unsigned kernels = 0, androids = 0;

		memset(&t, 0, sizeof(t));
		for (unsigned i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
			int d = tries_decide(&t, 3, 1, 2, 0);

			check(d == want[i], "pendulum, start %u: %d, not %d", i, d, want[i]);
			if (d != TRIES_HELD) {
				tries_after(&t, d, 0);
			}
			kernels += d == TRIES_START;
			androids += d == TRIES_ANDROID || d == TRIES_HELD;
		}
		check(kernels == 6 && androids == 5 && t.recoveries == 2 && t.cycles == 2 && t.starts == 8,
		    "pendulum: %u kernels, %u to Android, %llu trips, %u cycles, %llu starts", kernels, androids,
		    (unsigned long long)t.recoveries, t.cycles, (unsigned long long)t.starts);
		/* a good start (the kernel's): back to counting */
		t.count = t.cycles = 0;
		check(tries_decide(&t, 3, 1, 2, 0) == TRIES_START, "after a good start: still held");
	}
}

int
main(void)
{
	test_refusals();
	test_compose();
	test_tries();
	test_fields();
	test_cycles();
	printf("bcb-test: %d checks, %d failed: %s\n", checks, failures, failures ? "FAIL" : "PASS");
	return failures != 0;
}
