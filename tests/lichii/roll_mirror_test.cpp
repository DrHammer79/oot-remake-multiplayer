// Tests for the ceiling Skulltula's second 3x3 (lichii/hook/coop_roll_mirror.h).
#include <doctest/doctest.h>

#include <array>

#include "coop_roll_mirror.h"

using CoopRollMirror::IsRotation;
using CoopRollMirror::Mirrors;

TEST_CASE("roll mirror: only the ceiling Skulltula") {
    CHECK(Mirrors("Enemy_Skulltula"));
    CHECK_FALSE(Mirrors("Enemy_Skulltula_Gold"));
    CHECK_FALSE(Mirrors("Enemy_Octarock"));
    CHECK_FALSE(Mirrors(""));
}

TEST_CASE("roll mirror: the second 3x3 is written only while it holds a rotation") {
    // As read in the owner's game during NearRoll_L (yaw 45.8 degrees).
    const std::array<float, 9> yawed{0.697f, 0.0f, 0.717f, 0.0f, 1.0f, 0.0f, -0.717f, 0.0f, 0.697f};
    CHECK(IsRotation(yawed));
    CHECK(IsRotation({1, 0, 0, 0, 1, 0, 0, 0, 1}));
    CHECK_FALSE(IsRotation({0, 0, 0, 0, 0, 0, 0, 0, 0}));        // not set up
    CHECK_FALSE(IsRotation({2, 0, 0, 0, 2, 0, 0, 0, 2}));        // a scale
    CHECK_FALSE(IsRotation({1, 0, 0, 1, 0, 0, 0, 0, 1}));        // rows not square to each other
    CHECK_FALSE(IsRotation({1, 0, 0, 0, 1, 0, 0, 0, 1e30f}));    // other data
}
