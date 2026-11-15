// SPDX-License-Identifier: GPL-3.0-or-later
//
// (Navi) New Game+: beat Ganon, then play again with your hearts and tougher
// enemies. Navi's addition to oot-remake-multiplayer (UPSTREAM.txt).
//
// - Unlocked by Ganon's death: the game's own event request for it (taken),
//   Ganon's death setting or flow (coop_boss_scenes.cpp's table) or the
//   ending's Defeat_Ganon, seen by Hookshot's event request wrap
//   (CoopLiveEvents::TheGameRequests). Beating him at level L unlocks L + 1
//   (up to protocol kMaxNgPlus) and keeps the most hearts reached (MaxLife).
// - The level is chosen on the Multiplayer tab's Settings page (New Game+:
//   Off, NG+ 1 ... the highest unlocked) and kept in coop-ngplus.ini beside
//   coop.ini, with what was unlocked and the hearts.
// - With a level on: a NEW file (its opening, IsNewFileOpening) of the host
//   or of a game alone gets the kept hearts, through the game's own MaxLife
//   setter (never lowered; a loaded save and a guest's game get nothing of
//   their own: a guest has the host's save), and the difficulty is
//   ngplus_difficulty (coop/protocol/difficulty.hpp) of what is in force:
//   alone in CoopDifficulty::Now(), in a session by the host's server (the
//   host's level counts for everyone; a guest's own level is not used there,
//   the host's hearts reach the guests by the shared MaxLife).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace CoopHook {
class EmulatorMemory;
}

namespace CoopNgPlus {

inline constexpr std::int32_t kMostQuarters = 80;  // 20 hearts, the most kept

// What coop-ngplus.ini keeps.
struct Saved {
    std::uint8_t level = 0;     // chosen: 0 off
    std::uint8_t unlocked = 0;  // the highest level that may be chosen
    std::int32_t hearts = 0;    // quarter hearts carried into New Game+ (0: none)
    bool operator==(const Saved&) const = default;
};
Saved Parse(std::string_view text);
std::string Format(const Saved& s);
// Ganon beaten with `max_life` (quarter hearts; 0 or less: not known).
Saved Beaten(Saved s, std::int32_t max_life);
// A level within what is unlocked.
Saved Choose(Saved s, int level);
// An event the game asked for that is Ganon's death.
bool IsGanonDeath(std::string_view event);
// An event the game asked for that is the opening of a new file.
bool IsNewFileOpening(std::string_view event);

// What decides the kept hearts for the loaded file.
struct Give {
    std::uint8_t level = 0;                // the level chosen (0 off)
    std::int32_t hearts = 0;               // kept (quarter hearts)
    std::optional<std::int32_t> max_life;  // the file's MaxLife now (nothing: unread)
    bool new_file = false;                 // the file began with the opening since the title
    bool may_give = false;                 // alone, or this game hosts (a guest plays the host's save)
};
// The MaxLife to set, or nothing (off, a loaded save, a guest, already that high: never lowered).
std::optional<std::int32_t> HeartsToGive(const Give& g);

// Any thread.
std::uint8_t Level();
std::uint8_t Unlocked();
std::int32_t Hearts();
// The Settings page's choice: kept within what is unlocked, saved. False when nothing changed.
bool SetLevel(int level);

// The hook's thread, each frame (alone and in a session): Ganon's death, and the hearts while a level is on.
// `in_game`: our Link is in a loaded save. `may_give`: alone, or this game hosts the session.
void Frame(CoopHook::EmulatorMemory& memory, bool in_game, bool may_give);

// Chat lines ("New Game+ 1 unlocked ...").
std::vector<std::string> TakeLines();

}  // namespace CoopNgPlus
