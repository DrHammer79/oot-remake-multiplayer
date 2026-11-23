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
#include "coop_strike.h"
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

// The commands that change the game and reach the wire: act, warp, flag and give. state, dump, shot, input, step, pause
// and resume are not gated.
bool GatedCommand(std::string_view cmd);
// The gated commands run only when the test runner asked for them (LICHII_COOP_PLAYTEST_VERBS, set by tools/autotest
// next to the bridge port and by nothing a player starts: not play.ps1, start.sh or coop-options.ini) and cheats are on
// (LICHII_COOP_CHEATS). OOT_COOP_KIT_START is no sign of a test: the kit's start files set it for every player's game,
// and so is a loopback server (a real host's game connects to its own server at 127.0.0.1). Empty when allowed, else
// the reason (the error reply's text).
std::string VerbRefusal(bool runner, bool cheats);
// The line the bridge logs at start: "verbs on|off (test runner: yes|no, cheats: on|off)".
std::string VerbGateLine(bool runner, bool cheats);
// Whether an environment value counts as on (set, not empty, not "0").
bool EnvIsOn(std::string_view value);

// Longshot's Link snapshot (the control bridge's `status`, `game`) is taken only in a running Link update while a status
// is asked; a held game keeps the old one. `status` is the whole reply: its `frame` is Longshot's present count now,
// `game.frame` the present count the snapshot was taken at. Old: the snapshot is more than kSnapshotSlack presents behind
// (a guess: a running game takes it within a present or two; not tuned in a game yet). No `game.frame` (no sample was
// ever taken: only the typed event, loading and age) is not old.
inline constexpr double kSnapshotSlack = 4;
bool SnapshotIsOld(const CoopModJson::Value* status);
// Whether a reply has no Link sample at all: `status` is there, and its `game` is null, or has no `frame` (Longshot
// takes a sample only when a status is asked in a running Link update, so a game held since its start has none yet).
// Not the same as old: a sample that never came cannot be told from "behind" by its frame. A reply that is itself
// absent is not missing a sample (there is no reply: `control` says so).
bool SnapshotMissing(const CoopModJson::Value* status);
// The reason the Link members are null, "" when they are not (a fresh sample is in the reply): no reply, no sample
// yet, or an old sample. Shown as `not_ready` in the state, so a tester never reads a silent null.
std::string NotReadyReason(const CoopModJson::Value* status);
// The state's Link members from that reply, each with a leading comma: pos, facing, camera, life, max_life, rupees,
// age, event, loading, prompt, snapshot_present, snapshot_stale and not_ready (null, or the reason the members are null:
// NotReadyReason). An old snapshot shows null in every Link member (a
// walk that trusts a stale pos says "arrived" 11 m away) and `"snapshot_stale":true`; `snapshot_present` keeps its number.
// No reply (`status` null) or no `game`: all null, `snapshot_stale` false (`control` says why).
std::string LinkMembers(const CoopModJson::Value* status);
// Why a `give` is refused now, "" when it may go: a load is running (`game.loading` true). The write would be made, and
// the load would put the save's values back over it. No reply, or no `loading` member, is not a refusal.
std::string GiveRefusal(const CoopModJson::Value* status);

// `warp`: two ways to change scene. While an event runs, the module's own request (`Sequence_ReservedNextBanc`, the test
// script's `warp`; the game takes it only then, docs/scenes.md "Free-play scene change"). In free play, Longshot's control
// bridge `teleport` (the Cheats page's destination list; the game takes it at Link's next update; the way hookshot.py's
// boot-script warps go), which knows only its own list of maps and has no start position (`entrance`).
enum class WarpRoute { refuse, event, teleport };
struct WarpPlan {
    WarpRoute route = WarpRoute::refuse;
    std::string code;  // for `refuse`: the bridge's error code (not_in_play, not_in_event)
    std::string why;   // for `refuse`: the text
};
// Which way, from `status` (a reply with a fresh Link snapshot: an old or missing one cannot say, so it refuses), whether
// the caller named an `entrance`, and the `scene` asked: an event runs -> event; none and no entrance -> teleport; none
// and an entrance -> refuse (the teleport has no start position, the event-only request does). Longshot's teleport picks
// the map for Link's current age, so a `scene` ending in the other age (`_Child`/`_Adult`, any case) is refused
// (`wrong_age`) on that route; the event request sets the age itself.
WarpPlan PlanWarp(const CoopModJson::Value* status, bool entrance_given, std::string_view scene);
// What the teleport route does with Longshot's ok reply (`sent`, `map`: the map it sent, `accepted`: the game's event
// manager's answer): `accepted` false fails now (`refused`) instead of waiting out the timeout; else wait for `map`
// (Longshot's spelling, for Link's age), or for the scene asked when the reply has none.
struct TeleportWait {
    bool failed = false;
    std::string code, why;
    std::string wait_for;
};
TeleportWait ReadTeleport(const CoopModJson::Value& reply, std::string_view scene);
// Whether Link is in `scene` now: `status`'s map is the scene (any case; a `_Child` or `_Adult` ending names the age too,
// the map may not carry it: not seen in a game yet, so either spelling counts) and its fresh snapshot says no event and
// no load.
bool WarpArrived(const CoopModJson::Value* status, std::string_view scene);

// The `world` member of `state` (docs/playtest.md): the actors near our Link from the object walk's last snapshot.
// "" (no member) when the snapshot switch is off, so the old reply stays byte for byte the same. `now_frame` is the
// bridge's frame count, `now_ms` the steady clock in ms. Returns `,"world":{...}`: frame (the snapshot's), age_ms,
// stale (older than 1.5 s and 30 frames behind the game: then `actors` is empty; a held game does not age it), behind (the game is past the snapshot's frame), cost_us, count,
// total (actors in the snapshot), other_count (of them, kind "other": grass, pots), count (listed), actors
// [{id,addr,name,kind,pos,dist,life,max_life,owner,alive}] nearest first. `id` is the actor's address in decimal and
// `addr` the same in hex, as the LICHII_COOP_DEBUG_OBJECTS log prints it. `full` false (state): at most
// kStateLimit actors of a known kind, the "other" ones only counted; true (dump): all the snapshot's actors.
// An empty `actors` has a `why`: "no co-op session: ..." (no snapshot), "stale: no walk for N ms ..." or "empty: the
// walk ran and saw no actors". No snapshot yet: `frame` null.
std::string WorldMember(bool enabled, const std::optional<CoopWorldSnapshot::Snapshot>& snapshot,
                        std::uint64_t now_frame, std::uint64_t now_ms, bool full = false);

// The strike acts (docs/playtest.md): `kill ID NAME` and `hit ID NAME N` name an actor by the id `get_world` lists. The
// id is resolved in the objects walk, which runs only while connected and sharing, so with no usable world list (no
// snapshot, or one older than the stale limit) an id is refused before anything is queued. Empty when the lines have
// no id strike or a list is there; else the reply's text. A line that is not a strike is not looked at (the parser
// checks the rest).
std::string StrikeIdRefusal(std::string_view lines, bool world_list);
// Whether the snapshot is a usable world list: there is one and it is not stale (CoopWorldSnapshot::IsStale: by the
// wall clock and the game's frames together, so a held game keeps its list). `now_frame` is the bridge's frame count.
bool WorldListLive(const std::optional<CoopWorldSnapshot::Snapshot>& snapshot, std::uint64_t now_ms,
                   std::uint64_t now_frame);
// Whether the lines hold a `kill` or `hit` step (with an id or not): the strike queue gives each a number.
bool HasStrikeLine(std::string_view lines);
// The `strike` member of `state`: `,"strike":{seq,result,name,id}` for the newest strike the walk ran, or "" when none
// ran yet (so the reply stays as it was for a game that never struck).
std::string StrikeMember(const std::optional<CoopStrike::Result>& last);

}  // namespace CoopPlaytest
