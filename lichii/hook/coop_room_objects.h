// SPDX-License-Identifier: GPL-3.0-or-later
//
// room-objects (issues 196, 249, 194; reports/room-objects-a.md): which moving room objects are driven by their own
// logic, so another game's copy is never written from outside.
//
// The rule: in a game that follows another, a room object moves only through its own logic, the input its own
// switch, sensor or rider would set: CoopEyeSwitch sets the switch's plug (the Deku Tree compass-room lifts' IsOn),
// CoopLiftRide sets RideOnPlayer (Jabu-Jabu's and the Forest Temple's lifts), the push block's own node starts again
// (CoopWorld::FindBlockNode). A word written into such an object from outside fights that logic: the clock path
// (coop_platforms.h) took a frame counter on the Deku lifts (the game put it back, "do not hold") and a guessed float
// on Jabu-Jabu's moving floor. So the clock path leaves these objects alone and is for free-running movers only
// (no wired control input, no rider plug): the Deku Tree's water-room wave, the loops.
//
// The table is generated from the romfs: tools/puppet_pack/room_objects.py. Each placement of a platform name
// (CoopPlatforms::IsDriver) with why it is driven (the wired inputs, RideOnPlayer, or "with <name>": a part placed on
// a driven one), or "" when free. A live actor is the placement of its name nearest to it in its scene; a name or
// scene not in the table is free.
//
// No emulator code (tests/lichii/room_objects_test.cpp).
#pragma once

#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace CoopRoomObjects {

struct Placement {
    std::string_view scene;
    std::string_view name;
    std::array<float, 3> place;
    std::string_view why;  // "" free
};

std::span<const Placement> Placements();

// The placement a live actor `name` at `at` in `scene` is (its name's nearest there), or nullptr.
const Placement* Nearest(std::string_view scene, std::string_view name, const std::array<float, 3>& at);

// Driven by its own logic (its placement's `why` is not empty): never written from outside.
bool Driven(std::string_view scene, std::string_view name, const std::array<float, 3>& at);

// Its logic type has the bool output RideOnPlayer (the generated list): CoopLiftRide drives it.
bool RiderName(std::string_view name);

// The switch output plugs a room object reads (through gates, timers, delays and module inputs) that no CoopEyeSwitch
// line covers: the timed switches. SWITCH_STATE carries their presses (coop_switch_share.h). Generated with
// room_objects.py --eye-switches coop_eye_switch.cpp (its third table).
struct SwitchPlug {
    std::string_view scene;
    std::string_view name;
    std::array<float, 3> place;
    std::string_view plug;
};
std::span<const SwitchPlug> SwitchPlugs();
// The lines of a live switch `name` at `at` in `scene` (its placement within kSwitchMatchM).
inline constexpr float kSwitchMatchM = 1.5f;
std::vector<const SwitchPlug*> SwitchPlugsOf(std::string_view scene, std::string_view name,
                                             const std::array<float, 3>& at);
// A switch name some line has (any scene): the objects walk keeps these actors.
bool SharedSwitchName(std::string_view name);

}  // namespace CoopRoomObjects
