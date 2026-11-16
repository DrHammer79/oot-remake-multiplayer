// SPDX-License-Identifier: GPL-3.0-or-later
// The pad hook of the ExeFS module (v1 step 8 (d), docs/exefs.md "Input").
//
// main calls nn::hid::GetNpadStates (five overloads, one per controller style) through its GOT. Five hook entries
// of kind kImportRedirect (hooks.h) point those GOT entries at the module's coop_hook_npad_* (start.s), which calls
// the real function through its slot, then hands the result to pad_hook::Hook (pad_hook.h) before the game sees it.
// A data write per import, no code patch: the game's code pages stay as shipped.
#pragma once

#include <stdint.h>

#include "../proto/hooks.h"
#include "pad_hook.h"

namespace input {

// main's GOT entries for nn::hid::GetNpadStates, build e0f3ccfaf50065ae (1.1.1): main's R_AARCH64_JUMP_SLOT
// relocations by symbol name (_ZN2nn3hid13GetNpadStatesEPNS0_<style>EiRKj). main binds lazily (no BIND_NOW), so
// before the first call an entry points at main's PLT.
constexpr uint64_t kGotJoyDual = 0x4cbf5c0;
constexpr uint64_t kGotFullKey = 0x4cbf5c8;
constexpr uint64_t kGotJoyLeft = 0x4cbf5d0;
constexpr uint64_t kGotJoyRight = 0x4cbf5d8;
constexpr uint64_t kGotHandheld = 0x4cbf5e0;
constexpr uint32_t kHookCount = 5;

// The overloads, in the hook table's order.
enum Style : uint32_t { kFullKey, kHandheld, kJoyDual, kJoyLeft, kJoyRight, kStyles };
const char* StyleName(uint32_t style);

// Emulator / console: every slot gets the module's own import of the same function (rtld linked it from sdk). A
// lifting recomp fills the slots itself and never calls this.
void FillSlots();

// The pad state and its queues (the frame tick drains and logs them; the core will, after step 8 (f1)).
pad_hook::Hook& hook();

// GetNpadStates calls through the module, per style.
uint64_t Calls(uint32_t style);

}  // namespace input

extern "C" {
// The real functions, filled by FillSlots (or a lifter); start.s's coop_call_npad_* go through them.
COOP_EXPORT extern uint64_t coop_slot_npad_fullkey;
COOP_EXPORT extern uint64_t coop_slot_npad_handheld;
COOP_EXPORT extern uint64_t coop_slot_npad_joydual;
COOP_EXPORT extern uint64_t coop_slot_npad_joyleft;
COOP_EXPORT extern uint64_t coop_slot_npad_joyright;
// What main's GOT entries point at (start.s: a direct `b` to coop_npad_*_impl in input.cpp).
COOP_EXPORT void coop_hook_npad_fullkey();
COOP_EXPORT void coop_hook_npad_handheld();
COOP_EXPORT void coop_hook_npad_joydual();
COOP_EXPORT void coop_hook_npad_joyleft();
COOP_EXPORT void coop_hook_npad_joyright();
}

// The five entries of the hook table (switch/proto/main.cpp), in Style order.
#define COOP_INPUT_HOOK_ENTRIES                                                                                \
    {hooks::kImportRedirect, 0, 0, input::kGotFullKey, &coop_hook_npad_fullkey, &coop_slot_npad_fullkey},      \
    {hooks::kImportRedirect, 0, 0, input::kGotHandheld, &coop_hook_npad_handheld, &coop_slot_npad_handheld},   \
    {hooks::kImportRedirect, 0, 0, input::kGotJoyDual, &coop_hook_npad_joydual, &coop_slot_npad_joydual},      \
    {hooks::kImportRedirect, 0, 0, input::kGotJoyLeft, &coop_hook_npad_joyleft, &coop_slot_npad_joyleft},      \
    {hooks::kImportRedirect, 0, 0, input::kGotJoyRight, &coop_hook_npad_joyright, &coop_slot_npad_joyright},
