// SPDX-License-Identifier: GPL-3.0-or-later
// The pad in the game's process (v1 step 8 (d), docs/exefs.md "Input"): what the module does with every
// nn::hid::GetNpadStates result the game asks for (switch/adapter/input.cpp redirects main's five imports here).
//
// For each result it
//   - keeps players 1-4's latest pad (buttons, sticks) as the game read it, and queues every button edge;
//   - feeds player 1's pad to co-op's UI reader (coop/ui/pad.hpp): CoopUiEvents into a queue, L+R then Minus opens
//     co-op's menu, Minus closes it;
//   - while the menu is open, hands the game player 1's states with no buttons and centred sticks, and afterwards
//     no button held while it was open until that button is let go (coop::ui::PadGate).
// Pure, no libc and no C++ library: the module builds it freestanding, tests/switch/pad_hook_test.cpp on the host.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "coop/ui/events.h"
#include "coop/ui/pad.hpp"

namespace pad_hook {

// The layout every GetNpadStates overload fills (FullKey, Handheld, JoyDual, JoyLeft, JoyRight: NpadCommonState).
struct NpadState {
    int64_t sampling;
    uint64_t buttons;  // nn::hid::NpadButton bits (coop::ui::npad)
    int32_t stick[4];  // left x, y, right x, y (+-32767, y up)
    uint32_t attributes;
    uint32_t reserved;
};
static_assert(sizeof(NpadState) == 0x28, "NpadCommonState is 0x28 bytes");

constexpr uint32_t kAttrConnected = 1;
constexpr int kPlayers = 4;
constexpr uint32_t kNpadHandheld = 0x20;

// Npad id -> player 0..3 (No.1-No.4; Handheld is player 1), or -1 for one co-op does not read.
constexpr int PlayerOf(uint32_t npad_id) {
    if (npad_id < static_cast<uint32_t>(kPlayers)) return static_cast<int>(npad_id);
    return npad_id == kNpadHandheld ? 0 : -1;
}

struct Pad {
    uint64_t buttons = 0;
    int32_t stick[4] = {0, 0, 0, 0};
    uint64_t reads = 0;  // connected results seen (0: never connected)
};

// One button of one player went down or up (before the menu's gate: what the player pressed).
struct Edge {
    uint8_t player;  // 0..3
    uint8_t bit;     // NpadButton bit
    uint8_t down;    // 1 down, 0 up
    uint8_t reserved;
};

// A small queue that keeps the oldest items and counts the ones it had no room for.
template <typename T, size_t N>
class Queue {
public:
    void Push(const T& v) {
        if (n_ < N) {
            items_[(head_ + n_) % N] = v;
            ++n_;
        } else {
            ++dropped_;
        }
    }
    size_t Take(T* out, size_t max) {
        size_t k = 0;
        for (; k < max && n_ != 0; ++k) {
            out[k] = items_[head_];
            head_ = (head_ + 1) % N;
            --n_;
        }
        return k;
    }
    size_t size() const { return n_; }
    uint64_t dropped() const { return dropped_; }

private:
    T items_[N] = {};  // (zeros: the module's Hook is constinit, so this lands in .bss with no code)
    size_t head_ = 0;
    size_t n_ = 0;
    uint64_t dropped_ = 0;
};

class Hook {
public:
    // auto_close_ms: the menu closes by itself after this long (0: never). The prototype has no menu picture yet, so
    // it uses 5 s: whoever opens it by accident gets the pad back on their own.
    constexpr explicit Hook(uint64_t auto_close_ms = 0) : auto_close_ms_(auto_close_ms) {}

    // One GetNpadStates result: `count` states of npad `npad_id`, the latest first, as the game will get them.
    void OnStates(NpadState* states, int count, uint32_t npad_id, uint64_t now_ms) {
        const int p = PlayerOf(npad_id);
        if (p < 0 || count <= 0 || states == nullptr) return;
        if ((states[0].attributes & kAttrConnected) == 0) return;  // not this style: its zeros are not the pad
        Record(p, states[0]);
        if (p != 0) return;
        const bool had_pad = open_;
        if (open_ && auto_close_ms_ != 0 && now_ms - opened_ms_ >= auto_close_ms_) {
            Close(true);
        }
        const bool was_open = open_;
        const coop::ui::PadEvents ev =
            reader_.Feed(states[0].buttons, states[0].stick[0], states[0].stick[1], now_ms, was_open);
        for (size_t i = 0; i < ev.n; ++i) {
            if (ev.e[i].kind == COOP_UI_OPEN) {
                open_ = true;
                opened_ms_ = now_ms;
            } else if (ev.e[i].kind == COOP_UI_CLOSE) {
                open_ = false;
            }
            events_.Push(ev.e[i]);
        }
        // The menu has the pad while open, and on the read it opened or closed on (so neither Minus reaches the game).
        const bool menu_has_pad = had_pad || open_;
        states[0].buttons = gate_.Pass(states[0].buttons, menu_has_pad);
        for (int i = 0; i < count; ++i) {
            if (i > 0) {  // older states: the same mask, without moving the gate
                states[i].buttons = menu_has_pad ? 0 : states[i].buttons & ~gate_.latched();
            }
            if (menu_has_pad) {
                for (int32_t& s : states[i].stick) s = 0;
            }
        }
    }

    // Co-op's menu: open (the game gets none of player 1's pad), or closed. The core sets it too (f1).
    bool open() const { return open_; }
    void SetOpen(bool open, uint64_t now_ms) {
        if (open && !open_) opened_ms_ = now_ms;
        open_ = open;
    }

    const Pad& pad(int player) const { return pads_[player]; }
    size_t TakeEdges(Edge* out, size_t max) { return edges_.Take(out, max); }
    size_t TakeEvents(CoopUiEvent* out, size_t max) { return events_.Take(out, max); }
    uint64_t dropped_edges() const { return edges_.dropped(); }
    uint64_t dropped_events() const { return events_.dropped(); }
    // How many times the menu closed by itself (auto_close_ms).
    uint64_t auto_closes() const { return auto_closes_; }

private:
    void Record(int p, const NpadState& s) {
        Pad& pad = pads_[p];
        const uint64_t changed = pad.buttons ^ s.buttons;
        for (uint8_t bit = 0; bit < 64; ++bit) {
            if ((changed >> bit) & 1) {
                Edge e{};
                e.player = static_cast<uint8_t>(p);
                e.bit = bit;
                e.down = static_cast<uint8_t>((s.buttons >> bit) & 1);
                edges_.Push(e);
            }
        }
        pad.buttons = s.buttons;
        for (int i = 0; i < 4; ++i) pad.stick[i] = s.stick[i];
        ++pad.reads;
    }

    void Close(bool by_itself) {
        open_ = false;
        CoopUiEvent e{};
        e.kind = COOP_UI_CLOSE;
        events_.Push(e);
        if (by_itself) ++auto_closes_;
    }

    uint64_t auto_close_ms_;
    bool open_ = false;
    uint64_t opened_ms_ = 0;
    uint64_t auto_closes_ = 0;
    Pad pads_[kPlayers];
    coop::ui::PadReader reader_;
    coop::ui::PadGate gate_;
    Queue<Edge, 64> edges_;
    Queue<CoopUiEvent, 32> events_;
};

}  // namespace pad_hook
