// SPDX-License-Identifier: GPL-3.0-or-later
// The pad hook of the ExeFS module (input.h): the five GetNpadStates replacements and their slots.
#include "input.h"

#include <stddef.h>
#include <stdint.h>

// ---- Imports, resolved by rtld from sdk (the module's own; main's go through the hook) ----
namespace nn::hid {
struct NpadFullKeyState;
struct NpadHandheldState;
struct NpadJoyDualState;
struct NpadJoyLeftState;
struct NpadJoyRightState;
int GetNpadStates(NpadFullKeyState* out, int count, const unsigned int& npad_id);    // ...EPNS0_16NpadFullKeyStateEiRKj
int GetNpadStates(NpadHandheldState* out, int count, const unsigned int& npad_id);   // ...EPNS0_17NpadHandheldStateEiRKj
int GetNpadStates(NpadJoyDualState* out, int count, const unsigned int& npad_id);    // ...EPNS0_16NpadJoyDualStateEiRKj
int GetNpadStates(NpadJoyLeftState* out, int count, const unsigned int& npad_id);    // ...EPNS0_16NpadJoyLeftStateEiRKj
int GetNpadStates(NpadJoyRightState* out, int count, const unsigned int& npad_id);   // ...EPNS0_17NpadJoyRightStateEiRKj
}  // namespace nn::hid

extern "C" {
COOP_EXPORT uint64_t coop_slot_npad_fullkey = 0;
COOP_EXPORT uint64_t coop_slot_npad_handheld = 0;
COOP_EXPORT uint64_t coop_slot_npad_joydual = 0;
COOP_EXPORT uint64_t coop_slot_npad_joyleft = 0;
COOP_EXPORT uint64_t coop_slot_npad_joyright = 0;
// start.s: tail jumps through the slots above (the real GetNpadStates).
COOP_EXPORT int coop_call_npad_fullkey(pad_hook::NpadState* out, int count, const uint32_t* npad_id);
COOP_EXPORT int coop_call_npad_handheld(pad_hook::NpadState* out, int count, const uint32_t* npad_id);
COOP_EXPORT int coop_call_npad_joydual(pad_hook::NpadState* out, int count, const uint32_t* npad_id);
COOP_EXPORT int coop_call_npad_joyleft(pad_hook::NpadState* out, int count, const uint32_t* npad_id);
COOP_EXPORT int coop_call_npad_joyright(pad_hook::NpadState* out, int count, const uint32_t* npad_id);
}

namespace {

// The prototype has no menu picture yet (step 8 (e)): the menu closes by itself after 5 s.
constexpr uint64_t kAutoCloseMs = 5000;
constexpr uint64_t kTicksPerMs = 19200;  // the system counter runs at 19.2 MHz

constinit pad_hook::Hook g_hook{kAutoCloseMs};
uint64_t g_calls[input::kStyles] = {0, 0, 0, 0, 0};

uint64_t NowMs() {
    uint64_t ticks;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(ticks));
    return ticks / kTicksPerMs;
}

int After(uint32_t style, pad_hook::NpadState* out, int count, const uint32_t* npad_id, int n) {
    pad_hook::atom::Store<pad_hook::atom::kRelaxed>(g_calls[style], pad_hook::atom::Load<pad_hook::atom::kRelaxed>(g_calls[style]) + 1);
    if (n > count) n = count;
    if (n > 0 && npad_id != nullptr) g_hook.OnStates(out, n, *npad_id, NowMs());
    return n;
}

}  // namespace

// What main gets for GetNpadStates (start.s coop_hook_npad_* branch here): the real result, through the hook.
extern "C" int coop_npad_fullkey_impl(pad_hook::NpadState* out, int count, const uint32_t* npad_id) {
    return After(input::kFullKey, out, count, npad_id, coop_call_npad_fullkey(out, count, npad_id));
}
extern "C" int coop_npad_handheld_impl(pad_hook::NpadState* out, int count, const uint32_t* npad_id) {
    return After(input::kHandheld, out, count, npad_id, coop_call_npad_handheld(out, count, npad_id));
}
extern "C" int coop_npad_joydual_impl(pad_hook::NpadState* out, int count, const uint32_t* npad_id) {
    return After(input::kJoyDual, out, count, npad_id, coop_call_npad_joydual(out, count, npad_id));
}
extern "C" int coop_npad_joyleft_impl(pad_hook::NpadState* out, int count, const uint32_t* npad_id) {
    return After(input::kJoyLeft, out, count, npad_id, coop_call_npad_joyleft(out, count, npad_id));
}
extern "C" int coop_npad_joyright_impl(pad_hook::NpadState* out, int count, const uint32_t* npad_id) {
    return After(input::kJoyRight, out, count, npad_id, coop_call_npad_joyright(out, count, npad_id));
}

namespace input {

const char* StyleName(uint32_t style) {
    switch (style) {
        case kFullKey: return "FullKey";
        case kHandheld: return "Handheld";
        case kJoyDual: return "JoyDual";
        case kJoyLeft: return "JoyLeft";
        case kJoyRight: return "JoyRight";
        default: return "?";
    }
}

template <typename State>
uint64_t Import() {
    int (*fn)(State*, int, const unsigned int&) = &nn::hid::GetNpadStates;
    return reinterpret_cast<uint64_t>(fn);
}

void FillSlots() {
    coop_slot_npad_fullkey = Import<nn::hid::NpadFullKeyState>();
    coop_slot_npad_handheld = Import<nn::hid::NpadHandheldState>();
    coop_slot_npad_joydual = Import<nn::hid::NpadJoyDualState>();
    coop_slot_npad_joyleft = Import<nn::hid::NpadJoyLeftState>();
    coop_slot_npad_joyright = Import<nn::hid::NpadJoyRightState>();
}

pad_hook::Hook& hook() { return g_hook; }

uint64_t Calls(uint32_t style) {  // (the tick reads what the pad's thread counts)
    return style < kStyles ? pad_hook::atom::Load<pad_hook::atom::kRelaxed>(g_calls[style]) : 0;
}

}  // namespace input
