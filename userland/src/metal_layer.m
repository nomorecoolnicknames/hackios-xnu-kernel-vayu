/*
 * metal_layer: an animated full-screen Metal program on vayu's panel (docs/metal-roadmap.md task 10, results in
 * docs/metal-layer-results.md), linked into metal_test (the plug-in shows its device to metal_test alone at metal=1) and
 * run as
 *
 *     metal_test layer LIBRARY [VAR=VALUE ...] [SECONDS [DIVISOR [BUNDLE]]]       (tools/metal-layer-run.sh does it all)
 *
 * A CAMetalLayer the size of the display in a remote CAContext of its own, which SpringBoard hosts above every window
 * (MetalLayerHost.dylib, metal_layer_host.m: the window server shows the system application's contexts and what they
 * host, not another process's displayable context by itself). Every frame is public Metal API: -nextDrawable (an
 * IOSurface of QuartzCore's, made a texture by the plug-in's -newTextureWithDescriptor:iosurface:plane:), a pass that
 * clears it to a colour that encodes the frame number and draws a rotating triangle (tri_dpos_vs's positions from a
 * buffer, tri_cflat_fs's colour from setFragmentBytes: shaders task 7 validated on the A640), -presentDrawable: and
 * -commit. An opaque layer the size of the display is not composited: QuartzCore gives the drawable itself to the
 * framebuffer. LAYER_PLAIN=1 makes the context's layer a plain magenta CALayer with the Metal layer as its lower half,
 * which the window server (backboardd over src/eagl.c) must composite.
 *
 * Reaching the window server from a child of sweetd takes two things (metal_test.m runs the program again for them):
 * SpringBoard's bootstrap port, taken before the exec (metal_layer_take_bootstrap), and metal_layer_lookup.dylib, which
 * asks user 501's launchd domain for what the system domain does not have.
 *
 * What it checks and logs:
 *   - every 15th frame five pixels of the drawable, copied by a blit, judged against the frame's own colours: the centre
 *     (inside the triangle at every angle) and the four corners (outside at every angle);
 *   - every two seconds, frames and the frame interval (median, p95, p99, max) between successive -present calls and,
 *     where QuartzCore reports it, between successive presentedTimes; time blocked in -nextDrawable; GPU time
 *     (GPUStartTime/GPUEndTime);
 *   - three times in a run, the window server's own display-sized surfaces (what it composites into): how much of each
 *     is this program's clear colour, and whether every such point comes from ONE frame (a torn composition would mix
 *     two frame numbers). The best is saved as /private/var/tmp/metal_layer.raw (u32 width, height, bytes per row, then
 *     BGRA rows) for tools/metal-layer-png.py. With the full-screen layer they hold none of it (it is scanned out);
 *     tools/metal-layer-report.py then finds the drawables in the framebuffer's swap lines of the kernel log instead;
 *   - at the end, the drawables' IOSurface IDs, for that report.
 *
 * DIVISOR divides the CADisplay's bounds (pixels) into the layer's frame (the context's geometry is in points: 3).
 *
 * From another HakiOS port (MIT, as this file): telegdyd/ios-vayu, branch vayu-dev, commit 061e5a82d023 (its Metal tasks 8-11).
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <IOSurface/IOSurfaceRef.h>
#include <dlfcn.h>
#include <notify.h>
#include <mach/mach.h>
extern kern_return_t bootstrap_look_up(mach_port_t, const char *, mach_port_t *);
extern int proc_listallpids(void *, int);
extern int proc_name(int, void *, uint32_t);
#include <math.h>
#include <mach/mach_time.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define LAYER_RING 3                    /* vertex buffers in flight */
#define LAYER_MAX_FRAMES 20000

static int layer_failures, layer_checks;

static void
layer_check(BOOL ok, NSString *what)
{
	layer_checks++;
	layer_failures += !ok;
	printf("metal_layer: %s %s\n", ok ? "PASS" : "FAIL", [what UTF8String]);
}

static double
layer_now(void)
{
	static mach_timebase_info_data_t tb;

	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
}

static int
layer_cmp(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

/* median / p95 / p99 / max of N intervals in seconds, printed in milliseconds */
static void
layer_stats(const char *what, double *v, int n)
{
	if (n <= 0) {
		printf("metal_layer:   %s: none\n", what);
		return;
	}
	qsort(v, (size_t)n, sizeof(*v), layer_cmp);
	printf("metal_layer:   %s: n %d, median %.2f ms, p95 %.2f, p99 %.2f, max %.2f\n", what, n, v[n / 2] * 1e3,
	    v[(int)(n * 0.95)] * 1e3, v[(int)(n * 0.99) < n ? (int)(n * 0.99) : n - 1] * 1e3, v[n - 1] * 1e3);
}

/* the frame's colours: the clear colour carries the frame number (R its low byte, G the next), the triangle its
 * complement; both as BGRA8 bytes, which is what the drawable and the display hold */
static MTLClearColor
layer_clear(unsigned f)
{
	return MTLClearColorMake((f & 0xff) / 255.0, ((f >> 8) & 0xff) / 255.0, 0x60 / 255.0, 1.0);
}

static uint32_t
layer_clear_bgra(unsigned f)
{
	return 0xff000000u | (f & 0xffu) << 16 | ((f >> 8) & 0xffu) << 8 | 0x60u;
}

static void
layer_triangle_color(unsigned f, float c[4])
{
	c[0] = (255 - (f & 0xff)) / 255.0f;
	c[1] = 0xe0 / 255.0f;
	c[2] = ((f * 3) & 0xff) / 255.0f;
	c[3] = 1.0f;
}

static uint32_t
layer_triangle_bgra(unsigned f)
{
	return 0xff000000u | (255u - (f & 0xffu)) << 16 | 0xe0u << 8 | ((f * 3) & 0xffu);
}

static BOOL
layer_close(uint32_t a, uint32_t b)
{
	for (int s = 0; s < 32; s += 8) {
		if (abs((int)((a >> s) & 0xff) - (int)((b >> s) & 0xff)) > 2) {
			return NO;
		}
	}
	return YES;
}

/* a triangle around the centre, turned by ANGLE, its circumradius 0.6 of the half height (aspect corrected): the centre
 * is inside at every angle and the corners never are */
static void
layer_vertices(float *xy, double angle, double aspect)
{
	for (int k = 0; k < 3; k++) {
		const double a = angle + k * 2.0 * M_PI / 3.0;

		xy[2 * k] = (float)(0.6 * cos(a) / aspect);
		xy[2 * k + 1] = (float)(0.6 * sin(a));
	}
}

/* ask SpringBoard's MetalLayerHost.dylib to host context ID full screen (0: to stop) */
static void
layer_host(uint32_t contextId)
{
	FILE *file = fopen("/private/var/tmp/metal_layer.host", "w");

	if (file != NULL) {
		fprintf(file, "%u\n", contextId);
		fclose(file);
	}
	notify_post("com.hakios.metal_layer.host");
	printf("metal_layer: asked SpringBoard (MetalLayerHost) to host context %u\n", contextId);
	if (contextId != 0) {
		usleep(300000);
		[CATransaction flush];
	}
}

/* QuartzCore SPI, looked up at run time */
static NSString *
layer_const(const char *symbol, NSString *fallback)
{
	NSString *const *p = (NSString *const *)dlsym(RTLD_DEFAULT, symbol);

	return p != NULL && *p != nil ? *p : fallback;
}

/*
 * The display as the window server composited it. CARenderServerRenderDisplay gives an all-zero surface here (FACT,
 * 2026-11-07), so the composited frames are read where the framebuffer takes them: the window server draws each one
 * into a display-sized BGRA IOSurface of its own and swaps it to the panel (MiniIOMobileFramebuffer's "surfaces in
 * layers" lines name them). Every display-sized IOSurface that is not one of this program's drawables is looked up by
 * ID and judged: how much of a grid of points is this program's background (blue 0x60, red and green a frame number
 * within the last 255), and whether those points all come from ONE frame -- a composition torn between two frames
 * would mix two numbers. The best such surface is the check; with SAVE it is written to /private/var/tmp/metal_layer.raw
 * (u32 width, height, bytes per row, then the rows) for tools/metal-layer-png.py.
 */
static void
layer_capture(NSSet *ours, size_t w, size_t h, unsigned latest, BOOL save)
{
	int bestOurs = -1, bestTotal = 1, bestMixed = 0, bestFrames = 0, seen = 0;
	unsigned bestId = 0, bestList[8] = { 0 };

	for (uint32_t id = 1; id < 4096; id++) {
		IOSurfaceRef s;

		if ([ours containsObject:@(id)] || (s = IOSurfaceLookup(id)) == NULL) {
			continue;
		}
		if (IOSurfaceGetWidth(s) != w || IOSurfaceGetHeight(s) != h || IOSurfaceGetPixelFormat(s) != 'BGRA' ||
		    IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL) != 0) {
			CFRelease(s);
			continue;
		}
		const uint8_t *base = IOSurfaceGetBaseAddress(s);
		const size_t row = IOSurfaceGetBytesPerRow(s);
		unsigned frames[64];
		int nframes = 0, hits = 0, total = 0, mixed = 0;

		seen++;
		for (size_t y = h / 40; y < h; y += h / 20) {
			for (size_t x = w / 40; x < w; x += w / 20) {
				const uint32_t p = *(const uint32_t *)(base + y * row + x * 4);
				const unsigned f = ((p >> 16) & 0xff) | ((p >> 8) & 0xff) << 8;

				total++;
				if ((p & 0xff) >= 0x5e && (p & 0xff) <= 0x62 && f <= latest && latest - f < 256) {
					int k;

					hits++;
					for (k = 0; k < nframes && frames[k] != f; k++) {
					}
					if (k == nframes && nframes < 64) {
						frames[nframes++] = f;
					}
				}
			}
		}
		/* (frame numbers 1 apart can be one frame's colour rounded differently by the composition: allowed) */
		for (int k = 1; k < nframes; k++) {
			mixed += abs((int)frames[k] - (int)frames[0]) > 1;
		}
		if (hits > bestOurs) {
			bestOurs = hits;
			bestTotal = total;
			bestMixed = mixed;
			bestFrames = nframes;
			bestId = id;
			for (int k = 0; k < 8; k++) {
				bestList[k] = k < nframes ? frames[k] : 0;
			}
			if (save) {
				FILE *out = fopen("/private/var/tmp/metal_layer.raw", "wb");

				if (out != NULL) {
					uint32_t head[3] = { (uint32_t)w, (uint32_t)h, (uint32_t)row };

					fwrite(head, sizeof(head), 1, out);
					fwrite(base, row, h, out);
					fclose(out);
				}
			}
		}
		IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
		CFRelease(s);
	}
	printf("metal_layer: composited frame at frame %u: %d display-sized surfaces of the window server; the best, IOSurface %u: "
	    "%d of %d grid points this program's background (%.0f%%), from %d frame number(s):", latest, seen, bestId,
	    bestOurs < 0 ? 0 : bestOurs, bestTotal, 100.0 * (bestOurs < 0 ? 0 : bestOurs) / bestTotal, bestFrames);
	for (int k = 0; k < bestFrames && k < 8; k++) {
		printf(" %u", bestList[k]);
	}
	printf("%s\n", save && bestOurs >= 0 ? " (saved as /private/var/tmp/metal_layer.raw)" : "");
	/* (not a check: an opaque layer the size of the display is not composited at all -- QuartzCore gives the drawable
	 * itself to the framebuffer, which is what tools/metal-layer-report.py finds in the kernel's swap lines) */
	if (bestOurs > 0) {
		printf("metal_layer:   composited: %s\n", bestMixed == 0 ? "one frame" : "MIXED frames (torn)");
	}
}

/*
 * Before the program runs again under metal_layer_lookup.dylib (metal_test.m): this process takes a user/501 process's
 * bootstrap port, SpringBoard's, as its own. libxpc reads the bootstrap port once, when a process starts, and a task's
 * special ports outlive exec, so the program run again looks names up through it from its first instruction. The
 * kernel gives the port only to a platform binary, which here means ad hoc signed (kern_minimac.c): metal_test is
 * linked with -adhoc_codesign for it. Says what it did in LAYER_BOOTSTRAP.
 */
void
metal_layer_take_bootstrap(void)
{
	const char *donor = getenv("LAYER_BOOTSTRAP_FROM") != NULL ? getenv("LAYER_BOOTSTRAP_FROM") : "SpringBoard";
	pid_t pid = -1, pids[2048];
	const int n = proc_listallpids(pids, sizeof(pids));   /* (no /bin/sh here for a popen of ps) */

	for (int k = 0; k < n && pid < 0; k++) {
		char name[64] = "";

		if (proc_name(pids[k], name, sizeof(name)) > 0 && strcmp(name, donor) == 0) {
			pid = pids[k];
		}
	}
	mach_port_t task = MACH_PORT_NULL, bp = MACH_PORT_NULL;
	kern_return_t tk = pid > 0 ? task_for_pid(mach_task_self(), pid, &task) : KERN_FAILURE;
	kern_return_t bk = tk == KERN_SUCCESS ? task_get_special_port(task, TASK_BOOTSTRAP_PORT, &bp) : KERN_FAILURE;
	kern_return_t sk = bk == KERN_SUCCESS ? task_set_special_port(mach_task_self(), TASK_BOOTSTRAP_PORT, bp) : KERN_FAILURE;

	if (sk == KERN_SUCCESS) {
		bootstrap_port = bp;
	}
	printf("metal_layer: %s is pid %d; task_for_pid %d (port %u), its bootstrap port %#x (%u), made ours %d\n", donor, pid, tk,
	    task, bk, bp, sk);
	if (task != MACH_PORT_NULL) {
		mach_port_deallocate(mach_task_self(), task);
	}
	{
		char said[160];

		snprintf(said, sizeof(said), "%s pid %d: task_for_pid %d, its bootstrap port %#x, made ours %d", donor, pid, tk, bk, sk);
		setenv("LAYER_BOOTSTRAP", said, 1);
	}
}

int
metal_layer(id<MTLDevice> device, const char *library_path, double seconds, double divisor)
{
	NSError *error = nil;

	printf("metal_layer: device \"%s\"\n", [[device name] UTF8String]);
	if (device == nil) {
		layer_check(NO, @"a device");
		return 1;
	}

	/* the pipeline: tri_dpos_vs + tri_cflat_fs into BGRA8 */
	id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:library_path]]
						      error:&error];
	id<MTLFunction> vs = [library newFunctionWithName:@"tri_dpos_vs"], fs = [library newFunctionWithName:@"tri_cflat_fs"];
	MTLRenderPipelineDescriptor *rd = [[MTLRenderPipelineDescriptor alloc] init];

	rd.vertexFunction = vs;
	rd.fragmentFunction = fs;
	rd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
	id<MTLRenderPipelineState> pipeline = vs != nil && fs != nil ? [device newRenderPipelineStateWithDescriptor:rd error:&error] : nil;
	layer_check(pipeline != nil, [NSString stringWithFormat:@"render pipeline tri_dpos_vs+tri_cflat_fs for BGRA8%@",
	    pipeline == nil ? [NSString stringWithFormat:@" (%@)", error] : @""]);
	id<MTLCommandQueue> queue = [device newCommandQueue];
	if (pipeline == nil || queue == nil) {
		return 1;
	}

	/*
	 * The window server's services (com.apple.CARenderServer among them) are backboardd's, a job of user mobile, so
	 * launchd keeps them in the user/501 domain. A process sweetd started (a system job) looks them up in the system
	 * domain and is told there is no such service (launchd: "failed lookup ... (system) ... No such process"); launchd
	 * picks the domain by the job a process descends from, not by its uid (setuid(501) changed nothing), and
	 * SpringBoard's bootstrap port is not given out (task_get_special_port: 0x10000003). bootstrap_look_up_per_user
	 * does answer a root process, so from here a lookup the system domain refuses is asked of user 501's
	 * (metal_layer_lookup.c, a library metal_test layer runs with). The process stays root.
	 *
	 * That reaches this program's own lookups but not QuartzCore's (a call inside the shared cache); what does is the
	 * bootstrap port metal_test took from SpringBoard before it ran again (metal_layer_take_bootstrap).
	 */
	{
		/* what the kernel says about this process's signature: backboardd reads a client's entitlements through csops */
		extern int csops(pid_t, unsigned int, void *, size_t);
		uint32_t status = 0;
		static uint8_t buf[8192];
		const int s = csops(getpid(), 0 /* CS_OPS_STATUS */, &status, sizeof(status));
		const int x = csops(getpid(), 7 /* CS_OPS_ENTITLEMENTS_BLOB */, buf, sizeof(buf));
		const uint32_t xl = x == 0 ? (uint32_t)buf[4] << 24 | buf[5] << 16 | buf[6] << 8 | buf[7] : 0;
		const int d = csops(getpid(), 16 /* CS_OPS_DER_ENTITLEMENTS_BLOB */, buf, sizeof(buf));
		const uint32_t dl = d == 0 ? (uint32_t)buf[4] << 24 | buf[5] << 16 | buf[6] << 8 | buf[7] : 0;

		printf("metal_layer: code signature: csops status %d flags %#x (valid %d, platform %d); entitlements XML %d (%u bytes), DER %d "
		    "(%u bytes)\n", s, status, !!(status & 0x1), !!(status & 0x4000000), x, xl, d, dl);
	}
	printf("metal_layer: bootstrap port %u (%s)\n", bootstrap_port, getenv("LAYER_BOOTSTRAP") != NULL ? getenv("LAYER_BOOTSTRAP") : "this process's own");
	Class contextClass = NSClassFromString(@"CAContext"), displayClass = NSClassFromString(@"CADisplay");
	{
		mach_port_t server = MACH_PORT_NULL;
		kern_return_t kr = bootstrap_look_up(bootstrap_port, "com.apple.CARenderServer", &server);

		printf("metal_layer: bootstrap_look_up(com.apple.CARenderServer) = %d, port %u\n", kr, server);
	}
	id display = displayClass != Nil ? [displayClass performSelector:@selector(mainDisplay)] : nil;
	CGRect bounds = display != nil ? ((CGRect (*)(id, SEL))objc_msgSend)(display, @selector(bounds)) : CGRectMake(0, 0, 1125, 2436);

	printf("metal_layer: display %s %.0fx%.0f; layer frame = bounds / %.0f\n", display != nil ? [[display description] UTF8String] :
	    "(no CADisplay)", bounds.size.width, bounds.size.height, divisor);

	/* the layer and its context */
	const CGSize pixels = bounds.size;
	CAMetalLayer *layer = [CAMetalLayer layer];
	layer.device = device;
	layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	layer.framebufferOnly = NO;              /* the checks blit from the drawable */
	layer.opaque = YES;
	layer.frame = CGRectMake(0, 0, pixels.width / divisor, pixels.height / divisor);
	layer.contentsScale = divisor;
	layer.drawableSize = pixels;
	if ([layer respondsToSelector:@selector(setMaximumDrawableCount:)]) {
		layer.maximumDrawableCount = 3;
	}
	const uint32_t displayId = display != nil && [display respondsToSelector:@selector(displayId)] ?
	    ((uint32_t (*)(id, SEL))objc_msgSend)(display, @selector(displayId)) : 0;
	NSDictionary *options = @{ layer_const("kCAContextDisplayable", @"displayable"): @YES,
				   layer_const("kCAContextIgnoresHitTest", @"ignoresHitTest"): @YES,
				   layer_const("kCAContextDisplayId", @"displayId"): @(displayId) };
	printf("metal_layer: context options %s\n", [[[options description] stringByReplacingOccurrencesOfString:@"\n" withString:@" "] UTF8String]);
	id context = contextClass != Nil ? ((id (*)(id, SEL, id))objc_msgSend)(contextClass, @selector(remoteContextWithOptions:), options) : nil;
	layer_check(context != nil, [NSString stringWithFormat:@"a remote CAContext (displayable) for the layer%@",
	    context != nil ? [NSString stringWithFormat:@": context id %u", ((uint32_t (*)(id, SEL))objc_msgSend)(context, @selector(contextId))] : @""]);
	if (context == nil) {
		return 1;
	}
	((void (*)(id, SEL, float))objc_msgSend)(context, @selector(setLevel:), 1e6f);
	/* LAYER_PLAIN=1 (a diagnostic): the context's layer is a plain magenta CALayer, the Metal layer its lower half --
	 * whether the window server shows the context at all, apart from the drawables */
	if (getenv("LAYER_PLAIN") != NULL) {
		CALayer *root = [CALayer layer];
		CGColorSpaceRef rgb = CGColorSpaceCreateDeviceRGB();
		const CGFloat magenta[4] = { 1, 0, 1, 1 };
		CGColorRef colour = CGColorCreate(rgb, magenta);

		root.frame = layer.frame;
		root.backgroundColor = colour;
		root.opaque = YES;
		layer.frame = CGRectMake(0, layer.frame.size.height / 2, layer.frame.size.width, layer.frame.size.height / 2);
		[root addSublayer:layer];
		((void (*)(id, SEL, id))objc_msgSend)(context, @selector(setLayer:), root);
		CGColorRelease(colour);
		CGColorSpaceRelease(rgb);
		printf("metal_layer: LAYER_PLAIN: a magenta root layer, the Metal layer its lower half\n");
	} else {
		((void (*)(id, SEL, id))objc_msgSend)(context, @selector(setLayer:), layer);
	}
	if ([context respondsToSelector:@selector(orderAbove:)]) {
		((void (*)(id, SEL, uint32_t))objc_msgSend)(context, @selector(orderAbove:), 0);
	}
	[CATransaction flush];
	printf("metal_layer: context %s level %.0f, %s\n", [[context description] UTF8String],
	    ((float (*)(id, SEL))objc_msgSend)(context, @selector(level)),
	    [context respondsToSelector:@selector(isSecure)] && ((BOOL (*)(id, SEL))objc_msgSend)(context, @selector(isSecure)) ? "secure" : "not secure");
	/* SpringBoard hosts it (MetalLayerHost.dylib, metal_layer_host.m): the window server shows the system application's
	 * contexts and what they host, not a displayable context of another process by itself */
	layer_host(((uint32_t (*)(id, SEL))objc_msgSend)(context, @selector(contextId)));

	id<MTLBuffer> ring[LAYER_RING];
	for (int k = 0; k < LAYER_RING; k++) {
		ring[k] = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
	}
	dispatch_semaphore_t inflight = dispatch_semaphore_create(LAYER_RING);

	double *present = calloc(LAYER_MAX_FRAMES, sizeof(double)), *shown = calloc(LAYER_MAX_FRAMES, sizeof(double));
	double *wait = calloc(LAYER_MAX_FRAMES, sizeof(double)), *gpu = calloc(LAYER_MAX_FRAMES, sizeof(double));
	double *intervals = calloc(LAYER_MAX_FRAMES, sizeof(double));
	__block volatile int completed = 0, errors = 0, presentedSeen = 0;
	__block volatile int judged = 0, judgedWrong = 0;
	const double start = layer_now();
	double period = start, lastPresent = 0;
	unsigned f = 0, periodFirst = 0, captures = 0;
	NSString *firstDrawable = nil;
	NSMutableSet *surfaces = [NSMutableSet set];

	while (layer_now() - start < seconds && f < LAYER_MAX_FRAMES) {
		@autoreleasepool {
			dispatch_semaphore_wait(inflight, DISPATCH_TIME_FOREVER);
			const double t0 = layer_now();
			id<CAMetalDrawable> drawable = [layer nextDrawable];
			wait[f] = layer_now() - t0;
			if (drawable == nil) {
				layer_check(NO, [NSString stringWithFormat:@"-nextDrawable at frame %u", f]);
				dispatch_semaphore_signal(inflight);
				break;
			}
			id<MTLTexture> texture = [drawable texture];
			if (firstDrawable == nil) {
				firstDrawable = [NSString stringWithFormat:@"%s, texture %s %lux%lu format %lu, iosurface %p", class_getName([(id)drawable class]),
				    class_getName([(id)texture class]), (unsigned long)[texture width], (unsigned long)[texture height],
				    (unsigned long)[texture pixelFormat], [texture iosurface]];
				layer_check(texture != nil && [texture iosurface] != NULL && [texture width] == (NSUInteger)pixels.width,
				    [NSString stringWithFormat:@"the first drawable: %@", firstDrawable]);
			}
			if ([texture iosurface] != NULL) {
				[surfaces addObject:@(IOSurfaceGetID([texture iosurface]))];
			}

			const unsigned frame = f;
			id<MTLBuffer> vb = ring[f % LAYER_RING];
			float color[4];
			layer_vertices([vb contents], 2.0 * M_PI * (layer_now() - start) / 4.0, pixels.width / pixels.height);
			layer_triangle_color(f, color);

			MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
			rp.colorAttachments[0].texture = texture;
			rp.colorAttachments[0].loadAction = MTLLoadActionClear;
			rp.colorAttachments[0].storeAction = MTLStoreActionStore;
			rp.colorAttachments[0].clearColor = layer_clear(f);
			id<MTLCommandBuffer> cb = [queue commandBuffer];
			id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
			[enc setRenderPipelineState:pipeline];
			[enc setVertexBuffer:vb offset:0 atIndex:0];
			[enc setFragmentBytes:color length:sizeof(color) atIndex:0];
			[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
			[enc endEncoding];
			const BOOL judge = f % 15 == 7;
			/* the five judged pixels alone (the whole 11 MB drawable copied was 22 ms of the GPU, enough to make the window
			 * server's next frame miss the framebuffer's 20 ms wait for the GPU: "copied as it is") */
			id<MTLBuffer> readback = judge ? [device newBufferWithLength:5 * 4 options:MTLResourceStorageModeShared] : nil;
			if (judge) {
				const NSUInteger x[5] = { [texture width] / 2, 2, [texture width] - 3, 2, [texture width] - 3 };
				const NSUInteger y[5] = { [texture height] / 2, 2, 2, [texture height] - 3, [texture height] - 3 };
				id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];

				for (int k = 0; k < 5; k++) {
					[blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x[k], y[k], 0)
						   sourceSize:MTLSizeMake(1, 1, 1) toBuffer:readback destinationOffset:4 * k destinationBytesPerRow:4
					  destinationBytesPerImage:4];
				}
				[blit endEncoding];
			}
			[cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
				if ([done status] != MTLCommandBufferStatusCompleted) {
					errors++;
				}
				gpu[frame] = [done GPUEndTime] - [done GPUStartTime];
				if (judge) {
					const uint32_t *p = [readback contents];
					const uint32_t bg = layer_clear_bgra(frame), fg = layer_triangle_bgra(frame);
					const uint32_t centre = p[0];
					const uint32_t corner[4] = { p[1], p[2], p[3], p[4] };
					BOOL ok = layer_close(centre, fg);

					for (int k = 0; k < 4; k++) {
						ok = ok && layer_close(corner[k], bg);
					}
					judged++;
					if (!ok) {
						judgedWrong++;
						if (judgedWrong <= 5) {
							printf("metal_layer: frame %u read back wrong: centre %08x (want %08x), corners %08x %08x %08x %08x (want %08x)\n",
							    frame, centre, fg, corner[0], corner[1], corner[2], corner[3], bg);
						}
					}
				}
				completed++;
				dispatch_semaphore_signal(inflight);
			}];
			if ([(id)drawable respondsToSelector:@selector(addPresentedHandler:)]) {
				[drawable addPresentedHandler:^(id<MTLDrawable> d) {
					shown[frame] = [d presentedTime];
					if (shown[frame] > 0) {
						presentedSeen++;
					}
				}];
			}
			[cb presentDrawable:drawable];
			[cb commit];
			const double now = layer_now();
			present[f] = now;
			lastPresent = now;
			f++;
			CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true);
			[CATransaction flush];

			/* every two seconds: the period's intervals */
			if (now - period >= 2.0) {
				int n = 0, ns = 0;

				for (unsigned k = periodFirst + 1; k < f; k++) {
					intervals[n++] = present[k] - present[k - 1];
				}
				printf("metal_layer: %.1f s: %u frames in %.2f s (%.1f/s), %d completed, %d errors\n", now - start, f - periodFirst,
				    now - period, (f - periodFirst) / (now - period), completed, errors);
				layer_stats("present interval", intervals, n);
				n = 0;
				for (unsigned k = periodFirst + 1; k < f; k++) {
					if (shown[k] > 0 && shown[k - 1] > 0) {
						intervals[n++] = shown[k] - shown[k - 1];
					}
					ns += shown[k] > 0;
				}
				if (ns > 0) {
					layer_stats("presentedTime interval", intervals, n);
				}
				n = 0;
				for (unsigned k = periodFirst; k < f; k++) {
					intervals[n++] = wait[k];
				}
				layer_stats("blocked in -nextDrawable", intervals, n);
				n = 0;
				for (unsigned k = periodFirst; k < f && k < (unsigned)completed; k++) {
					intervals[n++] = gpu[k];
				}
				layer_stats("GPU time (plug-in's GPUStart/EndTime)", intervals, n);
				period = now;
				periodFirst = f;
			}
			/* captures at 1/4, 1/2 and the end of the run */
			if (captures < 2 && now - start >= seconds * (captures + 1) / 4.0) {
				layer_capture(surfaces, (size_t)pixels.width, (size_t)pixels.height, f - 1, NO);
				captures++;
			}
		}
	}
	layer_capture(surfaces, (size_t)pixels.width, (size_t)pixels.height, f - 1, YES);
	/* every buffer in flight done; then the semaphore back at its first value (libdispatch traps, SIGTRAP at exit, when
	 * one is freed below the value it was made with) */
	for (int k = 0; k < LAYER_RING; k++) {
		dispatch_semaphore_wait(inflight, dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC));
	}
	for (int k = 0; k < LAYER_RING; k++) {
		dispatch_semaphore_signal(inflight);
	}
	usleep(100000);
	{
		NSMutableString *ids = [NSMutableString string];

		for (NSNumber *id in [[surfaces allObjects] sortedArrayUsingSelector:@selector(compare:)]) {
			[ids appendFormat:@" %@", id];
		}
		printf("metal_layer: drawable IOSurface IDs:%s\n", [ids UTF8String]);
	}

	/* the whole run */
	int n = 0;
	for (unsigned k = 1; k < f; k++) {
		intervals[n++] = present[k] - present[k - 1];
	}
	printf("metal_layer: the run: %u frames in %.2f s (%.1f/s), %d completed, %d command buffers in error, %lu distinct drawable "
	    "surfaces, presentedTime reported for %d frames\n", f, lastPresent - start, f / (lastPresent - start), completed, errors,
	    (unsigned long)[surfaces count], presentedSeen);
	layer_stats("present interval (whole run)", intervals, n);
	n = 0;
	for (unsigned k = 1; k < f; k++) {
		if (shown[k] > 0 && shown[k - 1] > 0) {
			intervals[n++] = shown[k] - shown[k - 1];
		}
	}
	if (n > 0) {
		layer_stats("presentedTime interval (whole run)", intervals, n);
	}
	layer_check(errors == 0 && completed == (int)f, [NSString stringWithFormat:@"%u frames committed, %d completed, %d in error", f,
	    completed, errors]);
	layer_check(judged > 0 && judgedWrong == 0, [NSString stringWithFormat:@"%d frames read back from their drawables, %d wrong "
	    "(centre = the triangle's colour, four corners = the frame's clear colour)", judged, judgedWrong]);
	layer_check([surfaces count] >= 2 && [surfaces count] <= 4,
	    [NSString stringWithFormat:@"drawables recycled: %lu distinct IOSurfaces", (unsigned long)[surfaces count]]);

	layer_host(0);
	((void (*)(id, SEL, id))objc_msgSend)(context, @selector(setLayer:), nil);
	[CATransaction flush];
	printf("metal_layer: %d checks, %d failed\n", layer_checks, layer_failures);
	return layer_failures != 0;
}
