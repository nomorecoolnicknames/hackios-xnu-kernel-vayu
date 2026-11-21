/*
 * Android's bootloader control block in "misc", and the count of starts that never came good: what the loader
 * (xnuloader/loader.c) lets be written there and what it writes itself. No UEFI and no libc here, so that the
 * host test (tools/bcb-test) compiles the same lines the loader runs.
 *
 * The block (AOSP bootable/recovery bootloader_message.h; Qualcomm's ABL QcomModulePkg/Include/Library/Recovery.h,
 * struct RecoveryMessage) is misc's first 2048 bytes: char command[32], char status[32], char recovery[...]. vayu's
 * ABL (the phone's abl partition, LinuxLoader built 2023-05-07) reads misc's first two pages at every start
 * (RecoveryInit), cuts command at its 32nd byte and boots the recovery when command begins with "boot-recovery"
 * (or "boot-fastboot", which on this phone with dynamic partitions is the recovery too: userspace fastbootd). It
 * never writes the block back: whatever is there holds until somebody clears it. Its own "fastboot
 * reboot-recovery" writes just the same thing (FastbootCmds.c CmdRebootRecovery -> WriteRecoveryMessage: command
 * only, the rest of the pages read and written back as they were). It knows no "bootonce-bootloader": the stock
 * fastboot is reached only through the PMIC's reboot reason or the volume key, never through misc.
 *
 * So a write into misc may change only command, and only to "boot-recovery" or to nothing at all; the other 4064
 * bytes of the block must be what the flash holds now.
 */
#ifndef XNULOADER_BCB_H
#define XNULOADER_BCB_H

#define BCB_BLOCK_BYTES     4096u       /* misc's blocks on UFS; the control block is the first 2048 bytes of block 0 */
#define BCB_COMMAND_BYTES   32u
#define BCB_RECOVERY        "boot-recovery"

/* 1: the 32 bytes are "boot-recovery" and zeros after it; 0: anything else */
static int
bcb_is_recovery(const uint8_t *command)
{
	static const char want[] = BCB_RECOVERY;
	unsigned i;

	for (i = 0; i < sizeof(want) - 1; i++) {
		if (command[i] != (uint8_t)want[i]) {
			return 0;
		}
	}
	for (; i < BCB_COMMAND_BYTES; i++) {
		if (command[i] != 0) {
			return 0;
		}
	}
	return 1;
}

static int
bcb_is_clear(const uint8_t *command)
{
	for (unsigned i = 0; i < BCB_COMMAND_BYTES; i++) {
		if (command[i] != 0) {
			return 0;
		}
	}
	return 1;
}

/*
 * What is wrong with writing `want` (bytes of it) over misc's block `lba`, which holds `now`; NULL: it may be
 * written. Block 0 only, one whole block, command either "boot-recovery" or cleared, every other byte unchanged.
 */
static const char *
bcb_refusal(uint64_t lba, uint64_t bytes, const uint8_t *now, const uint8_t *want)
{
	if (lba != 0 || bytes != BCB_BLOCK_BYTES) {
		return "not misc's first block, or not one whole block of it";
	}
	for (unsigned i = BCB_COMMAND_BYTES; i < BCB_BLOCK_BYTES; i++) {
		if (now[i] != want[i]) {
			return "it changes more than the command (the block from byte 32 on is not what the flash holds)";
		}
	}
	if (!bcb_is_recovery(want) && !bcb_is_clear(want)) {
		return "its command is neither \"boot-recovery\" nor cleared";
	}
	return 0;
}

/*
 * What else is in the block, for the eye only (nothing here writes it): AOSP's bootloader_message has status at 32
 * (32 bytes) and recovery at 64 (768 bytes: "recovery\n--wipe_data\n..."), what a recovery is to do when it starts.
 * Android's RescueParty, after system_server has fallen over too often, leaves command "boot-recovery" and
 * recovery "recovery\n--prompt_and_wipe_data" there; a recovery that takes that literally wipes userdata. Clearing
 * the command (all the loader and the kernel ever do) keeps ABL from starting the recovery, and the field stays:
 * whoever puts a TWRP-like recovery back must look at it first.
 */
#define BCB_STATUS_AT       32u
#define BCB_STATUS_BYTES    32u
#define BCB_RECOVERY_AT     64u
#define BCB_RECOVERY_BYTES  768u

/* n bytes at p up to their first zero, as text into out (size bytes, always terminated): '|' for a newline, '.'
 * for anything else not printable */
static void
bcb_text(const uint8_t *p, unsigned n, char *out, unsigned size)
{
	unsigned i;

	if (size == 0) {
		return;
	}
	for (i = 0; i < n && i + 1 < size && p[i] != 0; i++) {
		out[i] = p[i] == '\n' ? '|' : p[i] >= 0x20 && p[i] < 0x7f ? (char)p[i] : '.';
	}
	out[i] = 0;
}

/* how many bytes of the block past the command are not zero (0: only the command is there) */
static unsigned
bcb_dirty(const uint8_t *block)
{
	unsigned n = 0;

	for (unsigned i = BCB_COMMAND_BYTES; i < BCB_BLOCK_BYTES; i++) {
		n += block[i] != 0;
	}
	return n;
}

/* 1: the recovery field asks for userdata to be wiped ("--wipe_data", "--prompt_and_wipe_data": RescueParty) */
static int
bcb_wipe_asked(const uint8_t *block)
{
	static const char want[] = "wipe_data";
	const uint8_t *r = block + BCB_RECOVERY_AT;

	for (unsigned i = 0; i + sizeof(want) - 1 <= BCB_RECOVERY_BYTES && r[i] != 0; i++) {
		unsigned k = 0;

		while (k < sizeof(want) - 1 && r[i + k] == (uint8_t)want[k]) {
			k++;
		}
		if (k == sizeof(want) - 1) {
			return 1;
		}
	}
	return 0;
}

/* misc's first block as it is, with command set to "boot-recovery" (recovery != 0) or cleared */
static void
bcb_compose(uint8_t *block, int recovery)
{
	static const char want[] = BCB_RECOVERY;

	for (unsigned i = 0; i < BCB_COMMAND_BYTES; i++) {
		block[i] = recovery && i < sizeof(want) - 1 ? (uint8_t)want[i] : 0;
	}
}

/*
 * boot_tries=N (loader 2.6): one block of darwin, "tries" in darwin's layout, that counts the standalone starts
 * since the last one iOS called good (the kernel, boot_tries_ok= seconds up: bsd/dev/hakios.ufs.c). The loader adds
 * one at each standalone start; when N starts in a row never came good, the next one writes "boot-recovery" into
 * misc instead of starting the kernel, and restarts: the phone is in the recovery, adb answers, nobody had to press
 * a key.
 *
 * boot_tries_clear_bcb=1 (the multiboot, HakiOS in the recovery partition and Android in boot): the other way
 * round -- the command is cleared, and ABL starts Android. An Android that sets "boot-recovery" again as it starts
 * (the menu at every power-on) would bring the phone back to a HakiOS that fails again, and so on for ever:
 * cycles counts those trips since the last good start, and with boot_tries_max_cycles=K, after K of them the
 * loader no longer starts HakiOS by itself at all but sends every start straight on to Android, until a start
 * comes good, somebody picks HakiOS in the menu, or the block is cleared.
 *
 *   0 "HKTRIES\0", u32 version 1, u32 count (starts since the last good one); 16 u64 starts (all of them);
 *   24 u64 recoveries (times this sent the phone away); 32 u32 cycles (of them since the last good start: 0 from
 *   a loader before it, the kernel zeroes it with count); 4092 u32 CRC-32 of the 4092 bytes before it.
 *   Little-endian. A block that is not one (never written: zeros) counts as all of them zero.
 */
#define TRIES_VERSION 1u

struct tries {
	uint32_t count, cycles;
	uint64_t starts, recoveries;
};

static uint32_t
tries_crc32(const uint8_t *p, uint64_t n)
{
	uint32_t c = 0xffffffffu;

	for (uint64_t i = 0; i < n; i++) {
		c ^= p[i];
		for (int k = 0; k < 8; k++) {
			c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
		}
	}
	return ~c;
}

static uint64_t
tries_le(const uint8_t *p, unsigned bytes)
{
	uint64_t v = 0;

	while (bytes-- > 0) {
		v = v << 8 | p[bytes];
	}
	return v;
}

static void
tries_put(uint8_t *p, uint64_t v, unsigned bytes)
{
	for (unsigned i = 0; i < bytes; i++, v >>= 8) {
		p[i] = (uint8_t)v;
	}
}

/* 1: the block is a tries block and *t what it says; 0: it is not one, *t all zero */
static int
tries_read(const uint8_t *b, struct tries *t)
{
	static const char magic[8] = "HKTRIES";

	t->count = t->cycles = 0;
	t->starts = t->recoveries = 0;
	for (unsigned i = 0; i < 8; i++) {
		if (b[i] != (uint8_t)magic[i]) {
			return 0;
		}
	}
	if (tries_le(b + 8, 4) != TRIES_VERSION || tries_crc32(b, 4092) != (uint32_t)tries_le(b + 4092, 4)) {
		return 0;
	}
	t->count = (uint32_t)tries_le(b + 12, 4);
	t->starts = tries_le(b + 16, 8);
	t->recoveries = tries_le(b + 24, 8);
	t->cycles = (uint32_t)tries_le(b + 32, 4);
	return 1;
}

static void
tries_make(uint8_t *b, const struct tries *t)
{
	static const char magic[8] = "HKTRIES";

	for (unsigned i = 0; i < 4096; i++) {
		b[i] = i < 8 ? (uint8_t)magic[i] : 0;
	}
	tries_put(b + 8, TRIES_VERSION, 4);
	tries_put(b + 12, t->count, 4);
	tries_put(b + 16, t->starts, 8);
	tries_put(b + 24, t->recoveries, 8);
	tries_put(b + 32, t->cycles, 4);
	tries_put(b + 4092, tries_crc32(b, 4092), 4);
}

/*
 * What this start does, given the count before it and the limit N: 1 -- N starts in a row never came good, send
 * the phone to the recovery (and the count starts again from 0); 0 -- count this start and go on.
 */
static int
tries_give_up(const struct tries *t, uint32_t limit)
{
	return limit != 0 && t->count >= limit;
}

/*
 * The whole of it, with the multiboot's switches: TRIES_START -- count this start and go on; TRIES_RECOVERY --
 * "boot-recovery" and a restart (boot_tries= alone); TRIES_ANDROID -- the command cleared and a restart
 * (boot_tries_clear_bcb=1); TRIES_HELD -- boot_tries_max_cycles=K trips already since the last good start: no
 * kernel, straight on to Android, nothing counted. by_hand: somebody chose this start (the menu): it is counted,
 * never sent away.
 */
#define TRIES_START     0
#define TRIES_RECOVERY  1
#define TRIES_ANDROID   2
#define TRIES_HELD      3

static int
tries_decide(const struct tries *t, uint32_t limit, int clear_bcb, uint32_t max_cycles, int by_hand)
{
	if (limit == 0 || by_hand) {
		return TRIES_START;
	}
	if (clear_bcb && max_cycles != 0 && t->cycles >= max_cycles) {
		return TRIES_HELD;
	}
	if (tries_give_up(t, limit)) {
		return clear_bcb ? TRIES_ANDROID : TRIES_RECOVERY;
	}
	return TRIES_START;
}

/* the count after what tries_decide said (TRIES_HELD: not written at all) */
static void
tries_after(struct tries *t, int decided, int by_hand)
{
	t->starts++;
	if (decided == TRIES_RECOVERY || decided == TRIES_ANDROID) {
		t->count = 0;
		t->recoveries++;
		t->cycles++;
	} else {
		t->count++;
		if (by_hand) {
			t->cycles = 0;  /* (somebody is at the phone: the trips before do not count against this one) */
		}
	}
}

#endif
