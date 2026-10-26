/*
 * The cameras' frames made into pictures by the processor (see camera_isp.h): one pixel per 2x2
 * cell, the nearest cell for each pixel of the picture (previews), or every pixel by bilinear
 * demosaic (stills); per pixel, in this order: the black level off, lens shading (a grid of gains per
 * colour, when there is one), the white balance's and AE's gains, the colour matrix (identity until
 * one is set), sRGB's curve. Grey-world white balance. The colour matrix and the lens shading can
 * come from the camera's own tuning (isp_load_tuning(), isp_tune()). The photographs then get the
 * "iPhone look" (spec L): noise reduction, sharpening, local contrast and vibrance on Y and the
 * chroma, and the highlight roll-off in their own gamma table (the look_pipe below); the preview gets
 * only a line-wide unsharp mask, which is what keeps its cost where it was.
 */
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include "camera_isp.h"

#define WHITE           959             /* 1023 less the black level of 64 */
#define LUT_WHITE       3836            /* WHITE in the gamma table's steps */

/*
 * The gamma table: sRGB's curve, the tone's S-curve on it, and -- the stills' -- the look's highlight
 * roll-off in its last tenth (v > 0.9): a shoulder that leaves 0.9 where it is, is a parabola up to
 * where white lands (0.975 at rolloff 1), and has half the slope there, so a lamp goes on getting a
 * little brighter instead of a hard 255.
 */
static void
build_gamma(const struct isp *isp, uint8_t *table, bool still)
{
	double rolloff = still ? (double)isp->look.rolloff : 0;

	for (int i = 0; i < 4096; i++) {
		double v = i >= LUT_WHITE ? 1 : (double)i / LUT_WHITE;

		v = v <= 0.0031308 ? 12.92 * v : 1.055 * pow(v, 1 / 2.4) - 0.055;
		v += isp->contrast * (v * v * (3 - 2 * v) - v);
		if (rolloff > 0 && v > 0.9) {
			double t = (v - 0.9) / 0.1;

			v = 0.9 + 0.1 * (t - 0.25 * rolloff * t * t);
		}
		v = v < 0 ? 0 : v > 1 ? 1 : v;
		table[i] = (uint8_t)(v * 255 + 0.5);
	}
}

void
isp_init(struct isp *isp)
{
	memset(isp, 0, sizeof(*isp));
	isp->wb[0] = isp->wb[1] = isp->wb[2] = 1;
	isp->digital = 1;
	isp->zoom = 1;
	isp->saturation = 1;
	for (int i = 0; i < 3; i++) {
		isp->ccm[i][i] = 1;
		isp->ccm_plain[i][i] = 1;
	}
	build_gamma(isp, isp->gamma, false);
	build_gamma(isp, isp->gamma_still, true);
}

/*
 * The gamma table again, with an S-curve of contrast c on sRGB's values (v + c * (smoothstep(v) - v): darker darks,
 * brighter lights, the ends and the middle where they were), and the saturation the colour matrix gets from now on.
 * vayu's pictures came out flat (camera work, 2026-10-26): sRGB's curve and a 15 % exposure target alone.
 */
void
isp_set_tone(struct isp *isp, float contrast, float saturation)
{
	isp->contrast = contrast;
	isp->saturation = saturation > 0 ? saturation : 1;
	build_gamma(isp, isp->gamma, false);
	build_gamma(isp, isp->gamma_still, true);
}

/* the stills' look and the preview's sharpening; NULL (or a field at 0) is the pictures as they were before */
void
isp_set_look(struct isp *isp, const struct isp_look *look)
{
	memset(&isp->look, 0, sizeof(isp->look));
	if (look != NULL) {
		isp->look = *look;
	}
	isp->look.nr = isp->look.nr > 0 ? isp->look.nr : 0;
	isp->look.sharpen = isp->look.sharpen > 0 ? isp->look.sharpen : 0;
	isp->look.lcon = isp->look.lcon > 0 ? isp->look.lcon : 0;
	isp->look.vibrance = isp->look.vibrance > 0 ? isp->look.vibrance : 0;
	isp->look.rolloff = isp->look.rolloff > 0 ? isp->look.rolloff : 0;
	isp->look.preview_sharp = isp->look.preview_sharp > 0 ? isp->look.preview_sharp : 0;
	/* (the curve already has the tone's contrast in it: the roll-off goes on top) */
	build_gamma(isp, isp->gamma_still, true);
}

/* where red and blue are in a cell (0 top left, 1 top right, 2 bottom left, 3 bottom right) */
static void
cell_places(uint32_t bayer, int *r, int *b)
{
	static const int red[4] = { 0, 1, 2, 3 }, blue[4] = { 3, 2, 1, 0 };

	*r = red[bayer & 3];
	*b = blue[bayer & 3];
}

/* the four values of the cell at cx of the pair of lines from `top' (above black, 0 and up) */
static inline void
cell(const uint8_t *top, size_t stride, uint32_t cx, int black, int v[4])
{
	const uint8_t *g0 = top + (size_t)(cx >> 1) * 5, *g1 = g0 + stride;
	unsigned o = (cx & 1) * 2;

	/* the frame's own black level (the kernel says 64 today; it was taken as 64 here before) */
	v[0] = (g0[o] << 2 | (g0[4] >> (o * 2) & 3)) - black;
	v[1] = (g0[o + 1] << 2 | (g0[4] >> (o * 2 + 2) & 3)) - black;
	v[2] = (g1[o] << 2 | (g1[4] >> (o * 2) & 3)) - black;
	v[3] = (g1[o + 1] << 2 | (g1[4] >> (o * 2 + 2) & 3)) - black;
	for (int i = 0; i < 4; i++) {
		v[i] = v[i] < 0 ? 0 : v[i];
	}
}

float
isp_sharpness(const struct hakios_camera_frame *f, const uint8_t *raw, float x0, float y0, float x1, float y1, float *level)
{
	enum { BLOCK = 4, MAX_BLOCKS = 512 };
	uint32_t cells_w = f->width / 2, cells_h = f->height / 2;
	uint32_t cx0 = (uint32_t)(x0 * cells_w), cx1 = (uint32_t)(x1 * cells_w), cy0 = (uint32_t)(y0 * cells_h),
	    cy1 = (uint32_t)(y1 * cells_h);
	float above[MAX_BLOCKS], here[MAX_BLOCKS];
	double steps = 0, sum = 0;
	uint32_t blocks_w, rows = 0, n = 0;
	int r, b;

	cell_places(f->bayer, &r, &b);
	cx1 = cx1 > cells_w ? cells_w : cx1;
	cy1 = cy1 > cells_h ? cells_h : cy1;
	blocks_w = cx1 > cx0 ? (cx1 - cx0) / BLOCK : 0;
	blocks_w = blocks_w > MAX_BLOCKS ? MAX_BLOCKS : blocks_w;
	/*
	 * Green's mean over blocks of 4x4 cells, and the squares of the steps between neighbouring
	 * blocks: a cell's own noise is a quarter of itself in a block's mean, and the picture's edges are
	 * not -- in the dark, at 32x and more, the steps from cell to cell were the noise's alone and the
	 * focus went wherever it liked.
	 */
	for (uint32_t by = cy0; by + BLOCK <= cy1; by += BLOCK, rows++) {
		for (uint32_t bx = 0; bx < blocks_w; bx++) {
			uint32_t g = 0;

			for (uint32_t cy = by; cy < by + BLOCK; cy++) {
				const uint8_t *top = raw + (size_t)cy * 2 * f->stride;

				for (uint32_t cx = cx0 + bx * BLOCK; cx < cx0 + bx * BLOCK + BLOCK; cx++) {
					int v[4];

					cell(top, f->stride, cx, (int)f->black, v);
					for (int i = 0; i < 4; i++) {
						g += i != r && i != b ? (uint32_t)v[i] : 0;
					}
				}
			}
			here[bx] = g / (2.0f * BLOCK * BLOCK);
			sum += here[bx];
			n++;
			if (bx > 0) {
				steps += (here[bx] - here[bx - 1]) * (here[bx] - here[bx - 1]);
			}
			if (rows > 0) {
				steps += (here[bx] - above[bx]) * (here[bx] - above[bx]);
			}
		}
		memcpy(above, here, sizeof(float) * blocks_w);
	}
	double mean = n ? sum / n : 0;

	if (level != NULL) {
		*level = (float)mean;
	}
	return mean > 0 ? (float)(steps / n / (mean * mean)) : 0;
}

static inline void lsc_at(const struct isp *isp, float u, float v, float g[3]);

void
isp_measure(const struct isp *isp, const struct hakios_camera_frame *f, const uint8_t *raw, struct isp_stats *s)
{
	uint32_t cells_w = f->width / 2, cells_h = f->height / 2;
	double sum[3] = { 0, 0, 0 }, grey[2] = { 0, 0 }, neutral[2] = { 0, 0 };
	float gains[3] = { 1, 1, 1 };
	int r, b;

	memset(s, 0, sizeof(*s));
	cell_places(f->bayer, &r, &b);
	for (uint32_t cy = 0; cy < cells_h; cy += 4) {
		const uint8_t *top = raw + (size_t)cy * 2 * f->stride;
		float v_pos = cells_h > 1 ? (float)cy / (cells_h - 1) : 0;

		for (uint32_t cx = 0; cx < cells_w; cx += 4) {
			int v[4], g = 0, top_v = 0;

			cell(top, f->stride, cx, (int)f->black, v);
			for (int i = 0; i < 4; i++) {
				g += i != r && i != b ? v[i] : 0;
				top_v = v[i] > top_v ? v[i] : top_v;
			}
			g /= 2;
			sum[0] += v[r];
			sum[1] += g;
			sum[2] += v[b];
			/*
			 * The white balance leaves out a cell with a colour near white -- a lamp, a screen:
			 * green, the most sensitive, stops there first and the cell would count as magenta --
			 * and one near black, where noise cut off at the black level is all there is.
			 */
			if (top_v < WHITE * 9 / 10 && g >= 24) {
				float cr = (float)v[r], cg = (float)g, cb = (float)v[b];

				/*
				 * As the picture will have it: the lens shading raises the corners several times, and
				 * the light there counts for that much more than in the frame as it comes.
				 */
				if (isp != NULL && isp->lsc.on) {
					lsc_at(isp, cells_w > 1 ? (float)cx / (cells_w - 1) : 0, v_pos, gains);
					cr *= gains[0];
					cg *= gains[1];
					cb *= gains[2];
				}
				/*
				 * Each cell's colour, not its light: summed, a lamp or a screen in the picture -- tens of
				 * times brighter than the room it is in -- would have the white balance to itself, and
				 * the room would be left the colour of its light.
				 */
				grey[0] += cr / cg;
				grey[1] += cb / cg;
				s->grey_cells++;
				/*
				 * Grey, or within a third of it in red and blue, with the white balance as it is: what
				 * the white balance should make grey, without the coloured things around it (the
				 * colour matrix makes any mistake in it twice as plain).
				 */
				if (isp != NULL) {
					float wr = cr * isp->wb[0] / cg, wb = cb * isp->wb[2] / cg;

					if (wr > 0.75f && wr < 1.33f && wb > 0.75f && wb < 1.33f) {
						neutral[0] += cr / cg;
						neutral[1] += cb / cg;
						s->neutral_cells++;
					}
				}
			}
			s->hist[g >> 4 > 63 ? 63 : g >> 4]++;
			s->cells++;
		}
	}
	for (int c = 0; c < 3; c++) {
		s->mean[c] = s->cells ? sum[c] / s->cells : 0;
	}
	for (int c = 0; c < 2; c++) {
		s->grey[c] = s->grey_cells ? grey[c] / s->grey_cells : 0;
		s->neutral[c] = s->neutral_cells ? neutral[c] / s->neutral_cells : 0;
	}
	for (int i = 63, above = 0; i >= 0; i--) {
		above += s->hist[i];
		if (above * 100 >= (int)s->cells) {
			s->bright = i * 16 + 8;
			break;
		}
	}
}

/*
 * Green's mean towards 15 % of white, unless that would put 1 % of the picture into white. The
 * exposure, longest first: the frame's lines at the shortest frame, then gain to 16x, then frames up
 * to max_frame_lines, then gain to the most; beyond that, digital gain (up to 4x).
 */
bool
isp_ae(struct isp *isp, const struct hakios_camera_frame *f, const struct isp_stats *s, struct hakios_camera_command *c)
{
	double now = (double)f->exposure_lines * f->gain / f->gain_unity * isp->digital, ratio, want;
	double target = 0.15 * WHITE, mean = s->mean[1] * isp->digital > 0.5 ? s->mean[1] * isp->digital : 0.5;
	double bright = s->bright * isp->digital;
	uint32_t fast = f->frame_lines_min - f->margin, longest = isp->max_frame_lines ? isp->max_frame_lines : 2 * f->frame_lines_min;
	uint32_t lines, gain, frame_lines = 0;
	double digital = 1;

	if (isp->asked && (f->exposure_lines != isp->asked_lines || f->gain != isp->asked_gain)) {
		return false;                   /* (the last change not in the frames yet) */
	}
	/* (the measured levels are the sensor's: digital gain is applied to the picture only) */
	ratio = target / mean;
	if (bright * ratio > 0.9 * WHITE && bright > 0) {
		ratio = 0.9 * WHITE / bright > target / mean * 0.5 ? 0.9 * WHITE / bright : target / mean * 0.5;
	}
	ratio = ratio < 0.25 ? 0.25 : ratio > 4 ? 4 : ratio;
	if (ratio > 0.94 && ratio < 1.06) {
		return false;
	}
	want = now * pow(ratio, 0.7);
	if (want <= fast) {
		lines = want < 4 ? 4 : (uint32_t)want;
		gain = f->gain_unity;
	} else if (want <= (double)fast * 16) {
		lines = fast;
		gain = (uint32_t)(want / fast * f->gain_unity);
	} else {
		lines = (uint32_t)(want / 16);
		lines = lines > longest - f->margin ? longest - f->margin : lines < fast ? fast : lines;
		frame_lines = lines + f->margin;
		double g = want / lines;

		if (g > (double)f->gain_max / f->gain_unity) {
			digital = g / ((double)f->gain_max / f->gain_unity);
			digital = digital > 4 ? 4 : digital;
			g = (double)f->gain_max / f->gain_unity;
		}
		gain = (uint32_t)(g * f->gain_unity);
	}
	isp->digital = (float)digital;
	isp->asked_lines = lines;
	isp->asked_gain = gain;
	isp->asked_frame_lines = frame_lines;
	isp->asked = true;
	memset(c, 0, sizeof(*c));
	c->command = HAKIOS_CAMERA_EXPOSURE;
	c->a = lines;
	c->b = gain;
	c->c = frame_lines;
	return true;
}

/*
 * The point of the reference lights' line nearest a light seen as red over green rg and blue over
 * green bg, on their logarithms: where along it (segment i and how far into it, 0 to 1).
 */
static void
on_reference_line(const struct isp_tuning *t, float rg, float bg, uint32_t *segment, float *along)
{
	float x = logf(rg), y = logf(bg), best = INFINITY;

	*segment = 0;
	*along = 0;
	for (uint32_t i = 0; i + 1 < t->refs; i++) {
		float x0 = logf(t->ref[i][0]), y0 = logf(t->ref[i][1]), dx = logf(t->ref[i + 1][0]) - x0,
		    dy = logf(t->ref[i + 1][1]) - y0, length = dx * dx + dy * dy;
		float u = length > 0 ? ((x - x0) * dx + (y - y0) * dy) / length : 0, d;

		u = u < 0 ? 0 : u > 1 ? 1 : u;
		d = (x0 + u * dx - x) * (x0 + u * dx - x) + (y0 + u * dy - y) * (y0 + u * dy - y);
		if (d < best) {
			best = d;
			*segment = i;
			*along = u;
		}
	}
}

float
isp_cct(const struct isp *isp)
{
	const struct isp_tuning *t = isp->tuning;
	uint32_t i;
	float u;

	if (t == NULL || t->refs < 2 || isp->wb[0] <= 0 || isp->wb[2] <= 0) {
		return 0;
	}
	on_reference_line(t, 1 / isp->wb[0], 1 / isp->wb[2], &i, &u);
	return t->ref[i][2] + u * (t->ref[i + 1][2] - t->ref[i][2]);
}

void
isp_awb(struct isp *isp, const struct isp_stats *s)
{
	/*
	 * The nearly grey cells, when there are enough of them (1 in 32); else every cell neither near
	 * white nor near black, when there are enough of those (1 in 16); else every cell's light.
	 */
	float r, b;

	if (s->neutral_cells * 32 >= s->cells && s->neutral[0] > 0.01 && s->neutral[1] > 0.01) {
		r = (float)(1 / s->neutral[0]);
		b = (float)(1 / s->neutral[1]);
	} else if (s->grey_cells * 16 >= s->cells && s->grey[0] > 0.01 && s->grey[1] > 0.01) {
		r = (float)(1 / s->grey[0]);
		b = (float)(1 / s->grey[1]);
	} else if (s->mean[0] >= 2 && s->mean[1] >= 2 && s->mean[2] >= 2) {
		r = (float)(s->mean[1] / s->mean[0]);
		b = (float)(s->mean[1] / s->mean[2]);
	} else {
		return;
	}

	/*
	 * Onto the line of the reference lights: a scene's own colours pull grey world off it -- a pink
	 * screen, a green wall -- and off it is what shows as green or magenta, while along it is a
	 * warmer or colder white. (The wide camera under a warm white LED: grey world of the room with a
	 * screen in it 15 % short in both red and blue, the light measured on a white sheet; kept to the
	 * line, 2 % off.)
	 */
	if (isp->tuning != NULL && isp->tuning->refs >= 2) {
		const struct isp_tuning *t = isp->tuning;
		uint32_t i;
		float u;

		on_reference_line(t, 1 / r, 1 / b, &i, &u);
		r = 1 / expf(logf(t->ref[i][0]) + u * (logf(t->ref[i + 1][0]) - logf(t->ref[i][0])));
		b = 1 / expf(logf(t->ref[i][1]) + u * (logf(t->ref[i + 1][1]) - logf(t->ref[i][1])));
	}
	r = r < 0.5f ? 0.5f : r > 4 ? 4 : r;
	b = b < 0.5f ? 0.5f : b > 4 ? 4 : b;
	isp->wb[0] = isp->wb[0] * 0.9f + r * 0.1f;
	isp->wb[2] = isp->wb[2] * 0.9f + b * 0.1f;
}

/* the part of the sensor's cells the picture shows: its middle, in the picture's proportions, 1/zoom of it each way */
static void
crop(const struct isp *isp, const struct hakios_camera_frame *f, uint32_t width, uint32_t height, uint32_t *x0, uint32_t *y0,
    uint32_t *cw, uint32_t *ch)
{
	uint32_t w = f->width / 2, h = f->height / 2;
	float zoom = isp->zoom < 1 ? 1 : isp->zoom;

	if ((uint64_t)width * h > (uint64_t)height * w) {
		*cw = w;
		*ch = (uint32_t)((uint64_t)w * height / width);
	} else {
		*ch = h;
		*cw = (uint32_t)((uint64_t)h * width / height);
	}
	*cw = (uint32_t)(*cw / zoom) < 2 ? 2 : (uint32_t)(*cw / zoom);
	*ch = (uint32_t)(*ch / zoom) < 2 ? 2 : (uint32_t)(*ch / zoom);
	*x0 = (w - *cw) / 2;
	*y0 = (h - *ch) / 2;
}

void
isp_set_ccm(struct isp *isp, const float m[9])
{
	/* the saturation after the matrix: S = (1 - s) L + s I, L every row the luma weights, so rows still sum to 1 */
	static const float luma[3] = { 0.2126f, 0.7152f, 0.0722f };
	float s = isp->saturation > 0 ? isp->saturation : 1;

	/* kept without it: the stills' vibrance does the saturating instead (pipe_init) */
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 3; c++) {
			isp->ccm_plain[r][c] = m[r * 3 + c];
		}
	}
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 3; c++) {
			float sum = 0;

			for (int k = 0; k < 3; k++) {
				sum += ((1 - s) * luma[k] + (r == k ? s : 0)) * m[k * 3 + c];
			}
			isp->ccm[r][c] = sum;
		}
	}
}

/*
 * The colour matrix with the white balance's and AE's gains folded in, in the gamma table's steps
 * (per 10-bit step above black): out[c] = sum over j of m[c][j] * value[j] (each value lens-shaded).
 */
struct pipe {
	float           m[3][3];
	bool            identity;
};

static void
pipe_init(const struct isp *isp, struct pipe *p, bool still)
{
	/* the stills' vibrance wants the colours as the matrix has them, not with its saturation folded in */
	const float (*m)[3] = still && isp->look.vibrance > 0 ? isp->ccm_plain : isp->ccm;

	p->identity = true;
	for (int c = 0; c < 3; c++) {
		for (int j = 0; j < 3; j++) {
			p->m[c][j] = m[c][j] * isp->wb[j] * isp->digital * (LUT_WHITE / (float)WHITE);
			p->identity = p->identity && (c == j ? m[c][j] == 1 : m[c][j] == 0);
		}
	}
}

static inline uint8_t
curve(const struct isp *isp, const uint8_t *table, float v)
{
	int i = (int)(v + 0.5f);

	return table[i < 0 ? 0 : i > 4095 ? 4095 : i];
}

static inline void
pixel(const struct isp *isp, const struct pipe *p, const uint8_t *table, float r, float g, float b, uint8_t *rgb)
{
	if (p->identity) {
		rgb[0] = curve(isp, table, r * p->m[0][0]);
		rgb[1] = curve(isp, table, g * p->m[1][1]);
		rgb[2] = curve(isp, table, b * p->m[2][2]);
		return;
	}
	rgb[0] = curve(isp, table, p->m[0][0] * r + p->m[0][1] * g + p->m[0][2] * b);
	rgb[1] = curve(isp, table, p->m[1][0] * r + p->m[1][1] * g + p->m[1][2] * b);
	rgb[2] = curve(isp, table, p->m[2][0] * r + p->m[2][1] * g + p->m[2][2] * b);
}

/* the lens shading's gains at (u, v), 0 to 1 across the sensor */
static inline void
lsc_at(const struct isp *isp, float u, float v, float g[3])
{
	float gx = u * (LSC_W - 1), gy = v * (LSC_H - 1);
	int x = (int)gx, y = (int)gy;

	x = x < 0 ? 0 : x > LSC_W - 2 ? LSC_W - 2 : x;
	y = y < 0 ? 0 : y > LSC_H - 2 ? LSC_H - 2 : y;
	float fx = gx - x, fy = gy - y;

	for (int c = 0; c < 3; c++) {
		const float (*t)[LSC_W] = isp->lsc.gain[c];

		g[c] = (t[y][x] * (1 - fx) + t[y][x + 1] * fx) * (1 - fy) + (t[y + 1][x] * (1 - fx) + t[y + 1][x + 1] * fx) * fy;
	}
}

/* one line of the picture as 8-bit R, G, B */
static void
render_line(const struct isp *isp, const struct hakios_camera_frame *f, const uint8_t *raw, uint32_t width, uint32_t height,
    uint32_t row, uint8_t *rgb)
{
	uint32_t x0, y0, cw, ch, cells_w = f->width / 2, cells_h = f->height / 2;
	int r, b;
	struct pipe p;

	crop(isp, f, width, height, &x0, &y0, &cw, &ch);
	cell_places(f->bayer, &r, &b);
	pipe_init(isp, &p, false);
	if (isp->turned) {
		row = height - 1 - row;         /* (and each line from its end, below) */
	}
	if (cw < width && ch < height) {
		/*
		 * Magnified (the zoom): each pixel between the four cells around it. The nearest cell blown up
		 * to a square of pixels showed every cell's own colour, its noise and the colours a 2x2 cell
		 * gets wrong at an edge, as coloured dots.
		 */
		float fy = y0 + (row + 0.5f) * ch / height - 0.5f, ty, v_at;
		uint32_t cy0 = fy < 0 ? 0 : (uint32_t)fy, cy1;

		cy0 = cy0 > cells_h - 1 ? cells_h - 1 : cy0;
		cy1 = cy0 + 1 > cells_h - 1 ? cells_h - 1 : cy0 + 1;
		ty = fy < (float)cy0 ? 0 : fy - cy0;
		v_at = cells_h > 1 ? (float)cy0 / (cells_h - 1) : 0;
		const uint8_t *t0 = raw + (size_t)cy0 * 2 * f->stride, *t1 = raw + (size_t)cy1 * 2 * f->stride;
		float gains[3] = { 1, 1, 1 };

		for (uint32_t x = 0; x < width; x++) {
			float fx = x0 + (x + 0.5f) * cw / width - 0.5f, tx, c[3] = { 0, 0, 0 };
			uint32_t cx0 = fx < 0 ? 0 : (uint32_t)fx, cx1;

			cx0 = cx0 > cells_w - 1 ? cells_w - 1 : cx0;
			cx1 = cx0 + 1 > cells_w - 1 ? cells_w - 1 : cx0 + 1;
			tx = fx < (float)cx0 ? 0 : fx - cx0;
			for (int k = 0; k < 4; k++) {
				int v[4];
				float w = (k & 1 ? tx : 1 - tx) * (k & 2 ? ty : 1 - ty);

				cell(k & 2 ? t1 : t0, f->stride, k & 1 ? cx1 : cx0, (int)f->black, v);
				c[0] += w * v[r];
				c[2] += w * v[b];
				for (int j = 0; j < 4; j++) {
					c[1] += j != r && j != b ? w * v[j] * 0.5f : 0;
				}
			}
			if (isp->lsc.on) {
				lsc_at(isp, cells_w > 1 ? (float)cx0 / (cells_w - 1) : 0, v_at, gains);
			}
			pixel(isp, &p, isp->gamma, c[0] * gains[0], c[1] * gains[1], c[2] * gains[2],
			    rgb + 3 * (isp->turned ? width - 1 - x : x));
		}
		return;
	}
	uint32_t cy = y0 + (uint32_t)((uint64_t)row * ch / height);
	const uint8_t *top = raw + (size_t)cy * 2 * f->stride;
	uint64_t step = ((uint64_t)cw << 16) / width, at = (uint64_t)x0 << 16;
	float v_pos = cells_h > 1 ? (float)cy / (cells_h - 1) : 0, gains[3] = { 1, 1, 1 };

	for (uint32_t x = 0; x < width; x++, at += step) {
		uint32_t cx = (uint32_t)(at >> 16);
		int v[4], g = 0;

		cell(top, f->stride, cx, (int)f->black, v);
		for (int j = 0; j < 4; j++) {
			g += j != r && j != b ? v[j] : 0;
		}
		if (isp->lsc.on) {
			lsc_at(isp, cells_w > 1 ? (float)cx / (cells_w - 1) : 0, v_pos, gains);
		}
		pixel(isp, &p, isp->gamma, v[r] * gains[0], g * 0.5f * gains[1], v[b] * gains[2],
		    rgb + 3 * (isp->turned ? width - 1 - x : x));
	}
}

/*
 * Two lines of 8-bit R, G, B as NV12 (BT.601's matrix, which CoreVideo's 420f and 420v and
 * tools/camraw.py take): each pixel's Y (y1 NULL: the second line has none, the picture's last of an
 * odd height), each 2x2's Cb and Cr from the sum of its four. The coefficients in 1/65536ths, video
 * range's scaled to its 219 steps of luma (16 to 235) and 224 of chroma (16 to 240); every value
 * rounded to the nearest step, so that the primaries come back within one step of 0 and 255 (red's and
 * blue's cannot be exact: no 8-bit Y, Cb, Cr is both).
 */
static void
nv12_pair(const uint8_t *rgb0, const uint8_t *rgb1, uint32_t width, uint8_t *y0, uint8_t *y1, uint8_t *uv, bool video_range)
{
	static const int32_t full[9] = { 19595, 38470, 7471, -11058, -21710, 32768, 32768, -27439, -5329 };
	static const int32_t video[9] = { 16829, 33039, 6416, -9714, -19070, 28784, 28784, -24103, -4681 };
	const int32_t *k = video_range ? video : full;
	int32_t oy = (video_range ? 16 << 16 : 0) + (1 << 15);

	for (int l = 0; l < 2; l++) {
		const uint8_t *p = l ? rgb1 : rgb0;
		uint8_t *out = l ? y1 : y0;

		for (uint32_t x = 0; out != NULL && x < width; x++, p += 3) {
			out[x] = (uint8_t)((k[0] * p[0] + k[1] * p[1] + k[2] * p[2] + oy) >> 16);
		}
	}
	for (uint32_t x = 0; x < width; x += 2) {
		uint32_t x1 = x + 1 < width ? x + 1 : x;
		int32_t R = rgb0[3 * x] + rgb0[3 * x1] + rgb1[3 * x] + rgb1[3 * x1];
		int32_t G = rgb0[3 * x + 1] + rgb0[3 * x1 + 1] + rgb1[3 * x + 1] + rgb1[3 * x1 + 1];
		int32_t B = rgb0[3 * x + 2] + rgb0[3 * x1 + 2] + rgb1[3 * x + 2] + rgb1[3 * x1 + 2];
		/*
		 * (the sums are four pixels': 18 bits off, with 128 added and just under half a step to round;
		 * a tie goes down, so that chroma's -127.5, a full colour's, comes out 0 as its +127.5 does 255)
		 */
		int32_t cb = (k[3] * R + k[4] * G + k[5] * B + (128 << 18) + (1 << 17) - 1) >> 18;
		int32_t cr = (k[6] * R + k[7] * G + k[8] * B + (128 << 18) + (1 << 17) - 1) >> 18;

		uv[x] = (uint8_t)(cb < 0 ? 0 : cb > 255 ? 255 : cb);
		uv[x + 1] = (uint8_t)(cr < 0 ? 0 : cr > 255 ? 255 : cr);
	}
}

/*
 * The preview's sharpening (spec L: only this of the look, at 0.3): an unsharp mask of radius 1 across the
 * line, on the Y that is already made. Down the line too it would keep two more lines of the picture and
 * cost what the preview cannot: the camera's frames are already down to 6-11 of 15-30 a second.
 */
static void
preview_sharpen(uint8_t *line, uint32_t width, float amount)
{
	int prev;

	if (width < 3) {
		return;
	}
	prev = line[0];
	for (uint32_t x = 0; x < width; x++) {
		int cur = line[x], next = line[x + 1 < width ? x + 1 : x - 1], blur = (prev + 2 * cur + next + 2) >> 2;
		int delta = cur - blur, out = cur;

		if (delta > 2) {
			delta -= 2;
		} else if (delta < -2) {
			delta += 2;
		} else {
			delta = 0;
		}
		if (delta != 0) {
			int add = (int)(delta * amount + (delta > 0 ? 0.5f : -0.5f));

			add = add > 24 ? 24 : add < -24 ? -24 : add;
			out = cur + add;
			out = out < 0 ? 0 : out > 255 ? 255 : out;
		}
		prev = line[x];
		line[x] = (uint8_t)out;
	}
}

void
isp_render_nv12(const struct isp *isp, const struct hakios_camera_frame *f, const uint8_t *raw, uint8_t *y, size_t y_stride,
    uint8_t *uv, size_t uv_stride, uint32_t width, uint32_t height, uint32_t first_row, uint32_t end_row, bool video_range)
{
	uint8_t line[2][3 * 8192];

	if (width > 8192) {
		return;
	}
	for (uint32_t row = first_row & ~1u; row < end_row && row < height; row += 2) {
		bool second = row + 1 < height;

		render_line(isp, f, raw, width, height, row, line[0]);
		render_line(isp, f, raw, width, height, second ? row + 1 : row, line[1]);
		nv12_pair(line[0], line[1], width, y + (size_t)row * y_stride, second ? y + (size_t)(row + 1) * y_stride : NULL,
		    uv + (size_t)(row / 2) * uv_stride, video_range);
		if (isp->look.preview_sharp > 0) {
			preview_sharpen(y + (size_t)row * y_stride, width, isp->look.preview_sharp);
			if (second) {
				preview_sharpen(y + (size_t)(row + 1) * y_stride, width, isp->look.preview_sharp);
			}
		}
	}
}

void
isp_render_bgra(const struct isp *isp, const struct hakios_camera_frame *f, const uint8_t *raw, uint8_t *out, size_t stride,
    uint32_t width, uint32_t height, uint32_t first_row, uint32_t end_row)
{
	uint8_t line[3 * 8192];

	if (width > 8192) {
		return;
	}
	for (uint32_t row = first_row; row < end_row && row < height; row++) {
		uint8_t *o = out + (size_t)row * stride;

		render_line(isp, f, raw, width, height, row, line);
		for (uint32_t x = 0; x < width; x++) {
			o[4 * x] = line[3 * x + 2];
			o[4 * x + 1] = line[3 * x + 1];
			o[4 * x + 2] = line[3 * x];
			o[4 * x + 3] = 255;
		}
	}
}

/* ---- every pixel: bilinear demosaic -------------------------------------------------------------------- */

/* the colour of each site of a 2x2 cell (0 R, 1 G, 2 B), position y * 2 + x */
static void
cfa(uint32_t bayer, int colour[4])
{
	int r, b;

	cell_places(bayer, &r, &b);
	for (int i = 0; i < 4; i++) {
		colour[i] = i == r ? 0 : i == b ? 2 : 1;
	}
}

/* line y of the frame as values above black (y clamped into the frame) */
static void
unpack_line(const struct hakios_camera_frame *f, const uint8_t *raw, int y, int *out)
{
	y = y < 0 ? 0 : y >= (int)f->height ? (int)f->height - 1 : y;
	const uint8_t *line = raw + (size_t)y * f->stride;

	for (uint32_t x = 0; x + 3 < f->width + 3 && x < f->width; x += 4) {
		const uint8_t *g = line + (x >> 2) * 5;

		for (int i = 0; i < 4 && x + i < f->width; i++) {
			int v = (g[i] << 2 | (g[4] >> (i * 2) & 3)) - (int)f->black;

			out[x + i] = v < 0 ? 0 : v;
		}
	}
}

/* one line (y) at full size as 8-bit R, G, B, from the lines y - 1, y, y + 1 (l[0..2]), through the gamma table lut */
static void
full_line(const struct isp *isp, const struct hakios_camera_frame *f, const int *const l[3], uint32_t y, const int colour[4],
    const struct pipe *p, const uint8_t *lut, uint8_t *rgb)
{
	int w = (int)f->width;
	float v_pos = f->height > 1 ? (float)y / (f->height - 1) : 0, gains[3] = { 1, 1, 1 };

	for (int x = 0; x < w; x++) {
		float sum[3] = { 0, 0, 0 };
		int n[3] = { 0, 0, 0 };

		/* each colour: its own value where the site has it, else the mean of its sites in the 3x3 around */
		for (int dy = -1; dy <= 1; dy++) {
			const int *line = l[dy + 1];
			int yy = (int)y + dy;

			yy = yy < 0 ? 0 : yy >= (int)f->height ? (int)f->height - 1 : yy;
			for (int dx = -1; dx <= 1; dx++) {
				int xx = x + dx < 0 ? 0 : x + dx >= w ? w - 1 : x + dx;
				int c = colour[(yy & 1) * 2 + (xx & 1)];

				if (dx == 0 && dy == 0) {
					sum[c] = (float)line[xx] * 64;          /* (its own value wins over the neighbours') */
					n[c] = 64;
				} else if (n[c] < 64) {
					sum[c] += (float)line[xx];
					n[c]++;
				}
			}
		}
		if (isp->lsc.on) {
			lsc_at(isp, w > 1 ? (float)x / (w - 1) : 0, v_pos, gains);
		}
		pixel(isp, p, lut, sum[0] / (n[0] ? n[0] : 1) * gains[0], sum[1] / (n[1] ? n[1] : 1) * gains[1],
		    sum[2] / (n[2] ? n[2] : 1) * gains[2], rgb + 3 * x);
	}
}

void
isp_render_full_nv12(const struct isp *isp, const struct hakios_camera_frame *f, const uint8_t *raw, uint8_t *y, size_t y_stride,
    uint8_t *uv, size_t uv_stride, uint32_t first_row, uint32_t end_row, bool video_range)
{
	uint32_t width = f->width;
	int colour[4];
	struct pipe p;
	static __thread int *lines[4];
	static __thread uint8_t *rgb[2];
	static __thread uint32_t have;

	if (width > have) {
		for (int i = 0; i < 4; i++) {
			free(lines[i]);
			lines[i] = malloc(sizeof(int) * width);
		}
		for (int i = 0; i < 2; i++) {
			free(rgb[i]);
			rgb[i] = malloc(3 * (size_t)width);
		}
		have = width;
	}
	cfa(f->bayer, colour);
	pipe_init(isp, &p, false);
	for (uint32_t row = first_row & ~1u; row < end_row && row < f->height; row += 2) {
		for (int i = 0; i < 4; i++) {
			unpack_line(f, raw, (int)row - 1 + i, lines[i]);
		}
		const int *const above[3] = { lines[0], lines[1], lines[2] }, *const below[3] = { lines[1], lines[2], lines[3] };

		bool second = row + 1 < f->height;

		full_line(isp, f, above, row, colour, &p, isp->gamma, rgb[0]);
		full_line(isp, f, below, second ? row + 1 : row, colour, &p, isp->gamma, rgb[1]);
		nv12_pair(rgb[0], rgb[1], width, y + (size_t)row * y_stride, second ? y + (size_t)(row + 1) * y_stride : NULL,
		    uv + (size_t)(row / 2) * uv_stride, video_range);
	}
}

/* one line (y) of the sensor demosaiced, as 8-bit R, G, B (the unpacked lines around it in this thread's l), through
 * the gamma table lut */
static void
demosaic_row(const struct isp *isp, const struct hakios_camera_frame *f, const uint8_t *raw, uint32_t row, const int colour[4],
    const struct pipe *p, const uint8_t *lut, int *const l[3], uint8_t *rgb)
{
	for (int i = 0; i < 3; i++) {
		unpack_line(f, raw, (int)row - 1 + i, l[i]);
	}
	full_line(isp, f, (const int *const *)l, row, colour, p, lut, rgb);
}

/* ---- the stills' look ----------------------------------------------------------------------------------- */

/*
 * Noise reduction (1), sharpening (2), local contrast (3), vibrance (4) and the highlight roll-off (5) of
 * spec L, all but the roll-off on the photograph's Y: the roll-off is in gamma_still. The rows come in
 * pairs from the demosaic below; they are kept in a small ring of LOOK_ROWS so that the 5x5's and the
 * sharpening's 3x3 have their neighbours. A piece of the picture (the render is done in pieces) clamps
 * at its own ends for those: two rows of the same scene at most, where a piece meets the next.
 */
#define LOOK_ROWS       8               /* (5 rows for the 5x5's, 3 for the sharpening, and the lag between) */
#define LOOK_LCON_W     33              /* (the local contrast's box: 1/32 of the width, 33 points across) */
#define LOOK_LCON_H     25
#define LOOK_THRESHOLD  2.5f            /* codes of Y below which the sharpening leaves the noise alone */
#define LOOK_HALO       24              /* the most the sharpening may add or take, in codes */

static unsigned long long look_ns[3];   /* the time in noise reduction, sharpening, local contrast (ns this still) */

static inline unsigned long long
now_ns(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (unsigned long long)t.tv_sec * 1000000000ull + (unsigned long long)t.tv_nsec;
}

static inline void
look_add(unsigned i, unsigned long long ns)
{
	__sync_fetch_and_add(&look_ns[i], ns);
}

/* the last photograph's time in its pieces, and the counters emptied for the next */
void
isp_take_still_timing(struct isp_still_timing *timing)
{
	timing->nr_ms = (double)__sync_lock_test_and_set(&look_ns[0], 0) / 1e6;
	timing->sharpen_ms = (double)__sync_lock_test_and_set(&look_ns[1], 0) / 1e6;
	timing->lcon_ms = (double)__sync_lock_test_and_set(&look_ns[2], 0) / 1e6;
}

/* the rows the look works on, this thread's (the pieces run on several at once) */
struct look_rows {
	uint8_t         *luma[LOOK_ROWS];       /* the row's Y, as the picture has it */
	uint8_t         *h[LOOK_ROWS];          /* and after the noise reduction's 1-D pass across it */
	uint8_t         *nr[LOOK_ROWS];         /* and after the noise reduction, the sharpening's input */
	int16_t         *cb[LOOK_ROWS], *cr[LOOK_ROWS];        /* chroma rows, 5 across summed */
	uint8_t         *cbcur, *crcur;         /* one chroma row before the box */
	float           sy[LOOK_ROWS];          /* the sensor line each row is from (the local contrast's map) */
	uint32_t        have_w, have_cw;
};
static __thread struct look_rows look_rows;

static inline uint32_t
look_slot(uint32_t row)
{
	return row % LOOK_ROWS;
}

static inline int
clamp_int(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static inline uint32_t
clamp_row(int r, uint32_t lo, uint32_t hi)
{
	return (uint32_t)(r < (int)lo ? (int)lo : r > (int)hi ? (int)hi : r);
}

static bool
look_alloc(uint32_t width)
{
	struct look_rows *r = &look_rows;
	uint32_t cw = (width + 1) / 2;

	if (width > r->have_w) {
		for (int i = 0; i < LOOK_ROWS; i++) {
			free(r->luma[i]);
			free(r->h[i]);
			free(r->nr[i]);
			r->luma[i] = malloc(width);
			r->h[i] = malloc(width);
			r->nr[i] = malloc(width);
		}
		r->have_w = width;
	}
	if (cw > r->have_cw) {
		for (int i = 0; i < LOOK_ROWS; i++) {
			free(r->cb[i]);
			free(r->cr[i]);
			r->cb[i] = malloc(sizeof(int16_t) * cw);
			r->cr[i] = malloc(sizeof(int16_t) * cw);
		}
		free(r->cbcur);
		free(r->crcur);
		r->cbcur = malloc(cw);
		r->crcur = malloc(cw);
		r->have_cw = cw;
	}
	for (int i = 0; i < LOOK_ROWS; i++) {
		if (r->luma[i] == NULL || r->h[i] == NULL || r->nr[i] == NULL || r->cb[i] == NULL || r->cr[i] == NULL) {
			return false;
		}
	}
	return r->cbcur != NULL && r->crcur != NULL;
}

/* the row's Y as BT.601 makes it of the 8-bit R, G, B (the same coefficients as nv12_pair's full range) */
static void
look_luma_row(const uint8_t *rgb, uint8_t *out, uint32_t width)
{
	for (uint32_t x = 0; x < width; x++, rgb += 3) {
		out[x] = (uint8_t)((19595 * rgb[0] + 38470 * rgb[1] + 7471 * rgb[2] + 32768) >> 16);
	}
}

/* the noise reduction's pass across a row: the mean of the cells around, each weighted by how near its Y is
 * to the middle's (exp(-(dY/sigma)^2), w[] of the 256 differences), so an edge does not go soft */
static void
look_h_row(const uint8_t *in, uint8_t *out, uint32_t width, int radius, const float *w)
{
	for (uint32_t x = 0; x < width; x++) {
		int c = in[x];
		float sum = 0, weights = 0;

		for (int dx = -radius; dx <= radius; dx++) {
			int v = in[clamp_int((int)x + dx, 0, (int)width - 1)];
			float weight = w[v > c ? v - c : c - v];

			sum += (float)v * weight;
			weights += weight;
		}
		out[x] = (uint8_t)(sum / weights + 0.5f);
	}
}

/* and down a row: the same weights, of the rows above and below it (clamped into the piece at its ends) */
static void
look_nr_row(const struct look_rows *r, uint32_t row, uint32_t lo, uint32_t hi, int radius, const float *w, bool nr,
    uint32_t width)
{
	unsigned long long began = now_ns();
	uint8_t *out = r->nr[look_slot(row)];

	if (!nr) {
		memcpy(out, r->luma[look_slot(row)], width);
	} else {
		const uint8_t *c = r->h[look_slot(row)];

		for (uint32_t x = 0; x < width; x++) {
			int cv = c[x];
			float sum = 0, weights = 0;

			for (int dy = -radius; dy <= radius; dy++) {
				int v = r->h[look_slot(clamp_row((int)row + dy, lo, hi))][x];
				float weight = w[v > cv ? v - cv : cv - v];

				sum += (float)v * weight;
				weights += weight;
			}
			out[x] = (uint8_t)(sum / weights + 0.5f);
		}
	}
	look_add(0, now_ns() - began);
}

/* the stills' local contrast map: the Y of a 33x25 grid of the frame's blocks (a box of 1/32 of the width),
 * made from the sensor's own cells (the lens shading and the white balance on them, as the picture has them) */
static void
lcon_map(const struct isp *isp, const struct hakios_camera_frame *f, const uint8_t *raw, const uint8_t *lut,
    uint8_t map[LOOK_LCON_H][LOOK_LCON_W])
{
	uint32_t cells_w = f->width / 2, cells_h = f->height / 2;
	int r, b, g1 = -1, g2 = -1;

	cell_places(f->bayer, &r, &b);
	for (int i = 0; i < 4; i++) {
		if (i != r && i != b) {
			if (g1 < 0) {
				g1 = i;
			} else {
				g2 = i;
			}
		}
	}
	if (cells_w == 0 || cells_h == 0) {
		memset(map, 128, sizeof(uint8_t) * LOOK_LCON_H * LOOK_LCON_W);
		return;
	}
	for (int gy = 0; gy < LOOK_LCON_H; gy++) {
		uint32_t cy0 = (uint32_t)((uint64_t)gy * cells_h / LOOK_LCON_H), cy1;
		uint32_t cy_at = (uint32_t)((uint64_t)(gy + 1) * cells_h / LOOK_LCON_H);

		cy1 = cy_at > cy0 ? cy_at : cy0 + 1;
		cy1 = cy1 > cells_h ? cells_h : cy1;
		for (int gx = 0; gx < LOOK_LCON_W; gx++) {
			uint32_t cx0 = (uint32_t)((uint64_t)gx * cells_w / LOOK_LCON_W), cx1;
			uint32_t cx_at = (uint32_t)((uint64_t)(gx + 1) * cells_w / LOOK_LCON_W);
			double sum = 0;
			int n = 0;

			cx1 = cx_at > cx0 ? cx_at : cx0 + 1;
			cx1 = cx1 > cells_w ? cells_w : cx1;
			/* nine cells spread over the block, not all of them: a cheap picture of where the light is */
			for (int j = 0; j < 3; j++) {
				uint32_t cy = cy0 + (uint32_t)((uint64_t)(2 * j + 1) * (cy1 - cy0) / 6);
				float v_pos;

				cy = cy >= cells_h ? cells_h - 1 : cy;
				v_pos = cells_h > 1 ? (float)cy / (cells_h - 1) : 0;
				for (int i = 0; i < 3; i++) {
					uint32_t cx = cx0 + (uint32_t)((uint64_t)(2 * i + 1) * (cx1 - cx0) / 6);
					float gains[3] = { 1, 1, 1 }, y;
					int v[4], index;

					cx = cx >= cells_w ? cells_w - 1 : cx;
					cell(raw + (size_t)cy * 2 * f->stride, f->stride, cx, (int)f->black, v);
					if (isp->lsc.on) {
						lsc_at(isp, cells_w > 1 ? (float)cx / (cells_w - 1) : 0, v_pos, gains);
					}
					gains[0] *= isp->wb[0] * isp->digital;
					gains[1] *= isp->wb[1] * isp->digital;
					gains[2] *= isp->wb[2] * isp->digital;
					y = 0.2126f * v[r] * gains[0] + 0.7152f * (v[g1] + v[g2]) * 0.5f * gains[1] +
					    0.0722f * v[b] * gains[2];
					index = (int)(y * (LUT_WHITE / (float)WHITE) + 0.5f);
					sum += lut[clamp_int(index, 0, 4095)];
					n++;
				}
			}
			map[gy][gx] = (uint8_t)(sum / n + 0.5);
		}
	}
}

/* the local contrast's blur where the sensor is at (u, v), 0 to 1 across it: the map, between its points */
static int
lcon_at(const uint8_t map[LOOK_LCON_H][LOOK_LCON_W], float u, float v)
{
	float mx = (u < 0 ? 0 : u > 1 ? 1 : u) * (LOOK_LCON_W - 1), my = (v < 0 ? 0 : v > 1 ? 1 : v) * (LOOK_LCON_H - 1);
	int x = clamp_int((int)mx, 0, LOOK_LCON_W - 2), y = clamp_int((int)my, 0, LOOK_LCON_H - 2);
	float fx = mx - x, fy = my - y, top, bottom;

	fx = fx < 0 ? 0 : fx > 1 ? 1 : fx;
	fy = fy < 0 ? 0 : fy > 1 ? 1 : fy;
	top = map[y][x] * (1 - fx) + map[y][x + 1] * fx;
	bottom = map[y + 1][x] * (1 - fx) + map[y + 1][x + 1] * fx;
	return (int)(top * (1 - fy) + bottom * fy + 0.5f);
}

/* one row out: the sharpening's unsharp mask (a 3x3 Gaussian of radius 1, the threshold, the +-24 halo limit),
 * then the local contrast, then the picture's range */
static void
look_out_row(const struct look_rows *r, uint32_t row, uint32_t lo, uint32_t hi, float amount, float lcon,
    const uint8_t map[LOOK_LCON_H][LOOK_LCON_W], uint32_t width, float sx_at, float sx_step, uint32_t fw, uint32_t fh,
    bool turned, bool video_range, uint8_t *dst)
{
	const uint8_t *a = r->nr[look_slot(clamp_row((int)row - 1, lo, hi))];
	const uint8_t *c = r->nr[look_slot(row)];
	const uint8_t *b = r->nr[look_slot(clamp_row((int)row + 1, lo, hi))];
	float sy = r->sy[look_slot(row)];
	unsigned long long began = now_ns(), sharpened;

	for (uint32_t x = 0; x < width; x++) {
		int xm = x > 0 ? (int)x - 1 : 0, xp = x + 1 < width ? (int)x + 1 : (int)width - 1;
		int y = c[x], blur, delta, out = y;

		blur = (a[xm] + 2 * a[x] + a[xp] + 2 * (c[xm] + 2 * c[x] + c[xp]) + b[xm] + 2 * b[x] + b[xp] + 8) >> 4;
		delta = y - blur;
		/* (a soft threshold: what is under it is noise, and what is over loses just it) */
		if (delta > LOOK_THRESHOLD) {
			delta = (int)((delta - LOOK_THRESHOLD) * amount + 0.5f);
		} else if (delta < -LOOK_THRESHOLD) {
			delta = (int)((delta + LOOK_THRESHOLD) * amount - 0.5f);
		} else {
			delta = 0;
		}
		delta = clamp_int(delta, -LOOK_HALO, LOOK_HALO);
		out = clamp_int(y + delta, 0, 255);
		dst[x] = (uint8_t)out;
	}
	sharpened = now_ns();

	if (lcon > 0) {
		for (uint32_t x = 0; x < width; x++) {
			/* (a turned picture's line is back to front: the sensor column of what is at x is the other way) */
			uint32_t sx = turned ? width - 1 - x : x;
			float u = fw > 1 ? (sx_at + (float)sx * sx_step) / (fw - 1) : 0;
			float v = fh > 1 ? sy / (fh - 1) : 0;
			int blur = lcon_at(map, u, v);
			int out = dst[x] + (int)(lcon * (dst[x] - blur) + (dst[x] >= blur ? 0.5f : -0.5f));

			dst[x] = (uint8_t)clamp_int(out, 0, 255);
		}
	}
	for (uint32_t x = 0; x < width; x++) {
		int out = dst[x];

		if (video_range) {
			out = 16 + (out * 219 + 127) / 255;
			out = clamp_int(out, 16, 235);
		}
		dst[x] = (uint8_t)out;
	}
	look_add(1, sharpened - began);
	look_add(2, now_ns() - sharpened);
}

/* a pair's chroma at half resolution, as BT.601 makes it of the four pixels; kept 5 across summed (when the
 * noise reduction is on: without it the row is kept as it is) */
static void
look_chroma_push(struct look_rows *r, const uint8_t *rgb0, const uint8_t *rgb1, uint32_t width, uint32_t pair, bool nr)
{
	static const int32_t k[9] = { 19595, 38470, 7471, -11058, -21710, 32768, 32768, -27439, -5329 };
	uint32_t cw = (width + 1) / 2;
	int16_t *hsb = r->cb[look_slot(pair)], *hsr = r->cr[look_slot(pair)];
	unsigned long long began = now_ns();

	for (uint32_t x = 0; x < cw; x++) {
		uint32_t x0 = 2 * x, x1 = x0 + 1 < width ? x0 + 1 : x0;
		int32_t R = rgb0[3 * x0] + rgb0[3 * x1] + rgb1[3 * x0] + rgb1[3 * x1];
		int32_t G = rgb0[3 * x0 + 1] + rgb0[3 * x1 + 1] + rgb1[3 * x0 + 1] + rgb1[3 * x1 + 1];
		int32_t B = rgb0[3 * x0 + 2] + rgb0[3 * x1 + 2] + rgb1[3 * x0 + 2] + rgb1[3 * x1 + 2];
		int32_t cb = (k[3] * R + k[4] * G + k[5] * B + (128 << 18) + (1 << 17) - 1) >> 18;
		int32_t cr = (k[6] * R + k[7] * G + k[8] * B + (128 << 18) + (1 << 17) - 1) >> 18;

		r->cbcur[x] = (uint8_t)(cb < 0 ? 0 : cb > 255 ? 255 : cb);
		r->crcur[x] = (uint8_t)(cr < 0 ? 0 : cr > 255 ? 255 : cr);
	}
	for (uint32_t x = 0; x < cw; x++) {
		if (nr) {
			int sb = 0, sr = 0;

			for (int dx = -2; dx <= 2; dx++) {
				int xx = clamp_int((int)x + dx, 0, (int)cw - 1);

				sb += r->cbcur[xx];
				sr += r->crcur[xx];
			}
			hsb[x] = (int16_t)sb;
			hsr[x] = (int16_t)sr;
		} else {
			hsb[x] = r->cbcur[x];
			hsr[x] = r->crcur[x];
		}
	}
	look_add(0, now_ns() - began);
}

/* one chroma row out: the 5x5 box of the rows around (clamped into the piece), the vibrance, the range */
static void
look_uv_row(const struct look_rows *r, uint32_t pair, uint32_t lo, uint32_t hi, uint32_t cw, bool nr, float vibrance,
    bool video_range, uint8_t *uv)
{
	unsigned long long began = now_ns();

	for (uint32_t x = 0; x < cw; x++) {
		int cb = 0, cr = 0;

		if (nr) {
			for (int dy = -2; dy <= 2; dy++) {
				uint32_t row = clamp_row((int)pair + dy, lo, hi);

				cb += r->cb[look_slot(row)][x];
				cr += r->cr[look_slot(row)][x];
			}
			cb = (cb + 12) / 25;
			cr = (cr + 12) / 25;
		} else {
			cb = r->cb[look_slot(pair)][x];
			cr = r->cr[look_slot(pair)][x];
		}
		if (vibrance > 0) {
			int dcb = cb - 128, dcr = cr - 128;
			int mag = dcb > 0 ? dcb : -dcb, other = dcr > 0 ? dcr : -dcr;
			float s;

			mag = other > mag ? other : mag;
			s = 1 + vibrance * (1 - (float)mag / 127);
			cb = 128 + (int)(dcb * s + (dcb >= 0 ? 0.5f : -0.5f));
			cr = 128 + (int)(dcr * s + (dcr >= 0 ? 0.5f : -0.5f));
		}
		if (video_range) {
			cb = 128 + (cb - 128) * 224 / 255;
			cr = 128 + (cr - 128) * 224 / 255;
		}
		uv[2 * x] = (uint8_t)clamp_int(cb, 0, 255);
		uv[2 * x + 1] = (uint8_t)clamp_int(cr, 0, 255);
	}
	look_add(0, now_ns() - began);
}

/* the generator: two output rows of the photograph at a time (demosaic, scale, the matrix and the curve) */
struct still_gen {
	const struct isp *isp;
	const struct hakios_camera_frame *f;
	const uint8_t *raw;
	uint32_t        width, height, x0, y0, cw, ch, fw;
	float           sx_step, sy_step;
	int             colour[4];
	struct pipe     p;
	int32_t         cached[2];
	int             *lines[3];
	uint8_t         *rows[2], *out[2];
	float           sy[2];          /* the sensor line each of the two is from */
};

static bool
still_gen_init(struct still_gen *g, const struct isp *isp, const struct hakios_camera_frame *f, const uint8_t *raw,
    uint32_t width, uint32_t height)
{
	static __thread int *lines[3];
	static __thread uint8_t *rows[2], *out[2];
	static __thread uint32_t have_fw, have_width;

	memset(g, 0, sizeof(*g));
	g->isp = isp;
	g->f = f;
	g->raw = raw;
	g->width = width;
	g->height = height;
	g->fw = f->width;
	g->cached[0] = g->cached[1] = -1;
	if (f->width > have_fw) {
		for (int i = 0; i < 3; i++) {
			free(lines[i]);
			lines[i] = malloc(sizeof(int) * f->width);
		}
		for (int i = 0; i < 2; i++) {
			free(rows[i]);
			rows[i] = malloc(3 * (size_t)f->width);
		}
		have_fw = f->width;
	}
	if (width > have_width) {
		for (int i = 0; i < 2; i++) {
			free(out[i]);
			out[i] = malloc(3 * (size_t)width);
		}
		have_width = width;
	}
	if (lines[2] == NULL || rows[1] == NULL || out[1] == NULL) {
		return false;
	}
	for (int i = 0; i < 3; i++) {
		g->lines[i] = lines[i];
	}
	for (int i = 0; i < 2; i++) {
		g->rows[i] = rows[i];
		g->out[i] = out[i];
	}
	crop(isp, f, width, height, &g->x0, &g->y0, &g->cw, &g->ch);
	cfa(f->bayer, g->colour);
	pipe_init(isp, &g->p, true);
	g->sx_step = 2.0f * g->cw / width;
	g->sy_step = 2.0f * g->ch / height;
	return true;
}

static void
still_gen_rows(struct still_gen *g, uint32_t row)
{
	const struct isp *isp = g->isp;
	const struct hakios_camera_frame *f = g->f;
	uint32_t fw = g->fw;

	for (int k = 0; k < 2; k++) {
		uint32_t r = row + k < g->height ? row + k : row, at = isp->turned ? g->height - 1 - r : r;
		float sy = 2.0f * g->y0 + (at + 0.5f) * g->sy_step - 0.5f, ty;
		uint32_t sy0 = sy < 0 ? 0 : (uint32_t)sy, sy1, slot[2];

		sy0 = sy0 > f->height - 1 ? f->height - 1 : sy0;
		sy1 = sy0 + 1 > f->height - 1 ? f->height - 1 : sy0 + 1;
		ty = sy < (float)sy0 ? 0 : sy - sy0;
		g->sy[k] = sy;
		/* the two lines of the sensor it is between, demosaiced (kept from the line before where they are the same) */
		for (int n = 0; n < 2; n++) {
			uint32_t want = n ? sy1 : sy0;

			if (g->cached[0] == (int32_t)want || g->cached[1] == (int32_t)want) {
				slot[n] = g->cached[0] == (int32_t)want ? 0 : 1;
				continue;
			}
			/* (into the slot the other line does not need: sy0's for sy1, and sy1's kept for sy0) */
			slot[n] = n == 1 ? slot[0] ^ 1 : g->cached[0] == (int32_t)sy1 ? 1 : 0;
			demosaic_row(isp, f, g->raw, want, g->colour, &g->p, isp->look.rolloff > 0 ? isp->gamma_still : isp->gamma,
			    g->lines, g->rows[slot[n]]);
			g->cached[slot[n]] = (int32_t)want;
		}
		const uint8_t *a = g->rows[slot[0]], *b = g->rows[slot[1]];
		uint8_t *o = g->out[k];

		for (uint32_t x = 0; x < g->width; x++) {
			float sx = 2.0f * g->x0 + (x + 0.5f) * g->sx_step - 0.5f, tx;
			uint32_t sx0 = sx < 0 ? 0 : (uint32_t)sx, sx1;

			sx0 = sx0 > fw - 1 ? fw - 1 : sx0;
			sx1 = sx0 + 1 > fw - 1 ? fw - 1 : sx0 + 1;
			tx = sx < (float)sx0 ? 0 : sx - sx0;
			uint8_t *px = o + 3 * (isp->turned ? g->width - 1 - x : x);

			for (int c = 0; c < 3; c++) {
				float top = a[3 * sx0 + c] + tx * (a[3 * sx1 + c] - a[3 * sx0 + c]);
				float bottom = b[3 * sx0 + c] + tx * (b[3 * sx1 + c] - b[3 * sx0 + c]);

				px[c] = (uint8_t)(top + ty * (bottom - top) + 0.5f);
			}
		}
	}
}

/* the look's rows through the noise reduction, the sharpening, the local contrast and the vibrance, into y and uv */
static void
look_render(struct still_gen *g, uint8_t *y, size_t y_stride, uint8_t *uv, size_t uv_stride, uint32_t lo, uint32_t hi,
    bool video_range, const uint8_t *lut)
{
	const struct isp *isp = g->isp;
	struct look_rows *rows = &look_rows;
	uint32_t width = g->width;
	float gain = g->f->gain_unity ? (float)g->f->gain / g->f->gain_unity : 1;
	uint8_t map[LOOK_LCON_H][LOOK_LCON_W];
	float w[256];
	float amount, lcon = isp->look.lcon, vibrance = isp->look.vibrance;
	bool nr = isp->look.nr > 0;
	int radius;
	uint32_t next_nr = lo, next_out = lo, nr_hi = lo, gen_hi = lo - 1;
	uint32_t next_uv = lo / 2, uv_hi = lo / 2 - 1;
	bool have_nr = false;

	if (!look_alloc(width)) {
		return;
	}
	gain = gain < 1 ? 1 : gain > 16 ? 16 : gain;
	radius = gain >= 8 ? 2 : 1;
	if (nr) {
		double sigma = 3.0 * isp->look.nr * sqrt((double)gain);

		for (int d = 0; d < 256; d++) {
			w[d] = (float)exp(-(double)d * d / (sigma * sigma));
		}
	}
	/* the sharpening: 0.6 at 1x gain, 0.25 at 16x (the noise reduction has taken the noise by then) */
	amount = isp->look.sharpen * (0.6f - 0.35f * (gain - 1) / 15);
	if (lcon > 0) {
		lcon_map(isp, g->f, g->raw, lut, map);
	}

	for (uint32_t row = lo; row <= hi; row += 2) {
		still_gen_rows(g, row);
		for (int k = 0; k < 2; k++) {
			uint32_t at = row + k > hi ? row : row + k, slot = look_slot(at);
			unsigned long long began = now_ns();
			uint8_t *luma = rows->luma[slot];

			look_luma_row(g->out[k], luma, width);
			if (nr) {
				look_h_row(luma, rows->h[slot], width, radius, w);
			}
			rows->sy[slot] = g->sy[k];
			look_add(0, now_ns() - began);
			gen_hi = at;
		}
		look_chroma_push(rows, g->out[0], g->out[1], width, row / 2, nr);
		uv_hi = row / 2;

		while (next_nr <= gen_hi && next_nr + (uint32_t)radius <= gen_hi) {
			look_nr_row(rows, next_nr, lo, gen_hi, radius, w, nr, width);
			nr_hi = next_nr++;
			have_nr = true;
		}
		while (have_nr && next_out + 1 <= nr_hi) {
			look_out_row(rows, next_out, lo, nr_hi, amount, lcon, map, width,
			    2.0f * g->x0 + 0.5f * g->sx_step - 0.5f, g->sx_step, g->fw, g->f->height, isp->turned,
			    video_range, y + (size_t)next_out * y_stride);
			next_out++;
		}
		while (next_uv + 2 <= uv_hi) {
			look_uv_row(rows, next_uv, lo / 2, uv_hi, (width + 1) / 2, nr, vibrance, video_range,
			    uv + (size_t)next_uv * uv_stride);
			next_uv++;
		}
	}
	/* the piece's last rows: their neighbours beyond it are their own (two rows of the same scene at most) */
	while (next_nr <= hi) {
		look_nr_row(rows, next_nr, lo, hi, radius, w, nr, width);
		nr_hi = next_nr++;
		have_nr = true;
	}
	while (have_nr && next_out <= hi) {
		look_out_row(rows, next_out, lo, nr_hi, amount, lcon, map, width,
		    2.0f * g->x0 + 0.5f * g->sx_step - 0.5f, g->sx_step, g->fw, g->f->height, isp->turned, video_range,
		    y + (size_t)next_out * y_stride);
		next_out++;
	}
	while (next_uv <= hi / 2) {
		look_uv_row(rows, next_uv, lo / 2, hi / 2, (width + 1) / 2, nr, vibrance, video_range,
		    uv + (size_t)next_uv * uv_stride);
		next_uv++;
	}
}

void
isp_render_still_nv12(const struct isp *isp, const struct hakios_camera_frame *f, const uint8_t *raw, uint8_t *y, size_t y_stride,
    uint8_t *uv, size_t uv_stride, uint32_t width, uint32_t height, uint32_t first_row, uint32_t end_row, bool video_range)
{
	struct still_gen g;
	uint32_t lo = first_row & ~1u, hi;
	bool filters = isp->look.nr > 0 || isp->look.sharpen > 0 || isp->look.lcon > 0 || isp->look.vibrance > 0;
	const uint8_t *lut = isp->look.rolloff > 0 ? isp->gamma_still : isp->gamma;

	if (width == 0 || height == 0 || end_row <= lo) {
		return;
	}
	hi = end_row > height ? height : end_row;
	hi--;
	if (hi < lo) {
		return;
	}
	if (!still_gen_init(&g, isp, f, raw, width, height)) {
		return;
	}
	if (filters) {
		look_render(&g, y, y_stride, uv, uv_stride, lo, hi, video_range, lut);
		return;
	}
	for (uint32_t row = lo; row <= hi; row += 2) {
		bool second = row + 1 < height;

		still_gen_rows(&g, row);
		nv12_pair(g.out[0], g.out[1], width, y + (size_t)row * y_stride, second ? y + (size_t)(row + 1) * y_stride : NULL,
		    uv + (size_t)(row / 2) * uv_stride, video_range);
	}
}

/* ---- lens shading from a flat field --------------------------------------------------------------------- */

bool
isp_lsc_from_flat(struct isp *isp, const struct hakios_camera_frame *f, const uint8_t *raw, float *min_ratio)
{
	uint32_t cells_w = f->width / 2, cells_h = f->height / 2, half_w = cells_w / (LSC_W - 1) / 2, half_h = cells_h / (LSC_H - 1) / 2;
	float level[3][LSC_H][LSC_W], top[3] = { 0, 0, 0 };
	uint32_t clipped = 0, seen = 0;
	int r, b;

	cell_places(f->bayer, &r, &b);
	for (int gy = 0; gy < LSC_H; gy++) {
		for (int gx = 0; gx < LSC_W; gx++) {
			int cx0 = (int)((uint64_t)gx * (cells_w - 1) / (LSC_W - 1)) - (int)half_w;
			int cy0 = (int)((uint64_t)gy * (cells_h - 1) / (LSC_H - 1)) - (int)half_h;
			double sum[3] = { 0, 0, 0 };
			uint32_t n = 0;

			for (int cy = cy0; cy <= cy0 + 2 * (int)half_h; cy += 2) {
				for (int cx = cx0; cx <= cx0 + 2 * (int)half_w; cx += 2) {
					int v[4];

					if (cx < 0 || cy < 0 || cx >= (int)cells_w || cy >= (int)cells_h) {
						continue;
					}
					cell(raw + (size_t)cy * 2 * f->stride, f->stride, (uint32_t)cx, (int)f->black, v);
					sum[0] += v[r];
					sum[2] += v[b];
					for (int j = 0; j < 4; j++) {
						sum[1] += j != r && j != b ? v[j] / 2.0 : 0;
						clipped += v[j] > WHITE - 30;
					}
					seen += 4;
					n++;
				}
			}
			for (int c = 0; c < 3; c++) {
				level[c][gy][gx] = n ? (float)(sum[c] / n) : 0;
				top[c] = level[c][gy][gx] > top[c] ? level[c][gy][gx] : top[c];
			}
		}
	}
	/* bright enough to tell shading from noise, and not clipped anywhere that counts */
	if (top[1] < 100 || clipped * 1000 > seen) {
		return false;
	}
	float darkest = 1;

	for (int c = 0; c < 3; c++) {
		for (int gy = 0; gy < LSC_H; gy++) {
			for (int gx = 0; gx < LSC_W; gx++) {
				float l = level[c][gy][gx] > 1 ? level[c][gy][gx] : 1;

				isp->lsc.gain[c][gy][gx] = top[c] / l;
				if (c == 1 && l / top[c] < darkest) {
					darkest = l / top[c];
				}
			}
		}
	}
	isp->lsc.on = true;
	if (min_ratio != NULL) {
		*min_ratio = darkest;
	}
	return true;
}

/* one module's part of a tuning file (tools/camtuning.py): its ranges, and where each range's values are */
static bool
tune_set(const uint8_t **p, const uint8_t *end, struct isp_tune_set *set, size_t floats)
{
	uint32_t n;

	if (end - *p < 4) {
		return false;
	}
	memcpy(&n, *p, 4);
	*p += 4;
	if (n == 0 || n > TUNE_LIGHTS) {
		return false;
	}
	set->lights = n;
	for (uint32_t i = 0; i < n; i++) {
		if (end - *p < 12) {
			return false;
		}
		memcpy(&set->light[i].lux_from, *p, 4);
		memcpy(&set->light[i].lux_to, *p + 4, 4);
		memcpy(&set->light[i].ccts, *p + 8, 4);
		*p += 12;
		if (set->light[i].ccts == 0 || set->light[i].ccts > TUNE_CCTS) {
			return false;
		}
		for (uint32_t j = 0; j < set->light[i].ccts; j++) {
			if ((size_t)(end - *p) < (2 + floats) * 4) {
				return false;
			}
			memcpy(&set->light[i].cct_from[j], *p, 4);
			memcpy(&set->light[i].cct_to[j], *p + 4, 4);
			set->light[i].v[j] = (const float *)(const void *)(*p + 8);     /* (every field is 4 bytes) */
			*p += (2 + floats) * 4;
		}
	}
	return true;
}

bool
isp_load_tuning(struct isp *isp, const char *path)
{
	struct isp_tuning *t = calloc(1, sizeof(*t));
	int fd = open(path, O_RDONLY);
	struct stat st;
	const uint8_t *p, *end;
	bool good = false;

	if (isp->tuning != NULL) {
		free(isp->tuning->file);
		free(isp->tuning);
		isp->tuning = NULL;
	}
	if (t != NULL && fd >= 0 && fstat(fd, &st) == 0 && st.st_size > 8 && st.st_size < (16 << 20) &&
	    (t->file = malloc((size_t)st.st_size)) != NULL && read(fd, t->file, (size_t)st.st_size) == st.st_size) {
		p = t->file;
		end = p + st.st_size;
		good = memcmp(p, "RWCISP02", 8) == 0;
		p += 8;
		good = good && tune_set(&p, end, &t->ccm, 9) && tune_set(&p, end, &t->lsc, 4 * LSC_W * LSC_H) && end - p >= 4;
		if (good) {
			memcpy(&t->refs, p, 4);
			p += 4;
			good = t->refs <= 16 && (size_t)(end - p) >= t->refs * 12;
		}
		if (good) {
			memcpy(t->ref, p, t->refs * 12);
		}
	}
	if (fd >= 0) {
		close(fd);
	}
	if (!good) {
		if (t != NULL) {
			free(t->file);
		}
		free(t);
		return false;
	}
	isp->tuning = t;
	return true;
}

/*
 * Where x falls among ascending ranges [from[i], to[i]]: in one (a = b), or between two (a, b and how
 * far from a's end to b's start, 0 to 1); before the first, the first; after the last, the last.
 */
static void
pick(const float *from, const float *to, uint32_t n, float x, uint32_t *a, uint32_t *b, float *t)
{
	*a = *b = n - 1;
	*t = 0;
	for (uint32_t i = 0; i < n; i++) {
		if (x <= to[i]) {
			*a = *b = i;
			if (i > 0 && x < from[i]) {
				*a = i - 1;
				*t = (x - to[i - 1]) / (from[i] - to[i - 1]);
			}
			return;
		}
	}
}

/* a module's values for this light and temperature: those of the ranges they fall in, or between */
static void
tune_blend(const struct isp_tune_set *set, float lux_index, float cct, size_t floats, float *out)
{
	float from[TUNE_LIGHTS], to[TUNE_LIGHTS], tl;
	uint32_t la, lb;

	for (uint32_t i = 0; i < set->lights; i++) {
		from[i] = set->light[i].lux_from;
		to[i] = set->light[i].lux_to;
	}
	pick(from, to, set->lights, lux_index, &la, &lb, &tl);
	memset(out, 0, floats * sizeof(float));
	for (int k = 0; k < 2; k++) {
		uint32_t l = k ? lb : la, ca, cb;
		float w = k ? tl : 1 - tl, tc;

		if (w == 0) {
			continue;
		}
		pick(set->light[l].cct_from, set->light[l].cct_to, set->light[l].ccts, cct, &ca, &cb, &tc);
		for (size_t i = 0; i < floats; i++) {
			out[i] += w * ((1 - tc) * set->light[l].v[ca][i] + tc * set->light[l].v[cb][i]);
		}
	}
}

void
isp_tune(struct isp *isp, float lux_index, float cct)
{
	const struct isp_tuning *t = isp->tuning;
	float m[9], g[4 * LSC_H * LSC_W];
	const int grid = LSC_H * LSC_W;

	if (t == NULL) {
		return;
	}
	tune_blend(&t->ccm, lux_index, cct, 9, m);
	isp_set_ccm(isp, m);
	tune_blend(&t->lsc, lux_index, cct, 4 * (size_t)grid, g);
	for (int gy = 0; gy < LSC_H; gy++) {
		for (int gx = 0; gx < LSC_W; gx++) {
			int at = gy * LSC_W + gx;

			isp->lsc.gain[0][gy][gx] = g[at];
			isp->lsc.gain[1][gy][gx] = (g[grid + at] + g[2 * grid + at]) / 2;
			isp->lsc.gain[2][gy][gx] = g[3 * grid + at];
		}
	}
	isp->lsc.on = true;
}
