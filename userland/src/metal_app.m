/*
 * metal_app: a real Metal program on vayu's device (docs/metal-roadmap.md task 9), linked into metal_test (the plug-in shows
 * its device to metal_test alone at metal=1) and run as `metal_test app LIBRARY [BUNDLE]`.
 *
 * Everything below is Metal's public API: a library from a .metallib built on the PC (tools/metal-mklib.py over the
 * hand-written AIR of hakios-metal/tools/kernels), a compute pipeline, three render pipelines (one specialised by
 * function constants), a texture filled by -replaceRegion:, a sampler, blits, completion handlers and
 * -waitUntilCompleted. Pipelines compile at creation (docs/air.md section 7); the plug-in keeps their stages in its disk
 * cache, so a second run of this program creates them from the cache -- the plug-in says so per pipeline ("pipeline
 * cache:" lines), and this program times each creation and counts the files of the cache directory.
 *
 * The expected values are the CPU rules of hakios-metal/tools/probe.cpp for the same shaders (9d, 9k): what passed on the
 * A640 in task 7 through the core's own C++ interface is what must come out here through Metal.
 *
 * With BUNDLE, the device is that plug-in's, made the way Metal makes it (metal_test's `bundle`), so a new plug-in can
 * be run before an image carries it; without, it is MTLCreateSystemDefaultDevice()'s. Every call after that is public.
 *
 * From another HakiOS port (MIT, as this file): telegdyd/ios-vayu, branch vayu-dev, commit 061e5a82d023 (its Metal tasks 8-11).
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dirent.h>
#include <math.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int app_failures, app_checks;
static int app_handlers, app_expected;   /* completion handlers run, and command buffers that added one */

static void
app_check(BOOL ok, NSString *what)
{
	app_checks++;
	app_failures += !ok;
	printf("metal_app: %s %s\n", ok ? "PASS" : "FAIL", [what UTF8String]);
}

static double
app_ms_since(uint64_t t0)
{
	static mach_timebase_info_data_t tb;

	if (tb.denom == 0) {
		mach_timebase_info(&tb);
	}
	return (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e6;
}

/* the files in the plug-in's writable shader cache (it sets HAKIOS_METAL_CACHE in this process when the device is made) */
static int
app_cache_files(void)
{
	const char *dir = getenv("HAKIOS_METAL_CACHE");
	DIR *d;
	struct dirent *e;
	int n = 0;

	if (dir == NULL || (d = opendir(dir)) == NULL) {
		return -1;
	}
	while ((e = readdir(d)) != NULL) {
		n += strstr(e->d_name, ".stage") != NULL || strstr(e->d_name, ".kernel") != NULL;
	}
	closedir(d);
	return n;
}

static NSString *
app_status(id<MTLCommandBuffer> cb)
{
	return [NSString stringWithFormat:@"status %lu%@", (unsigned long)[cb status],
	    [cb error] != nil ? [NSString stringWithFormat:@", error %@", [[cb error] localizedDescription]] : @""];
}

/* 9d's barycentric weights at pixel (i, j) of a 64x64 target, for tri_color_vs's two triangles (probe.cpp) */
static void
app_lambda(uint32_t i, uint32_t j, double lambda[3])
{
	const double x = i + 0.5, y = j + 0.5;

	if (j > i) {
		lambda[1] = x / 64.0;
		lambda[0] = (y - x) / 64.0;
		lambda[2] = 1.0 - y / 64.0;
	} else {
		lambda[0] = y / 64.0;
		lambda[1] = (x - y) / 64.0;
		lambda[2] = 1.0 - x / 64.0;
	}
}

static uint32_t
app_texel(uint32_t x, uint32_t y)
{
	return 0xff000000u | (0x10u + 0x30u * x) | (0x20u + 0x30u * y) << 8 | (uint32_t)(0x80 + 8 * (x + 4 * y)) << 16;
}

static id<MTLTexture>
app_target(id<MTLDevice> device)
{
	MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:64
										    height:64 mipmapped:NO];

	td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
	td.storageMode = MTLStorageModeShared;
	return [device newTextureWithDescriptor:td];
}

/* one pass over a 64x64 RGBA8 target cleared to CLEAR; the pixels read back with -getBytes: and, through a blit, into
 * a buffer (both must agree) */
static BOOL
app_draw(id<MTLDevice> device, id<MTLCommandQueue> queue, id<MTLRenderPipelineState> pipeline, id<MTLTexture> target,
	 MTLClearColor clear, id<MTLTexture> texture, id<MTLSamplerState> sampler, uint32_t *pixels, NSString *label)
{
	MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
	id<MTLBuffer> readback = [device newBufferWithLength:64 * 256 options:MTLResourceStorageModeShared];
	id<MTLCommandBuffer> cb = [queue commandBuffer];

	rp.colorAttachments[0].texture = target;
	rp.colorAttachments[0].loadAction = MTLLoadActionClear;
	rp.colorAttachments[0].storeAction = MTLStoreActionStore;
	rp.colorAttachments[0].clearColor = clear;
	[cb setLabel:label];
	id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
	if (enc == nil || readback == nil) {
		app_check(NO, [NSString stringWithFormat:@"%@: a render command encoder and a readback buffer", label]);
		return NO;
	}
	[enc setRenderPipelineState:pipeline];
	if (texture != nil) {
		[enc setFragmentTexture:texture atIndex:0];
		[enc setFragmentSamplerState:sampler atIndex:0];
	}
	[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
	[enc endEncoding];
	id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
	[blit copyFromTexture:target sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(64, 64, 1)
		     toBuffer:readback destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:64 * 256];
	[blit endEncoding];
	[cb addCompletedHandler:^(__unused id<MTLCommandBuffer> done) { __sync_fetch_and_add(&app_handlers, 1); }];
	app_expected++;
	[cb commit];
	[cb waitUntilCompleted];
	[target getBytes:pixels bytesPerRow:256 fromRegion:MTLRegionMake2D(0, 0, 64, 64) mipmapLevel:0];
	const BOOL same = memcmp(pixels, [readback contents], 64 * 256) == 0;
	app_check([cb status] == MTLCommandBufferStatusCompleted && same,
	    [NSString stringWithFormat:@"%@: committed and waited for (%@); -getBytes: and the blit into a buffer agree: %s (GPU %.3f ms)",
	    label, app_status(cb), same ? "yes" : "NO", ([cb GPUEndTime] - [cb GPUStartTime]) * 1e3]);
	return [cb status] == MTLCommandBufferStatusCompleted;
}

int
metal_app(id<MTLDevice> device, const char *library_path)
{
	NSError *error = nil;
	const uint64_t start = mach_absolute_time();

	app_failures = app_checks = app_handlers = app_expected = 0;
	if (device == nil) {
		printf("metal_app: FAIL no device\n");
		return 1;
	}
	printf("metal_app: device \"%s\"; shader cache %s (%d files before)\n", [[device name] UTF8String],
	    getenv("HAKIOS_METAL_CACHE") ? getenv("HAKIOS_METAL_CACHE") : "(none)", app_cache_files());
	id<MTLCommandQueue> queue = [device newCommandQueue];
	app_check(queue != nil, @"-newCommandQueue");
	if (queue == nil) {
		return 1;
	}

	/* --- the library, from the PC --- */
	id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:library_path]]
						     error:&error];
	NSArray *names = [[library functionNames] sortedArrayUsingSelector:@selector(compare:)];
	app_check(library != nil && [names count] >= 7, [NSString stringWithFormat:@"-newLibraryWithURL: %s: %lu functions (%@)%@",
	    library_path, (unsigned long)[names count], [names componentsJoinedByString:@" "],
	    error != nil ? [NSString stringWithFormat:@"; %@", [error localizedDescription]] : @""]);
	if (library == nil) {
		return 1;
	}
	id<MTLFunction> fill = [library newFunctionWithName:@"fill_id"];
	id<MTLFunction> colorVs = [library newFunctionWithName:@"tri_color_vs"];
	id<MTLFunction> colorFs = [library newFunctionWithName:@"tri_color_fs"];
	id<MTLFunction> texFs = [library newFunctionWithName:@"tri_tex_fs"];
	id<MTLFunction> flatVs = [library newFunctionWithName:@"tri_flat_vs"];
	app_check(fill != nil && colorVs != nil && colorFs != nil && texFs != nil && flatVs != nil &&
	    [fill functionType] == MTLFunctionTypeKernel && [colorVs functionType] == MTLFunctionTypeVertex &&
	    [colorFs functionType] == MTLFunctionTypeFragment, @"-newFunctionWithName: each function, with its type");

	/* --- pipelines: compiled at creation, or read from the cache --- */
	const int before = app_cache_files();
	uint64_t t0 = mach_absolute_time();
	id<MTLComputePipelineState> fillPipe = [device newComputePipelineStateWithFunction:fill error:&error];
	const double fillMs = app_ms_since(t0);
	app_check(fillPipe != nil, [NSString stringWithFormat:@"compute pipeline fill_id in %.2f ms (max %lu threads a threadgroup, width %lu)%@",
	    fillMs, (unsigned long)[fillPipe maxTotalThreadsPerThreadgroup], (unsigned long)[fillPipe threadExecutionWidth],
	    fillPipe == nil ? [NSString stringWithFormat:@": %@", [error localizedDescription]] : @""]);

	MTLRenderPipelineDescriptor *rd = [[MTLRenderPipelineDescriptor alloc] init];
	rd.vertexFunction = colorVs;
	rd.fragmentFunction = colorFs;
	rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
	t0 = mach_absolute_time();
	id<MTLRenderPipelineState> colorPipe = [device newRenderPipelineStateWithDescriptor:rd error:&error];
	const double colorMs = app_ms_since(t0);
	app_check(colorPipe != nil, [NSString stringWithFormat:@"render pipeline tri_color_vs+tri_color_fs in %.2f ms%@", colorMs,
	    colorPipe == nil ? [NSString stringWithFormat:@": %@", [error localizedDescription]] : @""]);

	rd.fragmentFunction = texFs;
	t0 = mach_absolute_time();
	id<MTLRenderPipelineState> texPipe = [device newRenderPipelineStateWithDescriptor:rd error:&error];
	const double texMs = app_ms_since(t0);
	app_check(texPipe != nil, [NSString stringWithFormat:@"render pipeline tri_color_vs+tri_tex_fs in %.2f ms%@", texMs,
	    texPipe == nil ? [NSString stringWithFormat:@": %@", [error localizedDescription]] : @""]);

	/* function constants: fc_value (0) = 7, fc_opaque (1) = true -> 07 ff 40 ff (tri_fc_fs.ll) */
	MTLFunctionConstantValues *fcv = [[MTLFunctionConstantValues alloc] init];
	int fcValue = 7;
	BOOL fcOpaque = YES;
	[fcv setConstantValue:&fcValue type:MTLDataTypeInt atIndex:0];
	[fcv setConstantValue:&fcOpaque type:MTLDataTypeBool atIndex:1];
	NSError *fcError = nil;
	id<MTLFunction> fcFs = [library newFunctionWithName:@"tri_fc_fs" constantValues:fcv error:&fcError];
	id<MTLRenderPipelineState> fcPipe = nil;
	double fcMs = 0;
	if (fcFs != nil) {
		rd.vertexFunction = flatVs;
		rd.fragmentFunction = fcFs;
		t0 = mach_absolute_time();
		fcPipe = [device newRenderPipelineStateWithDescriptor:rd error:&fcError];
		fcMs = app_ms_since(t0);
	}
	app_check(fcPipe != nil, [NSString stringWithFormat:@"function constants 0=7, 1=true: -newFunctionWithName:constantValues: %@ (%lu constants declared)%@",
	    fcPipe != nil ? [NSString stringWithFormat:@"and its pipeline in %.2f ms", fcMs] : @"or its pipeline failed",
	    (unsigned long)[[[library newFunctionWithName:@"tri_fc_fs"] functionConstantsDictionary] count],
	    fcError != nil ? [NSString stringWithFormat:@": %@", [fcError localizedDescription]] : @""]);
	/* ... and the guarded one left out: fc_opaque (1) = true alone -> 00 00 40 ff */
	MTLFunctionConstantValues *fcv2 = [[MTLFunctionConstantValues alloc] init];
	[fcv2 setConstantValue:&fcOpaque type:MTLDataTypeBool atIndex:1];
	id<MTLFunction> fcFs2 = [library newFunctionWithName:@"tri_fc_fs" constantValues:fcv2 error:&fcError];
	id<MTLRenderPipelineState> fcPipe2 = nil;
	if (fcFs2 != nil) {
		rd.vertexFunction = flatVs;
		rd.fragmentFunction = fcFs2;
		fcPipe2 = [device newRenderPipelineStateWithDescriptor:rd error:&fcError];
	}
	app_check(fcPipe2 != nil, [NSString stringWithFormat:@"function constants 1=true alone (0 left out): a function and its pipeline%@",
	    fcPipe2 == nil && fcError != nil ? [NSString stringWithFormat:@": %@", [fcError localizedDescription]] : @""]);
	/* ... and the required one left out: fc_value (0) = 7 alone -> no function, an error that says why */
	MTLFunctionConstantValues *fcv3 = [[MTLFunctionConstantValues alloc] init];
	[fcv3 setConstantValue:&fcValue type:MTLDataTypeInt atIndex:0];
	NSError *fcError3 = nil;
	id<MTLFunction> fcFs3 = [library newFunctionWithName:@"tri_fc_fs" constantValues:fcv3 error:&fcError3];
	app_check(fcFs3 == nil && fcError3 != nil, [NSString stringWithFormat:@"function constants 0=7 alone (the required 1 left out): %@",
	    fcFs3 == nil ? [NSString stringWithFormat:@"refused: %@", [fcError3 localizedDescription]] : @"a function was made"]);
	const int after = app_cache_files();
	printf("metal_app: pipelines: fill_id %.2f ms, tri_color %.2f ms, tri_tex %.2f ms, tri_fc %.2f ms; cache files %d before, %d after\n",
	    fillMs, colorMs, texMs, fcMs, before, after);

	/* --- compute: fill_id over 4096 invocations, out[gid] = gid * 0x9E3779B1 + c --- */
	if (fillPipe != nil) {
		const uint32_t n = 4096, c = 0x1234abcd;
		id<MTLBuffer> out = [device newBufferWithLength:n * 4 options:MTLResourceStorageModeShared];
		memset([out contents], 0xee, n * 4);
		id<MTLCommandBuffer> cb = [queue commandBuffer];
		id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
		[enc setComputePipelineState:fillPipe];
		[enc setBuffer:out offset:0 atIndex:0];
		[enc setBytes:&c length:4 atIndex:1];
		[enc dispatchThreadgroups:MTLSizeMake(n / 64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
		[enc endEncoding];
		[cb addCompletedHandler:^(__unused id<MTLCommandBuffer> done) { __sync_fetch_and_add(&app_handlers, 1); }];
		app_expected++;
		[cb commit];
		[cb waitUntilCompleted];
		uint32_t wrong = 0, first = UINT32_MAX;
		const uint32_t *w = [out contents];
		for (uint32_t i = 0; i < n; i++) {
			if (w[i] != i * 0x9E3779B1u + c) {
				wrong++;
				if (first == UINT32_MAX) first = i;
			}
		}
		app_check([cb status] == MTLCommandBufferStatusCompleted && wrong == 0,
		    [NSString stringWithFormat:@"compute: fill_id, 64 threadgroups of 64, every one of %u words gid*0x9E3779B1+c: %u wrong%@ (%@, GPU %.3f ms)",
		    n, wrong, first != UINT32_MAX ? [NSString stringWithFormat:@", first at %u: %08x", first, w[first]] : @"", app_status(cb),
		    ([cb GPUEndTime] - [cb GPUStartTime]) * 1e3]);
	}

	/* --- compute into a texture: tex_fill writes (i*0x9E3779B1+c, x, y, 0xdead) at each gid of a 64x32 RGBA32Uint texture,
	 * i = y * 64 + x (tex_fill.ll) -- the kernel the probe's on-phone compile gives other words for than the host's (task 9) --- */
	id<MTLFunction> texFill = [library newFunctionWithName:@"tex_fill"];
	if (texFill != nil) {
		id<MTLComputePipelineState> texFillPipe = [device newComputePipelineStateWithFunction:texFill error:&error];
		MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Uint width:64 height:32
											 mipmapped:NO];
		td.usage = MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
		id<MTLTexture> img = [device newTextureWithDescriptor:td];
		const uint32_t c = 0x0badf00d;
		uint32_t *texels = calloc(64 * 32, 16), wrong = 0, first = UINT32_MAX;
		if (texFillPipe != nil && img != nil) {
			id<MTLCommandBuffer> cb = [queue commandBuffer];
			id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
			[enc setComputePipelineState:texFillPipe];
			[enc setTexture:img atIndex:0];
			[enc setBytes:&c length:4 atIndex:0];
			[enc dispatchThreadgroups:MTLSizeMake(8, 4, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
			[enc endEncoding];
			[cb addCompletedHandler:^(__unused id<MTLCommandBuffer> done) { __sync_fetch_and_add(&app_handlers, 1); }];
			app_expected++;
			[cb commit];
			[cb waitUntilCompleted];
			[img getBytes:texels bytesPerRow:64 * 16 fromRegion:MTLRegionMake2D(0, 0, 64, 32) mipmapLevel:0];
			for (uint32_t y = 0; y < 32; y++) {
				for (uint32_t x = 0; x < 64; x++) {
					const uint32_t *t = texels + 4 * (y * 64 + x), i = y * 64 + x;
					if (t[0] != i * 0x9E3779B1u + c || t[1] != x || t[2] != y || t[3] != 0xdead) {
						wrong++;
						if (first == UINT32_MAX) first = i;
					}
				}
			}
			app_check([cb status] == MTLCommandBufferStatusCompleted && wrong == 0,
			    [NSString stringWithFormat:@"compute into a texture: tex_fill over 64x32 RGBA32Uint, %u of 2048 texels wrong%@ (%@)", wrong,
			    first != UINT32_MAX ? [NSString stringWithFormat:@", first at %u: %08x %08x %08x %08x", first, texels[4 * first],
			    texels[4 * first + 1], texels[4 * first + 2], texels[4 * first + 3]] : @"", app_status(cb)]);
		} else {
			app_check(NO, [NSString stringWithFormat:@"compute into a texture: tex_fill's pipeline and a 64x32 RGBA32Uint texture%@",
			    error != nil ? [NSString stringWithFormat:@": %@", [error localizedDescription]] : @""]);
		}
		free(texels);
	}

	/* --- blits: fill a buffer, copy part of it into another --- */
	{
		id<MTLBuffer> a = [device newBufferWithLength:65536 options:MTLResourceStorageModeShared];
		id<MTLBuffer> b = [device newBufferWithLength:65536 options:MTLResourceStorageModeShared];
		memset([b contents], 0x11, 65536);
		id<MTLCommandBuffer> cb = [queue commandBuffer];
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
		[blit fillBuffer:a range:NSMakeRange(0, 65536) value:0x5a];
		[blit copyFromBuffer:a sourceOffset:4096 toBuffer:b destinationOffset:8192 size:16384];
		[blit endEncoding];
		[cb addCompletedHandler:^(__unused id<MTLCommandBuffer> done) { __sync_fetch_and_add(&app_handlers, 1); }];
		app_expected++;
		[cb commit];
		[cb waitUntilCompleted];
		const uint8_t *pa = [a contents], *pb = [b contents];
		uint32_t wrong = 0;
		for (uint32_t i = 0; i < 65536; i++) {
			wrong += pa[i] != 0x5a;
			wrong += pb[i] != ((i >= 8192 && i < 8192 + 16384) ? 0x5a : 0x11);
		}
		app_check([cb status] == MTLCommandBufferStatusCompleted && wrong == 0,
		    [NSString stringWithFormat:@"blit: fillBuffer 64 KiB with 0x5a, then copyFromBuffer 16 KiB at +8192 of another: %u wrong bytes (%@)",
		    wrong, app_status(cb)]);
	}

	id<MTLTexture> target = app_target(device);
	uint32_t *pixels = calloc(64 * 64, 4);
	app_check(target != nil, @"a 64x64 RGBA8Unorm render target (shared storage)");

	/* --- render 1: per-vertex colours, interpolated (probe 9d) --- */
	if (colorPipe != nil && target != nil &&
	    app_draw(device, queue, colorPipe, target, MTLClearColorMake(0, 0, 0, 0), nil, nil, pixels, @"render tri_color")) {
		uint32_t within = 0, counted = 0, maxDev = 0;
		for (uint32_t j = 0; j < 64; j++) {
			for (uint32_t i = 0; i < 64; i++) {
				if (i == j) continue;
				double lambda[3];
				app_lambda(i, j, lambda);
				const uint32_t px = pixels[j * 64 + i];
				BOOL ok = YES;
				counted++;
				for (uint32_t ch = 0; ch < 4; ch++) {
					const uint32_t got = (px >> (8 * ch)) & 0xff;
					const uint32_t want = ch == 3 ? 255 : (uint32_t)lround(255.0 * lambda[ch]);
					const uint32_t dev = got > want ? got - want : want - got;
					if (dev > maxDev) maxDev = dev;
					ok &= ch == 3 ? dev == 0 : dev <= 1;
				}
				within += ok;
			}
		}
		app_check(within == counted, [NSString stringWithFormat:@"render tri_color: %u of %u pixels within one unit of the barycentric expectation (max deviation %u/255)",
		    within, counted, maxDev]);
	}

	/* --- render 2: a 4x4 texture filled by -replaceRegion:, sampled nearest at the interpolated (red, green) (probe 9k) --- */
	if (texPipe != nil && target != nil) {
		MTLTextureDescriptor *sd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4
											 mipmapped:NO];
		sd.usage = MTLTextureUsageShaderRead;
		id<MTLTexture> src = [device newTextureWithDescriptor:sd];
		uint32_t texels[16];
		for (uint32_t k = 0; k < 16; k++) texels[k] = app_texel(k % 4, k / 4);
		[src replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0 withBytes:texels bytesPerRow:16];
		uint32_t back[16] = { 0 };
		[src getBytes:back bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0];
		app_check(src != nil && memcmp(back, texels, sizeof(back)) == 0, @"a 4x4 RGBA8 texture: -replaceRegion: then -getBytes: read the same");
		MTLSamplerDescriptor *smd = [[MTLSamplerDescriptor alloc] init];
		smd.minFilter = MTLSamplerMinMagFilterNearest;
		smd.magFilter = MTLSamplerMinMagFilterNearest;
		smd.sAddressMode = MTLSamplerAddressModeClampToEdge;
		smd.tAddressMode = MTLSamplerAddressModeClampToEdge;
		id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:smd];
		app_check(sampler != nil, @"-newSamplerStateWithDescriptor: nearest, clamp to edge");
		if (app_draw(device, queue, texPipe, target, MTLClearColorMake(0, 0, 0, 0), src, sampler, pixels, @"render tri_tex")) {
			uint32_t judged = 0, exact = 0, skipped = 0;
			for (uint32_t j = 0; j < 64; j++) {
				for (uint32_t i = 0; i < 64; i++) {
					if (i == j) continue;
					double lambda[3];
					app_lambda(i, j, lambda);
					const double fu = 4 * lambda[0], fv = 4 * lambda[1];
					if (fabs(fu - round(fu)) < 1.0 / 64 || fabs(fv - round(fv)) < 1.0 / 64) {
						skipped++;
						continue;
					}
					const uint32_t tx = (uint32_t)fmin(3, fmax(0, floor(fu))), ty = (uint32_t)fmin(3, fmax(0, floor(fv)));
					judged++;
					exact += pixels[j * 64 + i] == app_texel(tx, ty);
				}
			}
			app_check(exact == judged && judged > 3000, [NSString stringWithFormat:@"render tri_tex: %u of %u pixels their texel exactly (%u near a texel edge not judged)",
			    exact, judged, skipped]);
		}
	}

	/* --- render 3: the specialised pipeline, every pixel 07 ff 40 ff --- */
	if (fcPipe != nil && target != nil) {
		if (app_draw(device, queue, fcPipe, target, MTLClearColorMake(0, 0, 0, 0), nil, nil, pixels, @"render tri_fc (constants 0=7, 1=true)")) {
			uint32_t right = 0;
			for (uint32_t k = 0; k < 64 * 64; k++) right += pixels[k] == 0xff40ff07u;
			app_check(right == 64 * 64, [NSString stringWithFormat:@"render tri_fc: %u of 4096 pixels 07 ff 40 ff (first %08x)", right, pixels[0]]);
		}
	}
	if (fcPipe2 != nil && target != nil) {
		if (app_draw(device, queue, fcPipe2, target, MTLClearColorMake(0, 0, 0, 0), nil, nil, pixels, @"render tri_fc (constant 1=true alone)")) {
			uint32_t right = 0;
			for (uint32_t k = 0; k < 64 * 64; k++) right += pixels[k] == 0xff400000u;
			app_check(right == 64 * 64, [NSString stringWithFormat:@"render tri_fc, 0 absent: %u of 4096 pixels 00 00 40 ff (first %08x)", right, pixels[0]]);
		}
	}

	/* --- completion handlers: one per command buffer, all run by now --- */
	for (int spin = 0; spin < 100 && app_handlers < app_expected; spin++) usleep(10000);
	app_check(app_handlers == app_expected, [NSString stringWithFormat:@"completion handlers: %d of %d command buffers called theirs",
	    app_handlers, app_expected]);
	free(pixels);
	printf("metal_app: %d checks, %d failed, %.1f ms in all; cache files now %d\n", app_checks, app_failures, app_ms_since(start), app_cache_files());
	printf("metal_app: %s\n", app_failures == 0 ? "ALL PASS" : "FAILED");
	return app_failures != 0;
}

/*
 * (task 11) `metal_test astc LIBRARY`: ASTC 4x4 LDR textures made of void-extent blocks alone (one constant colour a
 * block: what a blank page's snapshot is, SpringBoard's app snapshots being ASTC -- the window server on Metal drew a
 * white Safari card as one solid wrong colour), sampled by tri_tex_fs over a 64x64 target, at the snapshot's own
 * 1125x2436 and at sizes beside it. Each pixel must be the block colour.
 */
int
metal_astc(id<MTLDevice> device, const char *library_path)
{
	NSError *error = nil;
	static const struct { uint32_t w, h; } sizes[] = { { 64, 64 }, { 1125, 64 }, { 1125, 2436 } };
	static const uint16_t colours[][4] = { { 0xffff, 0xffff, 0xffff, 0xffff }, { 0x2020, 0x8080, 0xc0c0, 0xffff } };
	/* float sample (task 9's), half sample, half sample with alpha set by insertelement (CoreAnimation's _Ixrg shape) */
	static NSString *const fragments[] = { @"tri_tex_fs", @"tri_texh_fs", @"tri_texhx_fs" };

	app_failures = app_checks = 0;
	id<MTLCommandQueue> queue = [device newCommandQueue];
	id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:library_path]] error:&error];
	MTLSamplerDescriptor *smd = [[MTLSamplerDescriptor alloc] init];
	smd.minFilter = smd.magFilter = MTLSamplerMinMagFilterNearest;
	smd.sAddressMode = smd.tAddressMode = MTLSamplerAddressModeClampToEdge;
	id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:smd];
	id<MTLTexture> target = app_target(device);
	uint32_t *pixels = calloc(64 * 64, 4);

	for (size_t f = 0; f < sizeof(fragments) / sizeof(fragments[0]); f++) {
		MTLRenderPipelineDescriptor *rd = [[MTLRenderPipelineDescriptor alloc] init];
		rd.vertexFunction = [library newFunctionWithName:@"tri_color_vs"];
		rd.fragmentFunction = [library newFunctionWithName:fragments[f]];
		rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
		id<MTLRenderPipelineState> pipe = rd.vertexFunction != nil && rd.fragmentFunction != nil ?
						  [device newRenderPipelineStateWithDescriptor:rd error:&error] : nil;
		if (queue == nil || pipe == nil) {
			app_check(NO, [NSString stringWithFormat:@"pipeline tri_color_vs+%@ (%@)", fragments[f], error != nil ? [error localizedDescription] : @""]);
			continue;
		}
		for (size_t c = 0; c < sizeof(colours) / sizeof(colours[0]); c++) {
			const uint32_t want = 0xff000000u | (uint32_t)(colours[c][2] >> 8) << 16 | (uint32_t)(colours[c][1] >> 8) << 8 | (colours[c][0] >> 8);
			for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
				for (int astc = 0; astc < 2; astc++) {
					const uint32_t bw = (sizes[s].w + 3) / 4, bh = (sizes[s].h + 3) / 4;
					const size_t bytes = astc ? (size_t)bw * bh * 16 : (size_t)sizes[s].w * sizes[s].h * 4;
					uint8_t *data = malloc(bytes);

					if (astc) {
						for (uint32_t b = 0; b < bw * bh; b++) {
							static const uint8_t head[8] = { 0xfc, 0xfd, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };

							memcpy(data + 16 * b, head, 8);
							memcpy(data + 16 * b + 8, colours[c], 8);
						}
					} else {
						for (size_t k = 0; k < (size_t)sizes[s].w * sizes[s].h; k++) {
							memcpy(data + 4 * k, &want, 4);
						}
					}
					MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:astc ?
								    MTLPixelFormatASTC_4x4_LDR : MTLPixelFormatRGBA8Unorm width:sizes[s].w height:sizes[s].h mipmapped:NO];
					td.usage = MTLTextureUsageShaderRead;
					id<MTLTexture> src = [device newTextureWithDescriptor:td];
					[src replaceRegion:MTLRegionMake2D(0, 0, sizes[s].w, sizes[s].h) mipmapLevel:0 withBytes:data
					       bytesPerRow:astc ? bw * 16 : sizes[s].w * 4];
					free(data);
					NSString *label = [NSString stringWithFormat:@"%@ over %s %ux%u, colour %08x", fragments[f],
							   astc ? "ASTC 4x4 void-extent" : "RGBA8", sizes[s].w, sizes[s].h, want];
					if (src == nil) {
						app_check(NO, [label stringByAppendingString:@": no texture"]);
						continue;
					}
					if (app_draw(device, queue, pipe, target, MTLClearColorMake(0, 0, 0, 0), src, sampler, pixels, label)) {
						uint32_t near = 0;
						for (uint32_t k = 0; k < 64 * 64; k++) {
							near += abs((int)(pixels[k] & 0xff) - (int)(want & 0xff)) <= 2 &&
								abs((int)(pixels[k] >> 8 & 0xff) - (int)(want >> 8 & 0xff)) <= 2 &&
								abs((int)(pixels[k] >> 16 & 0xff) - (int)(want >> 16 & 0xff)) <= 2 &&
								(pixels[k] >> 24) >= 0xfd;
						}
						app_check(near >= 64 * 64 - 64, [NSString stringWithFormat:@"%@: %u of 4096 pixels within 2 of it (first %08x, middle %08x)",
						    label, near, pixels[0], pixels[32 * 64 + 40]]);
					}
				}
			}
		}
	}
	free(pixels);
	printf("metal_astc: %d checks, %d failed\n", app_checks, app_failures);
	return app_failures != 0;
}
