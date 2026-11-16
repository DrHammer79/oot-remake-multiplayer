// SPDX-License-Identifier: GPL-3.0-or-later
// The pad as co-op UI events (docs/ui.md "Input"): the game's own NpadButton bits and left stick, which Longshot's
// hos_coop_pad and an ExeFS build's nn::hid hook both see, turned into CoopUiEvents with a held direction repeating.
//
// Closed: only the open combination counts (L and R held, then Minus). Open: the D-pad or the left stick moves, A
// confirms, B goes back, L / R change the tab, Minus closes. No allocation, no function pointers, no C++ library (the
// ExeFS module builds it freestanding: switch/adapter/pad_hook.h).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "coop/ui/events.h"

namespace coop::ui {

// nn::hid::NpadButton bits.
namespace npad {
inline constexpr uint64_t kA = 1ull << 0;
inline constexpr uint64_t kB = 1ull << 1;
inline constexpr uint64_t kL = 1ull << 6;
inline constexpr uint64_t kR = 1ull << 7;
inline constexpr uint64_t kPlus = 1ull << 10;
inline constexpr uint64_t kMinus = 1ull << 11;
inline constexpr uint64_t kLeft = 1ull << 12;
inline constexpr uint64_t kUp = 1ull << 13;
inline constexpr uint64_t kRight = 1ull << 14;
inline constexpr uint64_t kDown = 1ull << 15;
inline constexpr uint64_t kStickLLeft = 1ull << 16;
inline constexpr uint64_t kStickLUp = 1ull << 17;
inline constexpr uint64_t kStickLRight = 1ull << 18;
inline constexpr uint64_t kStickLDown = 1ull << 19;
}  // namespace npad

// What one pad frame gave.
struct PadEvents {
    static constexpr size_t kMax = 8;
    CoopUiEvent e[kMax];  // (no zero fill: freestanding, no memset; only the first n are set)
    size_t n = 0;
    void Add(uint8_t kind, bool repeat = false) {
        if (n < kMax) {
            e[n] = CoopUiEvent{};
            e[n].kind = kind;
            e[n].repeat = static_cast<uint8_t>(repeat ? 1 : 0);
            ++n;
        }
    }
};

class PadReader {
public:
    static constexpr uint64_t kRepeatFirstMs = 400;  // a held direction repeats after this...
    static constexpr uint64_t kRepeatMs = 110;       // ...then this often
    static constexpr int32_t kStickDeadZone = 16000;  // of +-32767

    // One frame of the pad: its buttons, the left stick (x right, y up) and the time. `open`: co-op's menu is open.
    PadEvents Feed(uint64_t buttons, int32_t stick_x, int32_t stick_y, uint64_t now_ms, bool open) {
        PadEvents out;
        const uint64_t down = buttons & ~held_;
        held_ = buttons;
        if (!open) {
            dir_ = 0;
            if ((down & npad::kMinus) != 0 && (buttons & npad::kL) != 0 && (buttons & npad::kR) != 0) {
                out.Add(COOP_UI_OPEN);
            }
            return out;
        }
        if ((down & npad::kMinus) != 0) {
            out.Add(COOP_UI_CLOSE);
            return out;
        }
        if ((down & npad::kA) != 0) out.Add(COOP_UI_CONFIRM);
        if ((down & npad::kB) != 0) out.Add(COOP_UI_BACK);
        if ((down & npad::kL) != 0) out.Add(COOP_UI_TAB_PREV);
        if ((down & npad::kR) != 0) out.Add(COOP_UI_TAB_NEXT);
        const uint8_t dir = Direction(buttons, stick_x, stick_y);
        if (dir != dir_) {
            dir_ = dir;
            if (dir != 0) {
                out.Add(dir);
                next_ms_ = now_ms + kRepeatFirstMs;
            }
        } else if (dir != 0 && now_ms >= next_ms_) {
            out.Add(dir, true);
            next_ms_ = now_ms + kRepeatMs;
        }
        return out;
    }

    // The direction held (COOP_UI_UP..RIGHT, 0: none); with two, up/down wins (lists are vertical).
    static uint8_t Direction(uint64_t b, int32_t x, int32_t y) {
        if ((b & (npad::kUp | npad::kStickLUp)) != 0 || y > kStickDeadZone) return COOP_UI_UP;
        if ((b & (npad::kDown | npad::kStickLDown)) != 0 || y < -kStickDeadZone) return COOP_UI_DOWN;
        if ((b & (npad::kLeft | npad::kStickLLeft)) != 0 || x < -kStickDeadZone) return COOP_UI_LEFT;
        if ((b & (npad::kRight | npad::kStickLRight)) != 0 || x > kStickDeadZone) return COOP_UI_RIGHT;
        return 0;
    }

private:
    uint64_t held_ = 0;
    uint8_t dir_ = 0;
    uint64_t next_ms_ = 0;
};

// What the game gets of the pad around co-op's menu: nothing while the menu has it, and afterwards no button that
// was held while it had it (B for Back, Minus from the open combination, A for a confirm that closed it) until that
// button is let go, so none reaches the game as a new press. Buttons pressed after the menu let go pass at once.
class PadGate {
public:
    uint64_t Pass(uint64_t buttons, bool menu_has_pad) {
        if (menu_has_pad) {
            latched_ = buttons;
            return 0;
        }
        latched_ &= buttons;   // a latched button let go is free again
        return buttons & ~latched_;
    }
    uint64_t latched() const { return latched_; }

private:
    uint64_t latched_ = 0;
};

}  // namespace coop::ui
