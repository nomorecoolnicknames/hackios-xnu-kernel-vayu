/*
 * Copyright (c) 2026 HakiOS (vayu port). APSL-2.0, as the rest of this tree.
 *
 * a64e_mini: the kernel's arm64e trap path under XNU 12377 VAYU (osfmk/arm64/sweet_a64e.c + a64e_isa_emu.c, behind
 * vayu27=1 vayu_pacemu=1), as pid 1 of a RAM disk on its own: a static arm64 Mach-O without libc or dyld (as legacy's
 * hello), so it runs before any iOS userland exists. The full test is the a64e-tests branch's vayu_a64e_suite (arm64e,
 * dyld, ObjC, Swift), which needs iOS 27's root; this one checks the values of one instruction of each class the
 * kernel emulates, on a CPU without them (QEMU's cortex-a76, the phone's Kryo 485). The instructions are .inst words,
 * so no assembler option for ARMv8.3+ is needed and nothing rewrites them.
 *
 *   PACIA, AUTIA, XPACI (register forms)   the register unchanged (pointers are never signed on this kernel)
 *   PACGA                                   Xn & 0xffffffff00000000 with sweetspeed=4, else a hash << 32
 *   RETAA                                   a return
 *   LDRAA                                   the load
 *   FCADD #90 (FCMA)                        { n.re - m.im, n.im + m.re } per pair, the thread's FPCR
 *   LDAPUR (LRCPC2)                         the load at Xn + imm9
 *   MSR DIT / MRS DIT                       the thread's own bit (24)
 * Prints "A64E-MINI <name> PASS|FAIL ..." per check and "A64E-MINI SUMMARY: ALL PASS (N checks)" or "... N FAILED";
 * then exits (the initproc panic that follows is expected). Without vayu_pacemu=1 the first PACIA is a SIGILL.
 *   build (Mac): xcrun clang -arch arm64 -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin
 *                -Wl,-e,_start -Wl,-pagezero_size,0x100000000 -o a64e_mini a64e_mini.c
 */
typedef unsigned long u64;
typedef unsigned int u32;

static long
sys3(long num, long a, long b, long c)
{
	register long x16 __asm__("x16") = num;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	__asm__ volatile ("svc #0x80" : "+r" (x0) : "r" (x16), "r" (x1), "r" (x2) : "memory", "cc");
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
	sys3(4, console, (long)s, n);
}

static void
puthex(u64 v)
{
	char b[19];
	b[0] = '0';
	b[1] = 'x';
	for (int i = 0; i < 16; i++) {
		b[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 15];
	}
	b[18] = 0;
	put(b);
}

static int checks, failed;

static void
check(const char *name, u64 got, u64 want)
{
	checks++;
	put("A64E-MINI ");
	put(name);
	if (got == want) {
		put(" PASS\n");
		return;
	}
	failed++;
	put(" FAIL got=");
	puthex(got);
	put(" want=");
	puthex(want);
	put("\n");
}

#define ONE(word, in0, in1, in2) ({ \
	register u64 r0 __asm__("x0") = (in0); register u64 r1 __asm__("x1") = (in1); register u64 r2 __asm__("x2") = (in2); \
	__asm__ volatile (".inst " #word : "+r" (r0) : "r" (r1), "r" (r2) : "memory"); r0; })

__attribute__((used)) static u64 retaa_reached;

__attribute__((naked, noinline)) static void
retaa_function(void)
{
	/* the callee's frame as arm64e code has it: PACIASP (a HINT: a NOP here), work, RETAA (traps) */
	__asm__ volatile (
                ".inst 0xd503233f\n"            /* paciasp */
                "adrp x9, _retaa_reached@PAGE\n"
                "add x9, x9, _retaa_reached@PAGEOFF\n"
                "mov x10, #1\n"
                "str x10, [x9]\n"
                ".inst 0xd65f0bff\n");          /* retaa */
}

static void
tests(void)
{
	const u64 p = 0x0000000123456780ULL;           /* a user pointer */
	u64 v;

	v = ONE(0xdac10020, p, 0x42, 0);                /* pacia x0, x1 */
	check("pac/pacia", v, p);
	v = ONE(0xdac11020, p, 0x42, 0);                /* autia x0, x1 */
	check("pac/autia", v, p);
	v = ONE(0xdac143e0, p, 0, 0);                   /* xpaci x0 */
	check("pac/xpaci", v, p);

	/* pacga x0, x1, x2: sweetspeed=4 gives x1 & 0xffffffff00000000; else a hash in the top half, the same every time */
	u64 g1 = ONE(0x9ac23020, 0, 0xabcdef0112345678ULL, 0x99);
	u64 g2 = ONE(0x9ac23020, 0, 0xabcdef0112345678ULL, 0x99);
	check("pac/pacga.low32-zero", g1 & 0xffffffffULL, 0);
	check("pac/pacga.stable", g1, g2);
	put("A64E-MINI pac/pacga INFO value=");
	puthex(g1);
	put(g1 == 0xabcdef0100000000ULL ? " (sweetspeed=4: Xn & mask)\n" : " (hash)\n");

	retaa_reached = 0;
	retaa_function();
	check("pac/retaa", retaa_reached, 1);

	static u64 cell[2] = { 0x1122334455667788ULL, 0 };
	v = ONE(0xf8200420, 0, (u64)&cell[0], 0);       /* ldraa x0, [x1] */
	check("pac/ldraa", v, cell[0]);

	/* fcadd v0.4s, v1.4s, v2.4s, #90 */
	static u32 n[4] = { 0x3f800000, 0x40000000, 0x40400000, 0x40800000 };   /* 1 2 3 4 */
	static u32 m[4] = { 0x41200000, 0x41a00000, 0x41f00000, 0x42200000 };   /* 10 20 30 40 */
	static u32 d[4];
	__asm__ volatile (
                "ldr q1, [%0]\n"
                "ldr q2, [%1]\n"
                "movi v0.16b, #0\n"
                ".inst 0x6e82e420\n"
                "str q0, [%2]\n" : : "r" (n), "r" (m), "r" (d) : "v0", "v1", "v2", "memory");
	/* { 1 - 20, 2 + 10, 3 - 40, 4 + 30 } = { -19, 12, -37, 34 } */
	check("isa/fcadd.0", d[0], 0xc1980000);
	check("isa/fcadd.1", d[1], 0x41400000);
	check("isa/fcadd.2", d[2], 0xc2140000);
	check("isa/fcadd.3", d[3], 0x42080000);

	static u32 words[4] = { 0xdeadbeef, 0x01020304, 0, 0 };
	v = ONE(0x99404020, 0, (u64)&words[0], 0);      /* ldapur w0, [x1, #4] */
	check("isa/ldapur", v & 0xffffffffULL, 0x01020304);

	v = ONE(0xd53b42a0, 0x5555, 0, 0);              /* mrs x0, dit */
	check("isa/dit.0", v, 0);
	__asm__ volatile (".inst 0xd503415f" ::: "memory");     /* msr dit, #1 */
	v = ONE(0xd53b42a0, 0x5555, 0, 0);
	check("isa/dit.1", v, 1ULL << 24);
	__asm__ volatile (".inst 0xd503405f" ::: "memory");     /* msr dit, #0 */
	v = ONE(0xd53b42a0, 0x5555, 0, 0);
	check("isa/dit.cleared", v, 0);
}

void
start(void)
{
	long fd = sys3(5, (long)"/dev/console", 2, 0);  /* open O_RDWR */
	if (fd >= 0) {
		console = (int)fd;
	}
	put("A64E-MINI: the kernel's arm64e trap path (vayu27=1 vayu_pacemu=1)\n");
	tests();
	if (failed == 0) {
		put("A64E-MINI SUMMARY: ALL PASS (");
	} else {
		put("A64E-MINI SUMMARY: FAILURES (");
	}
	puthex((u64)checks);
	put(" checks, ");
	puthex((u64)failed);
	put(" failed)\n");
	sys3(1, failed != 0, 0, 0);
	for (;;) {
	}
}
