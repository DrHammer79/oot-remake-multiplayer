/* SPDX-License-Identifier: GPL-3.0-or-later
   CoopUiEvent: one input to co-op's own UI layer (docs/ui.md "Input"), the same on every platform.

   An adapter turns its input into these: the pad (coop/ui/pad.hpp: NpadButton bits, the same on Longshot and an ExeFS
   build), the keyboard (Longshot: hos_coop_key / hos_coop_char), the pointer (Longshot's mouse), and on a console the
   software keyboard's text (TEXT_CHAR for each codepoint, then TEXT_COMMIT). Plain C. */
#ifndef COOP_UI_EVENTS_H
#define COOP_UI_EVENTS_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CoopUiEvent.kind */
enum {
    COOP_UI_NONE = 0,
    COOP_UI_UP = 1,
    COOP_UI_DOWN = 2,
    COOP_UI_LEFT = 3,
    COOP_UI_RIGHT = 4,
    COOP_UI_CONFIRM = 5,
    COOP_UI_BACK = 6,
    COOP_UI_TAB_PREV = 7,
    COOP_UI_TAB_NEXT = 8,
    COOP_UI_OPEN = 9,           /* open co-op's menu (it is closed) */
    COOP_UI_CLOSE = 10,         /* close it */
    COOP_UI_TEXT_CHAR = 11,     /* ch: a typed codepoint */
    COOP_UI_TEXT_BACKSPACE = 12,
    COOP_UI_TEXT_COMMIT = 13,   /* the text is done (Enter; the software keyboard's OK) */
    COOP_UI_POINTER_MOVE = 14,  /* x, y: pixels of the picture */
    COOP_UI_POINTER_CLICK = 15  /* x, y: a primary click there */
};

typedef struct CoopUiEvent {
    uint8_t kind;
    uint8_t repeat;   /* a held direction's repeat (not the first press) */
    uint8_t pad_[2];
    uint32_t ch;      /* TEXT_CHAR */
    float x, y;       /* POINTER_* */
} CoopUiEvent;

#ifdef __cplusplus
}
#endif
#endif
