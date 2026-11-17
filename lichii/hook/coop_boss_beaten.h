// SPDX-License-Identifier: GPL-3.0-or-later
// A dungeon boss beaten by the group, given to a game that did not see it die (G2, player report #241; docs/sync-coverage.md
// D9, docs/dungeons-boss.md "Beaten by the group").
//
// D9 keeps a boss's end state each game's own: every game in the boss's area runs the boss's own death setting
// itself (coop_boss_scenes.h), which sets that game's flags, quest step and warp exactly as a solo kill does. A game
// that did not (its player was elsewhere in the world or in the dungeon, or it is an old build) still holds the boss
// alive while the shared heart container is taken, and a reload brings the fight back with no container to win.
//
// So the group's boss death, only a proven one (ProvenDeath: never BOSS_ROOM `defeated`, which a game also sends on a
// guess), is kept in Record() (the server keeps it for players who join later and forgets it when the host loads a
// save), and Apply gives a game
// whose own flags lack it the boss's whole beaten set: what that boss's death setting writes in the dungeon, read from
// the game's romfs (tools/puppet_pack/boss_beaten.py, coop_boss_beaten.inc). One way only: each Bool is written to
// its beaten value when it is not there, and the dungeon quest is raised to the step the death moves it to only from
// a step before it (a finished dungeon, Complete, is never stepped back). Never in the boss's own scene (its room's
// logic is loaded there, maybe a fight): only while our Link stands in another scene, so the dungeon loads next as a
// beaten save's: no boss, the blue warp there. Pure: the hook reads and writes the flags.
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "coop/protocol/messages.hpp"

namespace CoopBossBeaten {

struct BeatenBool {
    std::uint32_t hash;  // flag_hash of the name
    bool value;          // what the boss's death writes
    const char* name;
};

struct BeatenSet {
    std::uint8_t room;  // CoopBoss::Rooms() index (BOSS_ROOM's room)
    const char* boss;
    std::uint32_t quest;  // the dungeon quest's Enum (GameDataList)
    const char* quest_name;
    std::uint32_t step;  // flag_hash of the step the death moves the quest to
    const char* step_name;
    std::span<const std::uint32_t> below;  // the quest's steps before it: NotReady up to the boss step
    std::span<const BeatenBool> bools;
};

// The eight dungeon bosses' sets (Ganondorf and Ganon have no room), by room.
std::span<const BeatenSet> Sets();
const BeatenSet* ForRoom(std::uint8_t room);

// The room whose boss `m` proves dead, or nothing. The same rule as the server's (server/session.cpp):
// a BOSS_SCENE outro_start or outro_end: a game took the boss's death event itself (its own death setting or
// Dm_<Dungeon>_BossOut, by name; a copy's game only once its owner died) and, for outro_end, that game's death event
// ended (only the game that started it sends it). In that boss's own dungeon scene (not the boss rush). Its boss index
// 0 to 7 is the room (coop_boss_scenes.cpp kBosses). The server keeps the outro_starts and tells a joiner each as an
// outro_end from the same player (its cutscene rules do nothing with an end of a death they never saw).
// BOSS_ROOM defeated is never one: it only says that player's fight is over (maybe a guess: a long event, no
// reading). Our own game's proof is also Sync's beaten_here (the walk read the boss gone).
std::optional<std::uint8_t> ProvenDeath(const coop::protocol::Message& m);

// The rooms whose boss the group beat this session. BOSS_ROOM messages come on the network thread; the hook's frame
// reads. Cleared when the host loads a save and when we leave the session (the server tells a joiner again).
class Record {
public:
    // True when `room` is new (a boss room of the table).
    bool Add(std::uint8_t room);
    void Clear();
    std::vector<std::uint8_t> Rooms() const;

private:
    mutable std::mutex mutex_;
    std::vector<std::uint8_t> rooms_;
};
Record& TheRecord();

// One write to our game: a Bool (`is_enum` false, `value` 0 or 1) or the quest's Enum (`value` a step's hash).
struct Write {
    std::uint8_t room;
    std::uint32_t hash;
    bool is_enum;
    std::uint32_t value;
    const char* name;
};

class Apply {
public:
    // At each hook frame: `scene` nothing while loading; `place_known` once Link's position reads. Only at a scene
    // load (boss-away-crash, review of c78d17c4): the rooms `beaten` holds when a scene is first seen are the ones
    // given in it, each once, once Link's place reads, in a scene that is not the boss's. A boss beaten while we stand
    // in a scene waits for the next load: some of the set is read live outside the dungeon (Gohma_DefeatBoss by
    // Kokiri Forest's Npc_Kokiri_011, HavePlayedEvent.Dm_DodongoCave_BossOut by Death Mountain Trail's
    // DeathMountainTrail_Child_Prefab_6f34, SageOfForest_IsBefore_Step08 by Kokiri Forest's Level_All_01 and
    // Level_Area_01), and in the game alone those never change while such a scene is loaded: a kill is always in
    // another scene. `read(hash)` is the flag's value now (nothing: unread, left alone). Returns what to write.
    std::vector<Write> Check(const std::optional<std::string>& scene, bool place_known,
                             std::span<const std::uint8_t> beaten,
                             const std::function<std::optional<std::uint32_t>(std::uint32_t)>& read);
    void Reset();

private:
    std::optional<std::string> scene_;
    std::vector<std::uint8_t> loaded_;   // the record's rooms when this scene was first seen: the only ones given here
    std::vector<std::uint8_t> checked_;  // rooms done since this scene load
};

} // namespace CoopBossBeaten
