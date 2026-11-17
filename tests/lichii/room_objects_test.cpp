// Tests for which room objects their own logic drives (lichii/hook/coop_room_objects.cpp, room-objects).
#include <doctest/doctest.h>

#include <set>
#include <string>

#include "coop_room_objects.h"

using namespace CoopRoomObjects;

TEST_CASE("room objects: the generated table is whole and the issues' objects are in it") {
    const auto all = Placements();
    CHECK(all.size() == 132);
    std::size_t driven = 0;
    std::set<std::string> scenes;
    for (const Placement& p : all) {
        driven += p.why.empty() ? 0 : 1;
        scenes.emplace(p.scene);
    }
    CHECK(driven == 24);
    CHECK(scenes.contains("InsideTheDekuTree"));
    CHECK(scenes.contains("InsideJabuJabuBelly"));
    CHECK(scenes.contains("ForestTemple"));
}

TEST_CASE("room objects: switch-, sensor- and rider-driven objects are driven, free movers are not") {
    // #194 / tracker 90: the Deku Tree compass-room lifts rise on their plate's OnState (IsOn).
    const Placement* lift = Nearest("InsideTheDekuTree", "Obj_InsideTheDekuTree_ElevatorLift_A_01", {-25.4f, 118.0f, -1.6f});
    REQUIRE(lift != nullptr);
    CHECK(lift->why == "IsOn");
    // Its limit part has no logic of its own; it is placed on the lift and moves with it.
    CHECK(Driven("InsideTheDekuTree", "Obj_InsideTheDekuTree_ElevatorLiftLim_A_01", {-25.4f, 114.4f, -1.6f}));
    // #249: Jabu-Jabu's moving floor starts on a sensor and its rider plug.
    const Placement* floor = Nearest("InsideJabuJabuBelly", "Obj_InsideJabu_Jabu_MoveFloor_A_01", {0.3f, 95.0f, -42.6f});
    REQUIRE(floor != nullptr);
    CHECK(floor->why == "IsInPlayer+IsTargetFloor+RideOnPlayer");
    // #196: the water room's wave and its floor run free (their phase is the only difference).
    CHECK_FALSE(Driven("InsideTheDekuTree", "GroundControlActor_WaveLoop", {-26.2f, 82.3f, 38.0f}));
    CHECK_FALSE(Driven("InsideTheDekuTree", "Obj_Deku_MoveFloor_A_01", {-28.0f, 75.4f, 37.0f}));
    // A reset-only input (ForceWarp) does not drive a mover.
    CHECK_FALSE(Driven("InsideTheDekuTree", "GroundControlActor_ResidentMoveTo", {-27.5f, 87.5f, 28.1f}));
    // Not in the table: free.
    CHECK_FALSE(Driven("Kokiri", "Obj_InsideTheDekuTree_ElevatorLift_A_01", {0.0f, 0.0f, 0.0f}));
    CHECK(Nearest("InsideTheDekuTree", "Obj_NotAPlatform", {0.0f, 0.0f, 0.0f}) == nullptr);
}

TEST_CASE("room objects: one name, two placements: the nearest decides") {
    // Dodongo's Cavern: of three MoveFloor_A_01, only the shortcut one has a wired input.
    CHECK(Driven("DodongoCavern", "Obj_DodongoCave_MoveFloor_A_01", {10.5f, 99.0f, -23.0f}));
    CHECK_FALSE(Driven("DodongoCavern", "Obj_DodongoCave_MoveFloor_A_01", {-10.5f, 99.0f, -23.0f}));
}

TEST_CASE("room objects: the rider names are the logic types with RideOnPlayer") {
    CHECK(RiderName("Obj_ForestTemple_Elevator_A_01"));
    CHECK(RiderName("Obj_InsideJabu_Jabu_ElevatorLift_A_01"));
    CHECK(RiderName("Obj_InsideJabu_Jabu_ElevatorLift_A_02"));
    CHECK(RiderName("Obj_InsideJabu_Jabu_MoveFloor_A_01"));
    CHECK_FALSE(RiderName("Obj_InsideTheDekuTree_ElevatorLift_A_01"));  // OnPlayer, no RideOnPlayer
    CHECK_FALSE(RiderName("GroundControlActor_WaveLoop"));
}
