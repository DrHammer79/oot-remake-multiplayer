// The ceiling Skulltula's second 3x3 (Deku Tree; skulltula-roll).
//
// While it rolls (PreRotate, NearRoll_L/R) the owner's game turns it about
// 20 degrees a frame. Lane 2 (NEXT41) read, in the owner's game, two 3x3s that
// change every frame: the rotation at +0x328 and a second one at +0x60c that
// follows it one frame later. A copy took the owner's +0x328 at the same rate
// (the Mover's write), but players still saw it hang still: the second 3x3 is
// not ours to write today. The Mover writes it too, with the same rotation,
// for this actor only and only while it holds a rotation (so another layout
// is never written over). LICHII_COOP_ROLL_MIRROR=0 turns it off.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace CoopRollMirror {

inline constexpr std::uint64_t kSecondRotation = 0x60c;  // 9 x f32, rows of 3

inline bool Mirrors(std::string_view name) {
    return name == "Enemy_Skulltula";
}

// Rows of length 1, square to each other.
inline bool IsRotation(const std::array<float, 9>& m) {
    for (int i = 0; i < 3; ++i) {
        for (int j = i; j < 3; ++j) {
            float dot = 0.0f;
            for (int k = 0; k < 3; ++k) {
                dot += m[3 * i + k] * m[3 * j + k];
            }
            if (!std::isfinite(dot) || std::fabs(dot - (i == j ? 1.0f : 0.0f)) > 0.05f) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace CoopRollMirror
