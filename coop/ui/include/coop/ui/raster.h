/* SPDX-License-Identifier: GPL-3.0-or-later
   CoopRaster: draws a CoopDrawList (drawlist.h) into an RGBA picture in software (docs/ui.md, "ExeFS").

   For platforms with no drawing table of ours (an ExeFS build on an emulator or a console): the overlay's list becomes
   one RGBA picture the game shows with a picture pane of its own (v1 step 8 e2). Rects (rounded), circles and rings,
   triangles, convex polygons and their outlines, text from a font atlas and the HUD pack's images.

   The same pixels everywhere: every number is turned into 1/16 px integers once (an exact float step) and the rest is
   integer arithmetic, so GCC, Clang and MSVC, x64 and arm64, give the same bytes (tests/ui/raster_test.cpp pins them).
   Edges are anti-aliased: per pixel 4 sub-rows times 1/16 px of horizontal coverage.

   Plain C, no allocation (the caller gives every buffer), no function pointers, no floating point after the input:
   it works inside a payload lifted by a recomp and on a console.

   The picture: 0xAABBGGRR per pixel (bytes R G B A in memory), PREMULTIPLIED alpha, cleared to 0 each frame, so it
   goes over the game with "one, one minus source alpha" blending; coop_raster_to_straight converts rows for a pane
   that wants straight alpha. The picture belongs to the raster: only coop_raster_draw writes it (it clears just the
   rows it drew last frame and relies on the rest being 0).

   Dirty rows: each frame compares every drawn row with the last frame's (a 64-bit hash of the row's drawn part), so
   a backend uploads only the bands that changed (coop_raster_dirty). */
#ifndef COOP_UI_RASTER_H
#define COOP_UI_RASTER_H
#include <stddef.h>
#include <stdint.h>

#include "coop/ui/drawlist.h"

#ifdef __cplusplus
extern "C" {
#endif

#define COOP_RASTER_MAX_SIDE 8192 /* the widest and tallest picture */

/* One glyph of a font atlas. Atlas pixels; the pen is on the line's top (the text command's y), not the baseline. */
typedef struct CoopRasterGlyph {
    uint32_t codepoint;
    uint16_t x, y, w, h; /* its coverage in the atlas */
    int16_t ox, oy;      /* where its top left goes, from the pen */
    uint16_t advance64;  /* how far the pen then moves, in 1/64 atlas px */
    uint16_t pad_;
} CoopRasterGlyph;

/* A font atlas (built on the player's PC from the HUD pack; no game data in this repository). A text drawn at `size`
   px uses the atlas 1:1; other sizes scale it (bilinear). */
typedef struct CoopRasterFont {
    uint32_t size;             /* the text px the atlas was made for (> 0) */
    uint32_t line64;           /* a new line ('\n') moves the pen down this far, 1/64 atlas px */
    uint32_t missing64;        /* a codepoint not in the table: nothing drawn, the pen moves this far */
    uint32_t atlas_w, atlas_h;
    const uint8_t *alpha;      /* atlas_w x atlas_h coverage bytes, row by row */
    uint32_t n;
    const CoopRasterGlyph *glyphs; /* sorted by codepoint, ascending */
} CoopRasterFont;

/* One of the HUD pack's images (COOP_IMAGE_*): 0xAABBGGRR, straight alpha, w x h row by row. */
typedef struct CoopRasterImage {
    uint32_t w, h;
    const uint32_t *rgba;
} CoopRasterImage;

/* Per picture row (the raster's own; in the caller's work buffer). */
typedef struct CoopRasterRow {
    uint64_t hash;       /* the last frame's row (0: nothing drawn) */
    int16_t x0, x1;      /* the part drawn this frame (x0 >= x1: none) */
    int16_t last_x0, last_x1;
    uint8_t dirty;       /* differs from the frame before */
    uint8_t pad_[7];
} CoopRasterRow;

/* Picture rows [y0, y1) */
typedef struct CoopRasterBand {
    uint16_t y0, y1;
} CoopRasterBand;

typedef struct CoopRaster {
    uint32_t *pixels;
    int32_t width, height, stride; /* stride: pixels from one row to the next */
    CoopRasterRow *rows;
    uint8_t *cov;  /* width bytes: one command's coverage on the current row */
    uint8_t *part; /* width bytes: one part's (outline bands) */
    const CoopRasterFont *font[COOP_FONT_COUNT];
    const CoopRasterImage *image[COOP_IMAGE_COUNT];
    uint32_t frames;
    uint32_t skipped; /* commands of the last frame not drawn: a font or image not set, bad numbers, unknown kind */
} CoopRaster;

/* The work buffer coop_raster_init needs for a width x height picture (0: too big or not positive). */
size_t coop_raster_work_bytes(int width, int height);
/* Sets the raster up on `pixels` (stride >= width) and `work` (coop_raster_work_bytes, 8-byte aligned); clears the
   picture. No fonts or images. 0 on bad arguments, else 1. */
int coop_raster_init(CoopRaster *r, uint32_t *pixels, int width, int height, int stride, void *work, size_t work_bytes);
/* The atlas for a logical font (COOP_FONT_*), the picture for a logical image (COOP_IMAGE_*); null: none (its texts
   or images are skipped). Kept by pointer: they must live as long as the raster draws with them. */
void coop_raster_set_font(CoopRaster *r, int font, const CoopRasterFont *f);
void coop_raster_set_image(CoopRaster *r, int image, const CoopRasterImage *img);

/* One frame: clears the picture, draws the list's commands in order (each over the ones before), clipped to the
   picture, then marks the rows that differ from the last frame. List coordinates are picture pixels. */
void coop_raster_draw(CoopRaster *r, const CoopDrawList *l);
/* The last frame's changed rows as bands, top to bottom; returns how many. More than `max`: the last one is widened to
   cover the rest (never misses a row). */
int coop_raster_dirty(const CoopRaster *r, CoopRasterBand *out, int max);

/* Premultiplied -> straight alpha, `n` pixels (src and dst may be the same, but never the raster's own picture). */
void coop_raster_to_straight(const uint32_t *src, uint32_t *dst, size_t n);

/* A font's advances as CoopFontAdvances (text widths for the layout, drawlist.h), into the caller's arrays of
   f->n entries each. */
void coop_raster_font_advances(const CoopRasterFont *f, uint32_t *codepoints, float *advances, CoopFontAdvances *out);

#ifdef __cplusplus
}
#endif
#endif
