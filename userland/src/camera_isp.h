/*
 * What the phone's ISP would do with the cameras' RAW10 frames, done by the processor instead
 * (src/camera_isp.c): the levels of a frame, the exposure for the next ones (AE), the white balance
 * (AWB), and a picture: each 2x2 Bayer cell one pixel of colour, scaled to the size asked for, or
 * every pixel (bilinear demosaic) at the sensor's own size; lens shading (a grid of gains per
 * colour), the white balance's and AE's gains, a colour matrix, a gamma; as 8-bit YCbCr 4:2:0 (NV12,
 * full range: CoreVideo's 420f, or video range: 420v) or BGRA. The preview is demosaiced at half
 * resolution -- one pixel per cell, scaled up -- and its inner loops are NEON where the processor has
 * it (isp_render_nv12_half(), struct isp_fast); the photographs get the stills' look (struct isp_look:
 * noise reduction, sharpening, local contrast, vibrance, a highlight roll-off), off by default.
 */
#ifndef CAMERA_ISP_H
#define CAMERA_ISP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "hakios.camera.h"

struct isp_stats {
	double          mean[3];        /* R, G, B above black, in 10-bit steps */
	double          bright;         /* the level of green that 1 % of the cells are above */
	uint32_t        hist[64];       /* green above black, 16 steps a bin */
	uint32_t        cells;
	/*
	 * Red over green and blue over green, lens-shaded, each cell's counted once however bright it is:
	 * the mean of the cells none of whose colours is near white or near black, and of those of them
	 * that are grey or nearly with the white balance as it is.
	 */
	double          grey[2], neutral[2];
	uint32_t        grey_cells, neutral_cells;
};

/*
 * Lens shading: a gain for each colour at each point of a grid laid over the sensor (corners on its
 * corners), between the points bilinear. Off until it is given one (isp_lsc_from_flat(), isp_tune()).
 */
#define LSC_W           17
#define LSC_H           13
struct isp_lsc {
	bool            on;
	float           gain[3][LSC_H][LSC_W];
};

/*
 * A camera's colour tuning, as tools/camtuning.py makes it out of the camera's tuning file of Android's
 * camera stack: colour matrices and lens shading grids, each per range of the scene's light (a lux
 * index, Qualcomm's measure: higher is darker) and within that per range of the light's colour
 * temperature.
 */
#define TUNE_LIGHTS     6
#define TUNE_CCTS       6
struct isp_tune_set {
	uint32_t        lights;
	struct {
		float           lux_from, lux_to;
		uint32_t        ccts;
		float           cct_from[TUNE_CCTS], cct_to[TUNE_CCTS];
		const float     *v[TUNE_CCTS];  /* a matrix by rows (9), or R, Gr, Gb and B grids (4 x 13 x 17) */
	} light[TUNE_LIGHTS];
};
struct isp_tuning {
	struct isp_tune_set ccm, lsc;
	/*
	 * The white balance's reference lights, from the bluest to the reddest: each as the sensor sees a
	 * grey under it (red over green, blue over green), and the colour temperature its tables are
	 * picked by (tools/camtuning.py). Between them, the line the white balance keeps to.
	 */
	uint32_t        refs;
	float           ref[16][3];
	void            *file;
};

/*
 * The "iPhone look" of the photographs (spec L): every field is off at 0, where the picture is exactly
 * what it was before; camera_device.c turns them on for vayu in the same place it sets the tone
 * (cam_tone=0 turns them off), and cam_nr=, cam_sharp=, cam_lcon= and cam_vib= are a tester's knobs.
 * The preview gets none of it but the sharpening (preview_sharp), and that one across the line only, to
 * keep the preview as cheap as it is: the rest is the stills' (isp_render_still_nv12) alone.
 */
struct isp_look {
	/* 0 none; 1: edge-preserving noise reduction on the stills' Y, sigma 3 codes * sqrt(gain) -- 3x3,
	 * 5x5 from 8x gain -- and a 5x5 box on the chroma at half resolution (look.nr scales the sigma) */
	float           nr;
	/* 0 none; 1: unsharp mask on the stills' Y, 0.6 at 1x gain falling to 0.25 at 16x, threshold 2.5 codes,
	 * the delta limited to +-24 (look.sharpen scales the amount) */
	float           sharpen;
	/* 0 none; 0.15: Y += lcon * (Y - the mean of the 1/32 of the width around it) (look.lcon is the amount) */
	float           lcon;
	/* 0 none; 0.25: saturation 1 + vibrance * (1 - the pixel's own chroma), so that what is already
	 * coloured is left alone; the colour matrix's folded saturation is off for the stills when this is on */
	float           vibrance;
	/* 0 none; 1: the last 10 % of the stills' gamma table rolled off, white landing at 97.5 % (249)
	 * instead of clipping, and flat where the shoulder ends (on vayu) */
	float           rolloff;
	/* 0 none; the preview's unsharp mask on Y, across the line only (0.3 on vayu) */
	float           preview_sharp;
};

/*
 * The fast paths (camera_device.c sets them from the boot arguments; isp_init() has them on): the
 * preview's debayer at half resolution (one pixel per 2x2 cell, then scaled), and the NEON code of the
 * inner loops where the processor has it. The NEON loops are the scalar ones' arithmetic exactly; the
 * half-resolution preview is the same cells interpolated again, a code or two from the old picture.
 * cam_preview_half=0 and cam_neon=0 put the old paths back.
 */
struct isp_fast {
	bool            preview_half;
	bool            neon;
};

struct isp {
	float           wb[3];          /* the white balance's gains, green 1 */
	float           digital;        /* more gain, for what AE cannot reach with the sensor's */
	float           zoom;           /* the picture is the middle 1/zoom of the sensor each way (1: the whole of it) */
	bool            turned;         /* the picture turned half round (a sensor mounted the other way up) */
	float           ccm[3][3];      /* the colour matrix, camera RGB (white balanced) to linear sRGB; rows sum to 1 */
	float           ccm_plain[3][3];        /* the same without the folded saturation (the stills' with vibrance) */
	struct isp_lsc  lsc;
	uint8_t         gamma[4096];    /* linear (959 above black is 3836) to 8 bits, sRGB's curve */
	uint8_t         gamma_still[4096];      /* the same with the look's highlight roll-off (stills only) */
	struct isp_look look;           /* the stills' look, and the preview's sharpening (all off by default) */
	uint32_t        asked_lines, asked_gain, asked_frame_lines;     /* what AE asked for last */
	bool            asked;
	bool            mode_jump;      /* the mode (or the sensor) changed: the next AE request may step as far as the
	                                 * measurement says, and is not damped -- the vendor block's exposure is a guess */
	bool            fixed_frame;    /* the sensor's frame length is the mode's own (the kernel's stills mode writes no
	                                 * 0x0340): AE must not ask for a longer frame, only for lines and gain */
	uint32_t        last_frame_lines, last_line_ns;  /* the frame the last AE step was made from (a change of either is a
	                                 * mode change: isp_ae() takes the next step undamped) */
	uint32_t        max_frame_lines;        /* the longest frame AE may ask for (0: twice the shortest) */
	struct isp_tuning *tuning;              /* isp_load_tuning()'s, or none */
	float           saturation;     /* applied to every colour matrix set (1: none) */
	float           contrast;       /* the S-curve folded into the gamma table (0: none) */
	struct isp_fast fast;           /* the preview's half-resolution debayer, the NEON loops */
};

void isp_init(struct isp *isp);
/* the fast paths (look NULL: the defaults -- both on) */
void isp_set_fast(struct isp *isp, const struct isp_fast *fast);
/* the tone of the picture: an S-curve of contrast c (0 none, ~0.3 gentle) into the gamma table, and saturation s
 * (1 none) applied to the colour matrix from now on */
void isp_set_tone(struct isp *isp, float contrast, float saturation);
/* the stills' look and the preview's sharpening (look NULL: none of it, today's pictures) */
void isp_set_look(struct isp *isp, const struct isp_look *look);
/* how the last photograph's time went, in ms (0 when the look was off); the counters are emptied */
struct isp_still_timing {
	double          nr_ms, sharpen_ms, lcon_ms;
};
void isp_take_still_timing(struct isp_still_timing *timing);
/*
 * How sharp the picture is in a part of the frame (fractions of it, x0 < x1, y0 < y1): the squares of
 * green's steps between neighbouring blocks of 4x4 cells, over green's level squared -- higher is sharper,
 * the same scene at any exposure the same; and green's mean level there (level, may be NULL).
 */
float isp_sharpness(const struct hakios_camera_frame *f, const uint8_t *raw, float x0, float y0, float x1, float y1, float *level);
/* the levels, from every 4th cell of every 4th pair of lines (with the processor's lens shading and white balance, if isp) */
void isp_measure(const struct isp *isp, const struct hakios_camera_frame *f, const uint8_t *raw, struct isp_stats *s);
/* the exposure for the frames to come: true, and the command filled in, when it should change */
bool isp_ae(struct isp *isp, const struct hakios_camera_frame *f, const struct isp_stats *s, struct hakios_camera_command *c);
/*
 * The frames now come from another mode of the sensor (or another sensor): the exposure the sensor has
 * is not the one AE asked for (the stills mode starts from the vendor block's own), so the next request
 * is made from the measurement alone and may step as far as it says (no damping, no "the last change is
 * not in the frames yet"). isp_ae() calls it itself for a frame whose frame length or line time changed;
 * camera_device.c calls it when the kernel's mode changes.
 */
void isp_mode_changed(struct isp *isp);
/*
 * The white balance: grey world -- of the nearly grey, when enough is -- kept to the line of the
 * tuning's reference lights when there is one, a little of the way at a time.
 */
void isp_awb(struct isp *isp, const struct isp_stats *s);
/*
 * A picture of the frame, width x height, of the sensor's middle part with the picture's proportions
 * (rows from first_row to end_row only, for doing it in pieces). NV12: y and uv planes, full range (420f) or video
 * range (420v); BGRA: y only.
 */
void isp_render_nv12(const struct isp *isp, const struct hakios_camera_frame *f, const uint8_t *raw, uint8_t *y, size_t y_stride,
    uint8_t *uv, size_t uv_stride, uint32_t width, uint32_t height, uint32_t first_row, uint32_t end_row, bool video_range);
/*
 * The moving pictures' preview, by the fast path when isp->fast.preview_half is on: the frame demosaiced
 * once per 2x2 cell (half the picture's width and height), and that cell picture scaled up to width x
 * height. A quarter of the demosaic, the colour matrix and the curve work; the same NV12 conversion
 * (isp_render_nv12, whose path it falls back to when the picture is smaller than the cell one).
 */
void isp_render_nv12_half(const struct isp *isp, const struct hakios_camera_frame *f, const uint8_t *raw, uint8_t *y,
    size_t y_stride, uint8_t *uv, size_t uv_stride, uint32_t width, uint32_t height, uint32_t first_row, uint32_t end_row,
    bool video_range);
void isp_render_bgra(const struct isp *isp, const struct hakios_camera_frame *f, const uint8_t *raw, uint8_t *out, size_t stride,
    uint32_t width, uint32_t height, uint32_t first_row, uint32_t end_row);
/*
 * Every pixel at the sensor's own size (f->width x f->height): the two colours a site does not have
 * from the same colour's sites around it (bilinear); rows from first_row to end_row (even), NV12.
 */
/*
 * A photograph, width x height: every pixel of the sensor's part the picture shows (the zoom's, turned as the
 * moving pictures are) demosaiced as isp_render_full_nv12() does, and scaled to the picture between the
 * two nearest lines and pixels; rows from first_row to end_row (even), NV12.
 */
void isp_render_still_nv12(const struct isp *isp, const struct hakios_camera_frame *f, const uint8_t *raw, uint8_t *y,
    size_t y_stride, uint8_t *uv, size_t uv_stride, uint32_t width, uint32_t height, uint32_t first_row, uint32_t end_row,
    bool video_range);
void isp_render_full_nv12(const struct isp *isp, const struct hakios_camera_frame *f, const uint8_t *raw, uint8_t *y, size_t y_stride,
    uint8_t *uv, size_t uv_stride, uint32_t first_row, uint32_t end_row, bool video_range);
/*
 * Lens shading measured from a frame of a flat field (a white sheet held over the lens, evenly lit):
 * each colour's level at each grid point (a block's mean) over the brightest of its own; false if
 * the frame is too dark or clipped for it. min_ratio: the darkest point's level over the brightest's.
 */
bool isp_lsc_from_flat(struct isp *isp, const struct hakios_camera_frame *f, const uint8_t *raw, float *min_ratio);
void isp_set_ccm(struct isp *isp, const float m[9]);
/* the tuning in PATH (tools/camtuning.py's) in place of the one there was: false, and none, if it is not one */
bool isp_load_tuning(struct isp *isp, const char *path);
/* the colour matrix and lens shading for a scene of this lux index and colour temperature, from the tuning */
void isp_tune(struct isp *isp, float lux_index, float cct);
/* the colour temperature the white balance is at, by the tuning's reference lights (0: there are none) */
float isp_cct(const struct isp *isp);

#endif
