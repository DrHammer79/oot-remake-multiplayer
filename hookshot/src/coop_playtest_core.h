// SPDX-License-Identifier: GPL-3.0-or-later
// The play-test bridge's game-free part (docs/playtest.md, "Bridge protocol"): the frame hold, the requests'
// checks and the stick's numbers. coop_playtest.cpp puts it in the game (the socket, Longshot's control bridge,
// the module's test commands); the unit tests (tests/hookshot/playtest_test.cpp) drive it with plain threads.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop_mod_json.h"
#include "coop_world_snapshot.h"

namespace CoopPlaytest {

inline constexpr int kProtocol = 1;
inline constexpr std::size_t kMaxLine = 64 * 1024;
inline constexpr std::uint32_t kMaxFrames = 3600;   // step, input
inline constexpr double kDefaultIdleS = 600.0;      // a hold with no command this long ends by itself

// LICHII_COOP_PLAYTEST_PORT: 1024 to 65535; anything else (unset, empty, 0, words) is off.
std::optional<std::uint16_t> PortFrom(std::string_view value);
// LICHII_COOP_PLAYTEST_IDLE: seconds (0 never); unset or bad: the default.
double IdleFrom(std::string_view value);
// A dump's tag: 1 to 64 of letters, digits, '-' and '_' (it names a file in the dump folder: no path).
bool ValidTag(std::string_view tag);

// The game's frame hold. The game's calc thread calls AtFrame at the top of every frame hook; the bridge's thread
// calls the rest. While held, AtFrame waits (the frame's update has not run yet) and runs the jobs the bridge
// hands it there, on the game's thread.
class Gate {
public:
    using Clock = std::chrono::steady_clock;
    explicit Gate(double idle_s = kDefaultIdleS) : idle_s_(idle_s) {}

    // (calc thread) One frame hook: counts the frame, and holds while asked.
    void AtFrame();

    std::uint64_t Frame() const;
    bool Held() const;     // the calc thread waits in AtFrame now
    bool Holding() const;  // a hold is asked (held now, or at the next frame)

    // Hold at the next frame; true once held (false: no frame came in time: a load, the game closed).
    bool Pause(std::chrono::milliseconds timeout);
    // Run in real time again (also: the bridge's client left; the idle time ran out).
    void Resume();
    // Let exactly `frames` frames run, then hold. Holds first when running. `from`: the frame it started at.
    bool Step(std::uint32_t frames, std::chrono::milliseconds timeout, std::uint64_t* from = nullptr);
    // Run `job` on the calc thread inside the hold (holds first). False: not held in time (the job is dropped).
    bool RunHeld(std::function<void()> job, std::chrono::milliseconds timeout);
    // A command came: the idle time starts again.
    void Touch();
    // How the last hold ended by itself, once (for the log): "idle" or empty.
    std::string TakeEndNote();

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::uint64_t frame_ = 0;
    std::uint32_t budget_ = 0;  // frames still to run in a step
    bool want_hold_ = false;
    bool held_ = false;
    std::deque<std::function<void()>*> jobs_;
    Clock::time_point touched_ = Clock::now();
    double idle_s_;
    std::string end_note_;
};

// The stick: angle in degrees (0 up, 90 right, clockwise), strength 0 to 1, as Longshot's axes (-1 to 1, y up).
struct Axes {
    double x = 0;
    double y = 0;
};
Axes StickAxes(double angle_deg, double strength);

// A checked request.
struct Request {
    std::string cmd;
    CoopModJson::Value id;  // (null when none)
    std::uint32_t frames = 1;
    std::vector<std::string> buttons;
    std::optional<Axes> stick, rstick;
    bool exclusive = true;
    std::string scene, entrance, name, item, tag, value;
    std::string lines;  // act: one test-script line (kill, hit, hurt, place, ...); PlayNow checks its kind
    double timeout_s = 60;
};
// The request on one line, or `error` (bad_request's message).
std::optional<Request> ParseRequest(std::string_view line, std::string& error);

// Longshot's sequence request for an input (one step: the buttons and both sticks, `frames` frames, from the next
// controller read), not waited on: the bridge steps the frames itself.
std::string ControlInput(const Request& r, int control_id);

// The reply's first fields: id, ok, frame, paused; `rest` (a JSON object's members, may be empty) after them.
std::string Reply(const CoopModJson::Value& id, std::uint64_t frame, bool paused, const std::string& rest);
std::string ErrorReply(const CoopModJson::Value& id, std::uint64_t frame, bool paused, std::string_view code,
                       std::string_view message);
// A JSON string literal.
std::string Quote(std::string_view text);
// A value as JSON on one line, no spaces (CoopModJson::Write is for files: it indents objects and ends with a
// newline, which would split a reply in two).
std::string Compact(const CoopModJson::Value& value);

// The verbs that change the game (act, flag, warp, give) run only in a kit run (OOT_COOP_KIT_START: the kit's start
// file and the autotest set it) with cheats on (LICHII_COOP_CHEATS). A real host's game connects to its own server at
// 127.0.0.1, so a loopback server is no sign of a test. Empty when allowed, else the reason (the error reply's text).
std::string VerbRefusal(bool kit_run, bool cheats);
// The line the bridge logs at start: "verbs on|off (kit run: yes|no, cheats: on|off)".
std::string VerbGateLine(bool kit_run, bool cheats);
// Whether an environment value counts as on (set, not empty, not "0").
bool EnvIsOn(std::string_view value);

// The `world` member of `state` (docs/playtest.md): the actors near our Link from the object walk's last snapshot.
// "" (no member) when the snapshot switch is off, so the old reply stays byte for byte the same. `now_frame` is the
// bridge's frame count, `now_ms` the steady clock in ms. Returns `,"world":{...}`: frame (the snapshot's), age_ms,
// stale (older than 1.5 s: then `actors` is empty), behind (the game is past the snapshot's frame), cost_us, count,
// total (actors in the snapshot), other_count (of them, kind "other": grass, pots), count (listed), actors
// [{id,addr,name,kind,pos,dist,life,max_life,owner,alive}] nearest first. `id` is the actor's address in decimal and
// `addr` the same in hex, as the LICHII_COOP_DEBUG_OBJECTS log prints it. `full` false (state): at most
// kStateLimit actors of a known kind, the "other" ones only counted; true (dump): all the snapshot's actors.
// An empty `actors` has a `why`: "no co-op session: ..." (no snapshot), "stale: no walk for N ms ..." or "empty: the
// walk ran and saw no actors". No snapshot yet: `frame` null.
std::string WorldMember(bool enabled, const std::optional<CoopWorldSnapshot::Snapshot>& snapshot,
                        std::uint64_t now_frame, std::uint64_t now_ms, bool full = false);

}  // namespace CoopPlaytest
