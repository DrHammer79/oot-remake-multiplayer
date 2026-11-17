// Tests for the timed switches' presses (lichii/hook/coop_switch_share.cpp, room-objects).
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "coop_room_objects.h"
#include "coop_switch_share.h"

using namespace CoopSwitchShare;
namespace proto = coop::protocol;

namespace {

constexpr const char* kScene = "DodongoCavern";

// Dodongo's Cavern's moving-ground plate: Obj_StepSwitch_A_01 OnState through a timer (GroundControlActor_SimpleMoveTo).
const CoopRoomObjects::SwitchPlug* Plate() {
    const auto lines = CoopRoomObjects::SwitchPlugsOf(kScene, "Obj_StepSwitch_A_01", {17.2f, 112.3f, -23.0f});
    REQUIRE(lines.size() == 1);
    return lines[0];
}

struct Game {
    Sharer sharer;
    std::uint8_t self = 1;
    bool on = false;  // our switch's plug as the game has it
    std::vector<proto::SwitchState> sent;
    std::vector<Hold> holds;
    std::vector<std::string> notes;

    void Tick(std::uint64_t now_ms, bool others = true, const std::string& scene = kScene) {
        std::vector<proto::SwitchState> out;
        holds = sharer.Tick(scene, {Seen{Plate(), 0x1000, on}}, others, now_ms, out, notes);
        for (auto& m : out) {
            m.player_number = self;  // the server's relay writes the sender
            sent.push_back(m);
        }
    }
};

bool Has(const std::vector<std::string>& notes, const char* text) {
    for (const auto& n : notes) {
        if (n.find(text) != std::string::npos) return true;
    }
    return false;
}

}  // namespace

TEST_CASE("switch share: the generated lines are the timed switches no CoopEyeSwitch line covers") {
    CHECK(CoopRoomObjects::SwitchPlugs().size() == 12);
    CHECK(Plate()->plug == "OnState");
    // The Water Temple fountain reads one switch's OffState.
    CHECK(CoopRoomObjects::SwitchPlugsOf("WaterTemple", "Obj_SwitchHit_A_06", {52.1f, 110.0f, -23.2f}).size() == 1);
    // A switch elsewhere, or another scene: no line.
    CHECK(CoopRoomObjects::SwitchPlugsOf(kScene, "Obj_StepSwitch_A_01", {0.0f, 0.0f, 0.0f}).empty());
    CHECK(CoopRoomObjects::SwitchPlugsOf("ForestTemple", "Obj_StepSwitch_A_01", {17.2f, 112.3f, -23.0f}).empty());
    CHECK(CoopRoomObjects::SharedSwitchName("Obj_SwitchHit_A_06"));
    CHECK_FALSE(CoopRoomObjects::SharedSwitchName("Obj_DiveSwitch_A_01"));  // a CoopEyeSwitch line only
}

TEST_CASE("switch share: our press is sent, held on the other game's switch while on, let go when off") {
    Game a;
    Game b;
    b.self = 2;
    std::uint64_t t = 1000;
    a.Tick(t);
    b.Tick(t);
    CHECK(a.sent.empty());
    // A's player steps on the plate.
    a.on = true;
    a.Tick(t += 100);
    REQUIRE(a.sent.size() == 1);
    CHECK(a.sent[0].on);
    CHECK(a.sent[0].name == "Obj_StepSwitch_A_01");
    CHECK(a.sent[0].plug == PlugKey("OnState"));
    CHECK(Has(a.notes, "our player's press: sent"));
    b.sharer.Receive(a.sent[0], t);
    b.Tick(t += 50);
    REQUIRE(b.holds.size() == 1);
    CHECK(b.holds[0] == Hold{0x1000, PlugKey("OnState")});
    CHECK(Has(b.notes, "another player's is on"));
    // Our set made B's plug 1: still held (nothing to set while 1), and never sent back.
    b.on = true;
    for (int i = 0; i < 30; ++i) b.Tick(t += 100);
    CHECK(b.sent.empty());
    // A sends again while on (a later player gets it).
    for (int i = 0; i < 25; ++i) a.Tick(t += 100);
    CHECK(a.sent.size() >= 2);
    // A's plate goes off: off sent, B lets go; B's own 0 comes back and B sends nothing.
    a.on = false;
    a.Tick(t += 100);
    REQUIRE(!a.sent.empty());
    CHECK_FALSE(a.sent.back().on);
    b.sharer.Receive(a.sent.back(), t);
    b.Tick(t += 50);
    CHECK(b.holds.empty());
    CHECK(Has(b.notes, "let go after"));
    CHECK(Has(a.notes, "our player's press ended after"));
    b.on = false;
    for (int i = 0; i < 20; ++i) b.Tick(t += 100);
    CHECK(b.sent.empty());
}

TEST_CASE("switch share: our plug still 1 just after we held it is not a press of ours (echo)") {
    Game b;
    b.self = 2;
    std::uint64_t t = 1000;
    proto::SwitchState on;
    on.player_number = 1;
    on.scene = kScene;
    on.name = "Obj_StepSwitch_A_01";
    on.place = {17.2f, 112.3f, -23.0f};
    on.plug = PlugKey("OnState");
    on.on = true;
    b.sharer.Receive(on, t);
    b.on = true;
    b.Tick(t += 50);
    auto off = on;
    off.on = false;
    b.sharer.Receive(off, t);
    b.Tick(t += 50);  // our plug says 1 for a moment more
    b.Tick(t += 300);
    CHECK(b.sent.empty());
    // Later our own player presses it: that is ours.
    b.on = false;
    b.Tick(t += kEchoMs);
    b.on = true;
    b.Tick(t += 100);
    CHECK(b.sent.size() == 1);
}

TEST_CASE("switch share: solo sends nothing and holds nothing, and a remote on not sent again is let go") {
    Game a;
    std::uint64_t t = 1000;
    a.on = true;
    for (int i = 0; i < 30; ++i) a.Tick(t += 100, false);
    CHECK(a.sent.empty());
    CHECK(a.holds.empty());
    Game b;
    b.self = 2;
    proto::SwitchState on;
    on.player_number = 1;
    on.scene = kScene;
    on.name = "Obj_StepSwitch_A_01";
    on.place = {17.2f, 112.3f, -23.0f};
    on.plug = PlugKey("OnState");
    on.on = true;
    b.sharer.Receive(on, t);
    b.Tick(t += 100);
    CHECK(b.holds.size() == 1);
    b.Tick(t += kRemoteHoldMs + 100);  // the sender left without an off
    CHECK(b.holds.empty());
    // Another plug key or a far place is no line of ours.
    auto other = on;
    other.plug = PlugKey("FirstOn");
    b.sharer.Receive(other, t);
    other = on;
    other.place = {0.0f, 0.0f, 0.0f};
    b.sharer.Receive(other, t);
    b.Tick(t += 100);
    CHECK(b.holds.empty());
}

TEST_CASE("switch share: a player who leaves is let go at once, not after the timeout") {
    Game b;
    b.self = 2;
    std::uint64_t t = 1000;
    proto::SwitchState on;
    on.player_number = 1;
    on.scene = kScene;
    on.name = "Obj_StepSwitch_A_01";
    on.place = {17.2f, 112.3f, -23.0f};
    on.plug = PlugKey("OnState");
    on.on = true;
    b.sharer.Receive(on, t);
    auto three = on;
    three.player_number = 3;
    b.sharer.Receive(three, t);
    b.Tick(t += 100);
    REQUIRE(b.holds.size() == 1);
    b.sharer.Forget(1);
    b.Tick(t += 100);
    CHECK(b.holds.size() == 1);  // player 3's is still on
    b.sharer.Forget(3);
    b.Tick(t += 100);  // well before kRemoteHoldMs
    CHECK(b.holds.empty());
    CHECK(Has(b.notes, "let go after"));
}

TEST_CASE("switch share: a new scene forgets what was held") {
    Game b;
    b.self = 2;
    std::uint64_t t = 1000;
    proto::SwitchState on;
    on.player_number = 1;
    on.scene = kScene;
    on.name = "Obj_StepSwitch_A_01";
    on.place = {17.2f, 112.3f, -23.0f};
    on.plug = PlugKey("OnState");
    on.on = true;
    b.Tick(t);
    b.sharer.Receive(on, t);
    b.Tick(t += 100, true, "KokiriForest");
    b.Tick(t += 100);
    CHECK(b.holds.empty());
}
