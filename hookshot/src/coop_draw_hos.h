// SPDX-License-Identifier: GPL-3.0-or-later
// The draw list's Longshot backend (docs/ui.md): a CoopDrawList (coop/ui/drawlist.h) drawn through Hookshot's
// HosCoopDraw table, call for call (Measure, its text widths, is in coop_draw.h and lives in coop_draw_hos.cpp).
//
// Replay turns each command into the table call coop_draw.cpp made before the list (the same order and the same
// arguments: tests/hookshot/draw_golden_test.cpp), the logical fonts and images into the game look's ids.
#pragma once
#include "coop/ui/drawlist.h"
#include "coop_draw.h"
#include "hos_coop_draw.h"

namespace CoopDraw {

// The table's id for a logical font (0: Hookshot's own) or image (0: none), from the game look's ids.
int HostFont(const Look* look, int font);
int HostImage(const Look* look, int image);

// Draws `list` through `d`. A call the table lacks (an older Hookshot's: use_font, draw_image) is skipped, as the
// old code skipped it. A POLY is drawn with triangles (a fan; an outline as thin bands); CLIP has no call in this
// table: skipped (nothing emits one yet).
void Replay(const CoopDrawList& list, const HosCoopDraw& d, const Look* look);

}  // namespace CoopDraw
