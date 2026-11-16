// The draw-re research build (build.py --draw-re, COOP_DRAW_RE): LOG ONLY, it changes nothing in the game.
// v1 step 8 e2, R1 (coord reports v1-exefs-draw-re.md): the facts static reading could not give.
//   - How the game's texture objects look when they get their descriptor id: tap on the `bl f_71000434b0` at
//     main+0xe43c0 (in f_71000e1ec0): the object's first 256 bytes before, and its id (+104) a frame later.
//   - The set-texture call with live values: taps on the two `bl f_7100e4b660` of the map screen's CustomTex apply
//     (main+0xb2abc8 a loaded texture, main+0xb2aac0 the default one): material, id, texmap index, the entry before.
//   - Which screens exist and in which order: the UI manager's screen list (vtables, layouts, a few words each),
//     logged when it changes; the gfx manager's descriptor table once.
// Every line starts "coop-proto: draw-re: " (tools/switch_mod/run_check.py preset draw-re).
#pragma once
#include <stdint.h>

namespace draw_re {

// main's offsets (build e0f3ccfaf50065ae, 1.1.1), read in reports/v1-exefs-draw-re.md.
constexpr uint64_t kApplySite = 0xb2abc8;         // bl f_7100e4b660 (a loaded CustomTex texture)
constexpr uint64_t kApplyDefaultSite = 0xb2aac0;  // bl f_7100e4b660 (the default texture)
constexpr uint64_t kRegisterSite = 0xe43c0;       // bl f_71000434b0 (register a texture's descriptor)
constexpr uint32_t kApplyInsn = 0x940c82a6;
constexpr uint32_t kApplyDefaultInsn = 0x940c82e8;
constexpr uint32_t kRegisterInsn = 0x97fd7c3c;
constexpr uint64_t kUiManagerPtr = 0x4c7e098;  // **: the UI manager (screens: count +8, array +0x10)
constexpr uint64_t kGfxManagerPtr = 0x4c8cc98;  // **: the gfx manager (texture pool +384, ids: +2544 count, +2658 base)

// Every frame from coop_proto_frame, after main's base is known.
void Frame(uint64_t main, uint64_t frames);

}  // namespace draw_re

// The taps (draw_re.s) and their handler.
extern "C" {
void coop_draw_re_apply_entry();
void coop_draw_re_apply_default_entry();
void coop_draw_re_register_entry();
extern uint64_t coop_slot_draw_re_apply;
extern uint64_t coop_slot_draw_re_apply_default;
extern uint64_t coop_slot_draw_re_register;
// regs: x0..x7, x29, x30, x8, x19, x20, x21, x22 at the game's `bl`.
void coop_draw_re_tap(uint32_t which, const uint64_t* regs);
}

#define COOP_DRAW_RE_HOOK_COUNT 3
#define COOP_DRAW_RE_HOOK_ENTRIES                                                                              \
    {hooks::kCallRedirect, 0, draw_re::kApplyInsn, draw_re::kApplySite, &coop_draw_re_apply_entry,             \
     &coop_slot_draw_re_apply},                                                                                \
    {hooks::kCallRedirect, 0, draw_re::kApplyDefaultInsn, draw_re::kApplyDefaultSite,                          \
     &coop_draw_re_apply_default_entry, &coop_slot_draw_re_apply_default},                                     \
    {hooks::kCallRedirect, 0, draw_re::kRegisterInsn, draw_re::kRegisterSite, &coop_draw_re_register_entry,    \
     &coop_slot_draw_re_register},
