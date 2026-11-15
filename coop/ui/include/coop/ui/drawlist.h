/* SPDX-License-Identifier: GPL-3.0-or-later
   CoopDrawList: what co-op's own UI layer draws this frame, as plain data (docs/ui.md).

   The co-op overlay (chat, name tags, HUD blocks, the player list, the panel, cheats, title lines, the down ring, map
   icons) appends commands here; a backend draws them: on Longshot coop_draw_hos.cpp replays them onto Hookshot's
   HosCoopDraw table (the same calls, in the same order, as before the list), on an ExeFS build a backend of its own.
   Plain C, fixed capacity, no allocation and no function pointers, so the same code works inside a payload lifted by
   a recomp (reports: v1-arch-b, inputs.md) and on a console.
   Coordinates: pixels of the picture from its top left (y down). Colours: 0xAABBGGRR (Dear ImGui's IM_COL32: red in
   the low byte). Fonts and images are LOGICAL ids (COOP_FONT_*, COOP_IMAGE_*): the backend maps them to its own. */
#ifndef COOP_UI_DRAWLIST_H
#define COOP_UI_DRAWLIST_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define COOP_DRAW_MAX_CMDS 2048
#define COOP_DRAW_MAX_TEXT 32768   /* bytes of UTF-8, each text NUL-terminated */
#define COOP_DRAW_POLY_POINTS 8

/* CoopDrawCmd.kind, and what its v[] holds */
enum {
    COOP_DRAW_RECT = 1,      /* filled: x0 y0 x1 y1 rounding */
    COOP_DRAW_CIRCLE = 2,    /* x y radius thickness (0: filled) */
    COOP_DRAW_TRIANGLE = 3,  /* filled: x0 y0 x1 y1 x2 y2 */
    COOP_DRAW_POLY = 4,      /* convex, n (3..8) points x y ...; id 1: outline only */
    COOP_DRAW_TEXT = 5,      /* x y (its top left) px; the words at text_off */
    COOP_DRAW_IMAGE = 6,     /* the whole image `id` into x0 y0 x1 y1, rgba = tint */
    COOP_DRAW_FONT = 7,      /* texts and measures from here on use font `id` */
    COOP_DRAW_CLIP_PUSH = 8, /* x0 y0 x1 y1: nothing drawn outside until the matching pop */
    COOP_DRAW_CLIP_POP = 9
};

/* Logical fonts (COOP_DRAW_FONT id) */
enum {
    COOP_FONT_UI = 0,       /* the backend's own (Hookshot's ImGui font on Longshot) */
    COOP_FONT_REGULAR = 1,  /* the HUD pack's regular font (the game look) */
    COOP_FONT_MEDIUM = 2,   /* the HUD pack's medium font */
    COOP_FONT_COUNT = 3
};

/* Logical images (COOP_DRAW_IMAGE id) */
enum {
    COOP_IMAGE_HEART_0 = 0, /* the HUD pack's heart pictures: 0 to 4 quarters filled */
    COOP_IMAGE_HEART_4 = 4,
    COOP_IMAGE_COUNT = 5
};

typedef struct CoopDrawCmd {
    uint8_t kind;
    uint8_t n;         /* POLY: points */
    uint16_t id;       /* FONT, IMAGE: the logical id; POLY: outline */
    uint32_t rgba;
    uint32_t text_off; /* TEXT: into CoopDrawList.text */
    uint32_t text_len; /* TEXT: bytes, without the NUL */
    float v[2 * COOP_DRAW_POLY_POINTS];
} CoopDrawCmd;

typedef struct CoopDrawList {
    float width, height;   /* the picture */
    uint32_t n;            /* commands used */
    uint32_t text_used;    /* bytes of text used */
    uint32_t dropped;      /* commands that did not fit (the rest of the frame is lost; a backend may log it) */
    uint16_t font;         /* the font the last COOP_DRAW_FONT chose (COOP_FONT_UI at first) */
    uint16_t clip_depth;
    uint32_t last_text;    /* the last text's offset + 1 (0: none), so a repeated text (outline copies) is stored once */
    CoopDrawCmd cmd[COOP_DRAW_MAX_CMDS];
    char text[COOP_DRAW_MAX_TEXT];
} CoopDrawList;

/* Empties the list for a picture of width x height. */
void coop_draw_begin(CoopDrawList *l, float width, float height);
/* Each appends one command; 0 when it did not fit (counted in `dropped`), else 1. */
int coop_draw_rect(CoopDrawList *l, float x0, float y0, float x1, float y1, uint32_t rgba, float rounding);
int coop_draw_circle(CoopDrawList *l, float x, float y, float radius, uint32_t rgba, float thickness);
int coop_draw_triangle(CoopDrawList *l, float x0, float y0, float x1, float y1, float x2, float y2, uint32_t rgba);
int coop_draw_poly(CoopDrawList *l, const float *xy, int points, uint32_t rgba, int outline);
int coop_draw_text(CoopDrawList *l, float x, float y, float px, uint32_t rgba, const char *utf8);
int coop_draw_image(CoopDrawList *l, int image, float x0, float y0, float x1, float y1, uint32_t tint);
int coop_draw_font(CoopDrawList *l, int font);
int coop_draw_clip_push(CoopDrawList *l, float x0, float y0, float x1, float y1);
int coop_draw_clip_pop(CoopDrawList *l);
/* A TEXT command's words (NUL-terminated); "" for any other command. */
const char *coop_draw_text_of(const CoopDrawList *l, const CoopDrawCmd *c);

/* Text widths as data (no font engine): a font's advances at 1 px, codepoints sorted ascending. A backend without a
   measure of its own (an ExeFS build) fills these from its font atlas; a codepoint not in the table counts `missing`. */
typedef struct CoopFontAdvances {
    uint32_t n;
    const uint32_t *codepoints;
    const float *advances;
    float missing;
} CoopFontAdvances;
/* The width of `utf8` at `px` pixels high: the sum of its codepoints' advances times px. Bad UTF-8 bytes count as
   one missing codepoint each. A null table: 0. */
float coop_text_width(const CoopFontAdvances *f, float px, const char *utf8);

#ifdef __cplusplus
}
#endif
#endif
