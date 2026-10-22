/*
 * vayu: POCO X3 Pro (SM8150-AC / Snapdragon 860).
 * Generated from Mu-Silicium Resources/DTBs/vayu.dtb, then filled from vayuPkg.
 * Paste next to platform_redwood and add &platform_vayu to platforms[].
 */
static const struct platform platform_vayu = {
	/* io_base must be non-zero: XNU treats a zero SoC base as "no SoC" and skips clock setup. */
	.name = "vayu", .io_base = 0x00400000, .io_size = 0x3FC00000,
	.gic_dist = 0x17A00000, .gic_redist = 0x17A60000, .uart = 0, .log_other_cpu = 1,
	.cores = 8, .core_shift = 8,
	/*
	 * Cores 0-3 are Cortex-A55s; 4-7 are Cortex-A76s (one prime). The kernel uses
	 * the small ones only when the big ones are all busy.
	 */
	.small_cores = 0x0f,
	/*
	 * HYPOTHESIS: vayuPkg MemoryMapLib reserves "Display Reserved" at 0x9C000000.
	 * The loader prints the real GOP base as "display ... at ..."; correct this and
	 * common/devices/vayu.conf if it differs.
	 */
	.fb_base = 0x9C000000,
};
