// SPDX-License-Identifier: GPL-3.0-or-later
// The play-test bridge's game-free part (hookshot/src/coop_playtest_core.h): the frame hold driven by a fake frame
// source (a thread calling AtFrame as the game's calc thread does), and every command's checks.
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

#include "coop_playtest_core.h"

using namespace CoopPlaytest;
using namespace std::chrono_literals;

namespace {

// The game's calc thread: a frame hook about every millisecond until stopped.
struct FakeGame {
    Gate& gate;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> updates{0};  // frames whose update ran (past the hook)
    std::thread thread;
    explicit FakeGame(Gate& g) : gate(g) {
        thread = std::thread([this] {
            while (!stop.load()) {
                gate.AtFrame();
                updates.fetch_add(1);
                std::this_thread::sleep_for(1ms);
            }
        });
    }
    ~FakeGame() {
        stop.store(true);
        gate.Resume();
        thread.join();
    }
};

std::optional<Request> Parse(std::string_view line) {
    std::string error;
    return ParseRequest(line, error);
}

std::string ParseError(std::string_view line) {
    std::string error;
    CHECK_FALSE(ParseRequest(line, error));
    return error;
}

}  // namespace

TEST_CASE("playtest: the port and idle switches: off unless a real port is given") {
    CHECK_FALSE(PortFrom(""));
    CHECK_FALSE(PortFrom("0"));
    CHECK_FALSE(PortFrom("80"));
    CHECK_FALSE(PortFrom("65536"));
    CHECK_FALSE(PortFrom("5001x"));
    CHECK_FALSE(PortFrom("-5001"));
    CHECK(PortFrom("5001") == 5001);
    CHECK(PortFrom("5012") == 5012);
    CHECK(IdleFrom("") == kDefaultIdleS);
    CHECK(IdleFrom("abc") == kDefaultIdleS);
    CHECK(IdleFrom("-1") == kDefaultIdleS);
    CHECK(IdleFrom("0") == 0);
    CHECK(IdleFrom("30") == 30);
}

TEST_CASE("playtest: pause holds the game still, resume lets it run") {
    Gate gate(0);
    FakeGame game(gate);
    REQUIRE(gate.Pause(2s));
    CHECK(gate.Held());
    const auto frame = gate.Frame();
    const auto updates = game.updates.load();
    std::this_thread::sleep_for(100ms);
    CHECK(gate.Frame() == frame);          // no frame hook passed
    CHECK(game.updates.load() == updates);  // no update ran
    gate.Resume();
    std::this_thread::sleep_for(50ms);
    CHECK(gate.Frame() > frame);
    CHECK_FALSE(gate.Held());
}

TEST_CASE("playtest: step N runs exactly N frames and holds again, from a hold or from running") {
    Gate gate(0);
    FakeGame game(gate);
    for (const std::uint32_t n : {1u, 2u, 7u, 60u}) {
        std::uint64_t from = 0;
        REQUIRE(gate.Step(n, 2s, &from));
        CHECK(gate.Held());
        CHECK(gate.Frame() - from == n);
        // and nothing slips after the step: still there a while later
        const auto at = gate.Frame();
        const auto updates = game.updates.load();
        std::this_thread::sleep_for(30ms);
        CHECK(gate.Frame() == at);
        CHECK(game.updates.load() == updates);
    }
    gate.Resume();
    std::this_thread::sleep_for(20ms);
    std::uint64_t from = 0;
    REQUIRE(gate.Step(5, 2s, &from));  // (from running: held first, then 5)
    CHECK(gate.Frame() - from == 5);
}

TEST_CASE("playtest: a job runs on the frame thread inside the hold, no frame passing") {
    Gate gate(0);
    FakeGame game(gate);
    REQUIRE(gate.Pause(2s));
    const auto frame = gate.Frame();
    std::thread::id ran_on;
    REQUIRE(gate.RunHeld([&] { ran_on = std::this_thread::get_id(); }, 2s));
    CHECK(ran_on == game.thread.get_id());
    CHECK(gate.Frame() == frame);
    CHECK(gate.Held());
}

TEST_CASE("playtest: the hold ends when the client leaves (Resume) and when the idle time runs out") {
    {
        Gate gate(0);
        FakeGame game(gate);
        REQUIRE(gate.Pause(2s));
        gate.Resume();  // (what the bridge does when its client closes or crashes)
        std::this_thread::sleep_for(30ms);
        CHECK_FALSE(gate.Holding());
        CHECK_FALSE(gate.Held());
    }
    {
        Gate gate(0.1);
        FakeGame game(gate);
        REQUIRE(gate.Pause(2s));
        std::this_thread::sleep_for(400ms);
        CHECK_FALSE(gate.Held());
        CHECK(gate.TakeEndNote() == "idle");
        CHECK(gate.TakeEndNote().empty());
    }
}

TEST_CASE("playtest: no frame source (a load) answers in time instead of hanging") {
    Gate gate(0);
    CHECK_FALSE(gate.Pause(50ms));
    CHECK_FALSE(gate.Step(3, 50ms));
    CHECK_FALSE(gate.RunHeld([] {}, 50ms));
}

TEST_CASE("playtest: the stick's angle and strength as Longshot's axes") {
    auto near = [](Axes a, double x, double y) { return std::fabs(a.x - x) < 1e-6 && std::fabs(a.y - y) < 1e-6; };
    CHECK(near(StickAxes(0, 1), 0, 1));     // up
    CHECK(near(StickAxes(90, 1), 1, 0));    // right
    CHECK(near(StickAxes(180, 0.5), 0, -0.5));
    CHECK(near(StickAxes(-90, 1), -1, 0));
    CHECK(near(StickAxes(45, 2), std::sqrt(0.5), std::sqrt(0.5)));  // strength capped at 1
    CHECK(near(StickAxes(NAN, 1), 0, 0));
}

TEST_CASE("playtest: every command parses, ids come back as given") {
    for (const char* line : {R"({"cmd":"hello"})", R"({"cmd":"state","id":"s1"})", R"({"cmd":"pause"})",
                             R"({"cmd":"resume"})"}) {
        CHECK(Parse(line));
    }
    auto r = Parse(R"({"id":7,"cmd":"step","frames":60})");
    REQUIRE(r);
    CHECK(r->frames == 60);
    CHECK(Reply(r->id, 10, true, "").rfind("{\"id\":7,\"ok\":true,\"frame\":10,\"paused\":true", 0) == 0);
    CHECK(Parse(R"({"cmd":"step"})")->frames == 1);

    r = Parse(R"({"cmd":"input","buttons":["A","ZR"],"stick":[90,1.0],"frames":20})");
    REQUIRE(r);
    CHECK(r->buttons.size() == 2);
    CHECK(r->stick);
    CHECK_FALSE(r->rstick);
    CHECK(r->exclusive);
    const std::string c = ControlInput(*r, 3);
    CHECK(c.find("\"cmd\":\"sequence\"") != std::string::npos);
    CHECK(c.find("\"frames\":20") != std::string::npos);
    CHECK(c.find("\"buttons\":[\"A\",\"ZR\"]") != std::string::npos);
    CHECK(c.find("\"lx\":1.000000,\"ly\":0.000000") != std::string::npos);
    CHECK(c.find("\"wait\":false") != std::string::npos);
    CHECK(c.find("\"exclusive\":true") != std::string::npos);
    // (nothing given: still a step, both sticks centred, so Longshot does not refuse it as empty)
    CHECK(ControlInput(*Parse(R"({"cmd":"input","exclusive":false})"), 4).find("\"lx\":0.000000") != std::string::npos);

    r = Parse(R"({"cmd":"warp","scene":"KokiriForest_Child","entrance":"Kokiri_Start","timeout":30})");
    REQUIRE(r);
    CHECK(r->scene == "KokiriForest_Child");
    CHECK(r->entrance == "Kokiri_Start");
    CHECK(r->timeout_s == 30);

    r = Parse(R"({"cmd":"flag","name":"ForestTemple_Room18_Picture_Shot","value":1})");
    REQUIRE(r);
    CHECK(r->value == "1");
    CHECK(Parse(R"({"cmd":"flag","name":"0x1A2B","value":false})")->value == "0");
    CHECK(Parse(R"({"cmd":"flag","name":"Quest.Step","value":"Done"})")->value == "Done");
    CHECK(Parse(R"({"cmd":"flag","name":"X","value":-3})")->value == "-3");

    CHECK(Parse(R"({"cmd":"give","item":"bombs"})")->item == "bombs");
    CHECK(Parse(R"({"cmd":"dump","tag":"after_chest-2"})")->tag == "after_chest-2");
    CHECK(Parse(R"({"cmd":"shot"})")->tag.empty());
    CHECK(Parse(R"({"cmd":"shot","name":"door_open"})")->tag == "door_open");
}

TEST_CASE("playtest: bad requests are refused with a reason, never acted on") {
    CHECK(ParseError("not json").find("JSON") != std::string::npos);
    CHECK(ParseError("[1,2]").find("object") != std::string::npos);
    CHECK(ParseError(R"({"id":1})").find("cmd") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"format_disk"})").find("cmd") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"step","frames":0})").find("frames") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"step","frames":3601})").find("frames") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"step","frames":1.5})").find("frames") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"step","frames":"10"})").find("frames") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"input","buttons":["A","HOME"]})").find("buttons") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"input","buttons":"A"})").find("buttons") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"input","stick":[90]})").find("stick") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"input","stick":[90,1.5]})").find("stick") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"input","rstick":["up",1]})").find("rstick") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"input","exclusive":1})").find("exclusive") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"warp"})").find("scene") != std::string::npos);
    // (a test command line is built from these: no spaces, ';' or '#' that would make a second step)
    CHECK(ParseError(R"({"cmd":"warp","scene":"A B"})").find("scene") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"warp","scene":"A;give bombs"})").find("scene") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"warp","scene":"A","entrance":"x\ngive"})").find("entrance") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"warp","scene":"A","timeout":0})").find("timeout") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"flag","name":"X"})").find("value") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"flag","name":"X","value":1.5})").find("value") != std::string::npos);
    // (past int32: refused here, never handed to the game's thread; review-01111-b)
    CHECK(ParseError(R"({"cmd":"flag","name":"X","value":2147483648})").find("value") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"flag","name":"X","value":2150000000})").find("value") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"flag","name":"X","value":-2147483649})").find("value") != std::string::npos);
    CHECK(Parse(R"({"cmd":"flag","name":"X","value":2147483647})")->value == "2147483647");
    CHECK(Parse(R"({"cmd":"flag","name":"X","value":-2147483648})")->value == "-2147483648");
    CHECK(ParseError(R"({"cmd":"flag","name":"X Y","value":1})").find("name") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"give"})").find("item") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"give","item":"bombs#x"})").find("item") != std::string::npos);
    // dump and shot name files only inside the bridge's own folder (review-01111-b 1)
    CHECK(ParseError(R"({"cmd":"dump"})").find("tag") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"dump","tag":"../x"})").find("tag") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"dump","tag":"C:\\x"})").find("tag") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"shot","path":"/tmp/x.png"})").find("path") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"shot","name":"../../x"})").find("name") != std::string::npos);
    CHECK_FALSE(ValidTag(std::string(65, 'a')));
    CHECK(ParseError(std::string(kMaxLine + 1, ' ')).find("64 KB") != std::string::npos);
}

TEST_CASE("playtest: every reply is one line: nested values written compact, strings escaped") {
    // (the JSON text has the escape \n inside a string: raw strings keep the backslash)
    const auto v = CoopModJson::Parse(R"({"a":[1, 2.5, {"b":"x\ny"}], "c":{"d":true,"e":null}})");
    REQUIRE(v);
    const std::string c = Compact(*v);
    CHECK(c == R"({"a":[1,2.5,{"b":"x\ny"}],"c":{"d":true,"e":null}})");
    CHECK(c.find('\n') == std::string::npos);
    CHECK(Quote("two\nlines") == R"("two\nlines")");
    const std::string r = Reply(*v, 3, false, "\"state\":" + c);
    CHECK(r.find('\n') == std::string::npos);
    CHECK(CoopModJson::Parse(r));
    CHECK(ErrorReply(*v, 3, true, "failed", "a\nb").find('\n') == std::string::npos);
    CHECK(Reply(CoopModJson::Value::Of(7.0), 10, true, "") == R"({"id":7,"ok":true,"frame":10,"paused":true})");
}

TEST_CASE("playtest: an error reply carries the frame, the hold and a reason") {
    const std::string e = ErrorReply(CoopModJson::Value{}, 42, false, "busy", "another client \"x\"");
    CHECK(e.find("\"id\":null") != std::string::npos);
    CHECK(e.find("\"ok\":false") != std::string::npos);
    CHECK(e.find("\"frame\":42") != std::string::npos);
    CHECK(e.find("\"paused\":false") != std::string::npos);
    CHECK(e.find("\"error\":\"busy\"") != std::string::npos);
    CHECK(CoopModJson::Parse(e));  // valid JSON, the quote escaped
}

TEST_CASE("playtest: the world member is absent when the snapshot switch is off") {
    CHECK(WorldMember(false, std::nullopt, 10, 1000).empty());
    CoopWorldSnapshot::Snapshot s;
    s.frame = 10;
    s.taken_ms = 1000;
    CHECK(WorldMember(false, s, 10, 1000).empty());  // off: nothing, so the old `state` text is unchanged
}

TEST_CASE("playtest: the world member with no snapshot yet is an empty list with a null frame") {
    const std::string m = WorldMember(true, std::nullopt, 10, 1000);
    const auto v = CoopModJson::Parse("{" + m.substr(1) + "}");
    REQUIRE(v);
    const auto* w = v->Get("world");
    REQUIRE(w);
    CHECK(w->Get("frame")->kind == CoopModJson::Value::Kind::Null);
    CHECK(w->Get("actors")->array.empty());
    // solo: the reason says so, so a tester does not read "nothing here"
    CHECK(Compact(*w->Get("why")).find("no co-op session") != std::string::npos);
}

TEST_CASE("playtest: the world member lists actors with owner, alive and both frames") {
    CoopWorldSnapshot::Snapshot s;
    s.frame = 8;
    s.taken_ms = 1000;
    CoopWorldSnapshot::WorldActor foe;
    foe.id = 4660;
    foe.name = "Enemy_Test";
    foe.kind = CoopWorldSnapshot::Kind::enemy;
    foe.pos = {1.5f, 2.0f, -3.25f};
    foe.dist = 4.0f;
    foe.life = 2;
    foe.max_life = 4;
    foe.owner = 0;
    foe.alive = true;
    CoopWorldSnapshot::WorldActor grass;
    grass.name = "Obj_Plant_Grass_A";
    s.actors = {foe, grass};
    const std::string m = WorldMember(true, s, 10, 1200);
    CHECK(m.find('\n') == std::string::npos);
    const auto v = CoopModJson::Parse("{" + m.substr(1) + "}");
    REQUIRE(v);
    const auto* w = v->Get("world");
    REQUIRE(w);
    CHECK(w->Get("frame")->number == 8);
    CHECK(w->Get("age_ms")->number == 200);
    CHECK(Compact(*w->Get("stale")) == "false");
    CHECK(Compact(*w->Get("behind")) == "true");  // the game is at 10, the snapshot at 8
    CHECK(w->Get("count")->number == 1);  // `state`: the known kinds only, grass is counted
    CHECK(w->Get("total")->number == 2);
    CHECK(w->Get("other_count")->number == 1);
    CHECK(w->Get("why") == nullptr);  // not empty: no reason
    const auto& a0 = w->Get("actors")->array[0];
    CHECK(a0.Get("id")->number == 4660);
    CHECK(Compact(*a0.Get("addr")) == "\"0x1234\"");  // the id in hex, as the debug log prints addresses
    CHECK(Compact(*a0.Get("owner")) == "\"server\"");
    CHECK(Compact(*a0.Get("kind")) == "\"enemy\"");
    CHECK(Compact(*a0.Get("alive")) == "true");
    CHECK(a0.Get("pos")->array.size() == 3);
    CHECK(a0.Get("pos")->array[2].number == doctest::Approx(-3.25).epsilon(0.05));
    const std::string whole = WorldMember(true, s, 10, 1200, true);  // `dump`: the grass too
    const auto fv = CoopModJson::Parse("{" + whole.substr(1) + "}");
    REQUIRE(fv);
    const auto* fw = fv->Get("world");
    REQUIRE(fw);
    CHECK(fw->Get("count")->number == 2);
    const auto& a1 = fw->Get("actors")->array[1];
    CHECK(Compact(*a1.Get("owner")) == "null");  // unknown owner: null
    CHECK(Compact(*a1.Get("alive")) == "null");  // no life read: unknown, not dead
    CHECK(Compact(*a1.Get("life")) == "null");
}

TEST_CASE("playtest: a world snapshot older than 1.5 s is an empty list marked stale") {
    CoopWorldSnapshot::Snapshot s;
    s.frame = 8;
    s.taken_ms = 1000;
    s.actors.resize(3);
    const std::string m = WorldMember(true, s, 8, 1000 + CoopWorldSnapshot::kStaleMs + 1);
    const auto v = CoopModJson::Parse("{" + m.substr(1) + "}");
    REQUIRE(v);
    const auto* w = v->Get("world");
    REQUIRE(w);
    CHECK(Compact(*w->Get("stale")) == "true");
    CHECK(w->Get("count")->number == 0);
    CHECK(w->Get("actors")->array.empty());
    CHECK(w->Get("age_ms")->number == 1501);
    CHECK(Compact(*w->Get("why")).find("stale: no walk for 1501 ms") != std::string::npos);
}

TEST_CASE("playtest: an empty snapshot that is fresh says the walk ran and saw nothing") {
    CoopWorldSnapshot::Snapshot s;
    s.frame = 8;
    s.taken_ms = 1000;
    const std::string m = WorldMember(true, s, 8, 1100);
    const auto v = CoopModJson::Parse("{" + m.substr(1) + "}");
    REQUIRE(v);
    const auto* w = v->Get("world");
    REQUIRE(w);
    CHECK(Compact(*w->Get("stale")) == "false");
    CHECK(w->Get("count")->number == 0);
    CHECK(Compact(*w->Get("why")).find("empty:") != std::string::npos);
}

TEST_CASE("playtest: a room with only other kinds gives state an empty list with a why, and the dump lists them") {
    CoopWorldSnapshot::Snapshot s;
    s.frame = 8;
    s.taken_ms = 1000;
    for (int i = 0; i < 3; ++i) {
        CoopWorldSnapshot::WorldActor a;
        a.id = 1000 + static_cast<std::uint64_t>(i);
        a.name = "Obj_Plant_Grass_A";
        s.actors.push_back(a);
    }
    const auto parse = [&](bool full) {
        const std::string m = WorldMember(true, s, 8, 1000, full);
        return CoopModJson::Parse("{" + m.substr(1) + "}");
    };
    const auto brief = parse(false);
    REQUIRE(brief);
    const auto* w = brief->Get("world");
    CHECK(w->Get("count")->number == 0);
    CHECK(w->Get("other_count")->number == 3);
    CHECK(Compact(*w->Get("why")).find("only other kinds near: dump lists them") != std::string::npos);
    const auto whole = parse(true);
    REQUIRE(whole);
    CHECK(whole->Get("world")->Get("count")->number == 3);
    CHECK(whole->Get("world")->Get("why") == nullptr);
}

TEST_CASE("playtest: state lists at most 40 known-kind actors and counts the rest, the dump lists them all") {
    CoopWorldSnapshot::Snapshot s;
    s.frame = 8;
    s.taken_ms = 1000;
    for (int i = 0; i < 60; ++i) {  // 60 enemies then 100 grass (the snapshot is sorted known kinds first)
        CoopWorldSnapshot::WorldActor a;
        a.id = 100 + static_cast<std::uint64_t>(i);
        a.name = "Enemy_Test";
        a.kind = CoopWorldSnapshot::Kind::enemy;
        s.actors.push_back(a);
    }
    for (int i = 0; i < 100; ++i) {
        CoopWorldSnapshot::WorldActor a;
        a.id = 1000 + static_cast<std::uint64_t>(i);
        a.name = "Obj_Plant_Grass_A";
        s.actors.push_back(a);
    }
    const auto parse = [&](bool full) {
        const std::string m = WorldMember(true, s, 8, 1000, full);
        return CoopModJson::Parse("{" + m.substr(1) + "}");
    };
    const auto brief = parse(false);
    REQUIRE(brief);
    CHECK(brief->Get("world")->Get("count")->number == 40);
    CHECK(brief->Get("world")->Get("actors")->array.size() == 40);
    CHECK(brief->Get("world")->Get("total")->number == 160);
    CHECK(brief->Get("world")->Get("other_count")->number == 100);
    const auto whole = parse(true);
    REQUIRE(whole);
    CHECK(whole->Get("world")->Get("count")->number == 160);
    CHECK(whole->Get("world")->Get("actors")->array.size() == 160);
}

TEST_CASE("playtest: a held state waits for a newer snapshot only when one is coming") {
    CoopWorldSnapshot::Snapshot s;
    s.frame = 5;
    s.taken_ms = 1000;
    CHECK_FALSE(CoopWorldSnapshot::WorthWaiting(std::nullopt, 10, 1000));  // solo: no walk, no wait
    CHECK(CoopWorldSnapshot::WorthWaiting(s, 10, 1100));                   // fresh and behind: wait
    CHECK_FALSE(CoopWorldSnapshot::WorthWaiting(s, 5, 1100));              // already at the frame
    CHECK_FALSE(CoopWorldSnapshot::WorthWaiting(s, 10, 1000 + CoopWorldSnapshot::kStaleMs + 1));  // stale: no wait
}

TEST_CASE("playtest: act takes one plain test line") {
    const auto r = Parse(R"({"cmd":"act","lines":"kill Enemy_Test"})");
    REQUIRE(r);
    CHECK(r->cmd == "act");
    CHECK(r->lines == "kill Enemy_Test");
    CHECK(ParseError(R"({"cmd":"act"})").find("lines") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"act","lines":""})").find("lines") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"act","lines":"kill A\ngive bombs"})").find("lines") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"act","lines":"kill A;give bombs"})").find("lines") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"act","lines":"kill A # x"})").find("lines") != std::string::npos);
    CHECK(ParseError(R"({"cmd":"nope"})").find("act") != std::string::npos);  // the command list names act
}

TEST_CASE("playtest: act, flag, warp and give run only when the test runner asked and cheats are on") {
    // (the kit's start files set OOT_COOP_KIT_START for every player, and a real host's game connects to its own
    // server at 127.0.0.1: neither makes a test. Only the runner's own variable does.)
    CHECK(VerbRefusal(true, true).empty());
    const std::string no_runner = VerbRefusal(false, true);  // a player's game with cheats on: refused
    CHECK(no_runner.find("LICHII_COOP_PLAYTEST_VERBS") != std::string::npos);
    CHECK(no_runner.find("OOT_COOP_KIT_START") == std::string::npos);
    CHECK(VerbRefusal(false, false).find("LICHII_COOP_PLAYTEST_VERBS") != std::string::npos);
    const std::string no_cheats = VerbRefusal(true, false);  // the runner, cheats off: refused
    CHECK(no_cheats.find("LICHII_COOP_CHEATS") != std::string::npos);
}

TEST_CASE("playtest: exactly act, warp, flag and give are gated") {
    for (const char* cmd : {"act", "warp", "flag", "give"}) CHECK(GatedCommand(cmd));
    for (const char* cmd : {"state", "dump", "shot", "input", "step", "pause", "resume", "hello", "nope", ""}) {
        CHECK_FALSE(GatedCommand(cmd));
    }
}

TEST_CASE("playtest: the gate line at start says what is on and why") {
    CHECK(VerbGateLine(true, true) == "verbs on (test runner: yes, cheats: on)");
    CHECK(VerbGateLine(true, false) == "verbs off (test runner: yes, cheats: off)");
    CHECK(VerbGateLine(false, true) == "verbs off (test runner: no, cheats: on)");
    CHECK(VerbGateLine(false, false) == "verbs off (test runner: no, cheats: off)");
}

TEST_CASE("playtest: an environment value is on when set, not empty and not 0") {
    CHECK(EnvIsOn("1"));
    CHECK(EnvIsOn("yes"));
    CHECK_FALSE(EnvIsOn(""));
    CHECK_FALSE(EnvIsOn("0"));
}
