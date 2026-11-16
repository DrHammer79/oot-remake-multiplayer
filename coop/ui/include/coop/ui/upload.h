/* SPDX-License-Identifier: GPL-3.0-or-later
   Getting the raster's picture (raster.h) into the game's textures, cheaply (docs/ui.md, "ExeFS picture pane").

   - coop_draw_list_hash: a frame whose list is the same as the last one needs no raster and no upload.
   - CoopUpload: which picture rows each texture still lacks. The picture is shown through N textures in turn (double
     buffering: the GPU may still read the one shown last), so a changed row is pending for EVERY texture until each has
     had it. A texture starts with nothing: all its rows are pending (the first frame uploads the whole picture).
     coop_upload_take hands out a texture's pending rows as bands (close bands merged, at most a number of rows per
     frame); the rest stays pending for the next frame. A texture is shown only when it is complete.

   Plain C, no allocation (the caller gives the row flags), no function pointers: it runs inside the ExeFS payload. */
#ifndef COOP_UI_UPLOAD_H
#define COOP_UI_UPLOAD_H
#include <stddef.h>
#include <stdint.h>

#include "coop/ui/drawlist.h"
#include "coop/ui/raster.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 64-bit FNV-1a of what the list draws: its size, the commands used and the text used. Same list, same hash. */
uint64_t coop_draw_list_hash(const CoopDrawList *l);

#define COOP_UPLOAD_MAX_BUFFERS 8

typedef struct CoopUpload {
    int32_t height;
    int32_t buffers; /* textures, 1..COOP_UPLOAD_MAX_BUFFERS */
    int32_t gap;     /* bands closer than this many rows are sent as one */
    uint8_t *rows;   /* height bytes (the caller's): bit b set = texture b still needs this row */
} CoopUpload;

/* Sets up for `buffers` textures of `height` rows; every row pending for every texture. 0 on bad arguments. */
int coop_upload_init(CoopUpload *u, int height, int buffers, int gap, uint8_t *rows);
/* Picture rows that changed (coop_raster_dirty's bands): pending again for every texture. */
void coop_upload_mark(CoopUpload *u, const CoopRasterBand *bands, int n);
/* The next rows texture `buffer` needs, as bands top to bottom: at most `max_rows` rows in all (gap rows included) and
   `max_bands` bands. Those rows stop being pending for it. Returns the band count; *complete = 1 when nothing is left
   pending for that texture. */
int coop_upload_take(CoopUpload *u, int buffer, int max_rows, CoopRasterBand *out, int max_bands, int *complete);
/* Rows still pending for a texture. */
int coop_upload_pending(const CoopUpload *u, int buffer);

#ifdef __cplusplus
}
#endif
#endif
