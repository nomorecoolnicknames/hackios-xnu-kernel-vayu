/*
 * Copyright (c) 2026 HakiOS (vayu port). APSL-2.0, as the rest of this tree.
 *
 * fbhold (K5): a pid 1 that keeps the system up, for the display probes. legacy's hello exits at once and the
 * initproc panic that follows ends the run (and the picture) seconds after the root; the kernel's display tests
 * (vayu_fbtest, vayu_fbstat, vayu_mdp, vayu_vsync) and whoever looks at the panel need the minutes until the
 * loader's loader_return_after. A static arm64 Mach-O without libc or dyld, as a64e_mini: it opens /dev/console,
 * says so, then sleeps in select() and prints a heartbeat every 10 s; after FBHOLD_SECONDS (300) it exits.
 *   build (Mac): xcrun clang -arch arm64 -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin
 *                -Wl,-e,_start -Wl,-pagezero_size,0x100000000 -o fbhold fbhold.c
 */
typedef unsigned long u64;

#define FBHOLD_SECONDS  300

static long
sys5(long num, long a, long b, long c, long d, long e)
{
	register long x16 __asm__("x16") = num;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	__asm__ volatile ("svc #0x80" : "+r" (x0) : "r" (x16), "r" (x1), "r" (x2), "r" (x3), "r" (x4) : "memory", "cc");
	return x0;
}

static int console = 1;

static void
put(const char *s)
{
	unsigned n = 0;
	while (s[n]) {
		n++;
	}
	sys5(4, console, (long)s, n, 0, 0);
}

static void
putdec(u64 v)
{
	char b[24];
	int i = 23;
	b[i] = 0;
	do {
		b[--i] = (char)('0' + v % 10);
		v /= 10;
	} while (v != 0 && i > 0);
	put(b + i);
}

void
start(void)
{
	struct {
		long tv_sec;
		int tv_usec;
	} tv;
	long fd = sys5(5, (long)"/dev/console", 2, 0, 0, 0);   /* open O_RDWR */
	if (fd >= 0) {
		console = (int)fd;
	}
	put("FBHOLD: pid 1 keeps the system up for ");
	putdec(FBHOLD_SECONDS);
	put(" s (the display probes of K5)\n");
	for (u64 t = 10; t <= FBHOLD_SECONDS; t += 10) {
		tv.tv_sec = 10;
		tv.tv_usec = 0;
		sys5(93, 0, 0, 0, 0, (long)&tv);                /* select(0, NULL, NULL, NULL, &tv): sleep 10 s */
		put("FBHOLD: ");
		putdec(t);
		put(" s\n");
	}
	put("FBHOLD: done, exiting\n");
	sys5(1, 0, 0, 0, 0, 0);
	for (;;) {
	}
}
