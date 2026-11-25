// SPDX-License-Identifier: GPL-3.0-or-later
//
// Test only (LICHII_COOP_TEST_LIFE_HOLD; players never set it): our Link's life held full on every object walk, so a
// lane run can stand in an enemy room. `setflag PlayerStatus.Life` cannot do it: it writes the flag, and Link's own
// Life component, which the hearts and his damage use, writes its value back over the flag. The hold writes the
// component, as the heart container's refill does (CoopObjects::AskRefill).
// No emulator or Hookshot code here: standard C++ only (tests/lichii/test_life_hold_test.cpp).
#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

namespace CoopTestLifeHold {

// The env value: unset, empty or "0" is off; "1" holds life at Link's own max; a number from 4 to 80 (quarter
// hearts) also sets his max to it. Anything else is off.
inline std::optional<std::int32_t> Parse(std::string_view text) {
    std::int32_t v = 0;
    const char* end = text.data() + text.size();
    if (text.empty() || std::from_chars(text.data(), end, v).ptr != end) {
        return std::nullopt;
    }
    if (v == 1) {
        return 0;  // (0: his own max)
    }
    if (v >= 4 && v <= 80) {
        return v;
    }
    return std::nullopt;
}

struct Write {
    std::optional<std::int32_t> life;
    std::optional<std::int32_t> max;
};

// What to write into Link's Life component for the hold `hold` (Parse), his life and max now. Nothing when he is
// already there, or when the read failed. A Link at 0 is left (down or out: the game's own way runs).
inline Write Step(std::int32_t hold, std::int32_t life, std::int32_t max) {
    Write w;
    if (life <= 0 || max <= 0) {
        return w;
    }
    const std::int32_t want = hold > 0 ? hold : max;
    if (want != max) {
        w.max = want;
    }
    if (life != want) {
        w.life = want;
    }
    return w;
}

}  // namespace CoopTestLifeHold
