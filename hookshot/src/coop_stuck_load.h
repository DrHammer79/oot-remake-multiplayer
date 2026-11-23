// SPDX-License-Identifier: GPL-3.0-or-later
// A load that never ends (black-01112, rows 242/256): the game's own scene load (Sequence_IsLoadingBanc, which
// BeforeChangeBanc sets while a scene loads) still on after kStuckMs. Row 242's cause was a mod's model that named
// textures no folder had (Character Select's default part without Link's textures): the game waited for them for
// good, the world stayed black, and nothing told the player or the log why, solo or co-op. Solo and co-op alike
// (every frame of the module): one log line, a record in bug-reports/coop-stuck (CoopProblemAsk::StuckName), so the
// next title asks for /bugreport once, and one chat line in a session. A load that ends after all (a slow PC) takes
// its record back.
//
// Keyed on the game's own load state, never on "no Link" alone: cutscenes without Link (the New Game intro, the Deku
// Tree, the staff roll, the title demo, the collapse, warp cutscenes) are not a scene load, so they never count.
// The time only counts once the title has been up this run (the boot before the first title is the game's own
// start: shader caches made on a fresh install can take minutes there), and the title being up stops it.
// "No Link, not loading, not the title" for kStuckMs is only logged (Unclear), never asked about: it tells a log
// reader whether a black screen had the loading flag on or not.
//
// Logic only (tests/hookshot/stuck_load_test.cpp); the frame's reads are in coop_hook.cpp (StuckLoadFrame).
#pragma once

#include <cstdint>

namespace CoopStuckLoad {

// A scene load this long is a load that does not end (a normal one takes seconds; a slow disk's first load of a big
// scene well under a minute).
inline constexpr std::uint64_t kStuckMs = 120'000;

struct Sample {
    bool title_up = false;   // the title's menu is up (CoopSave::TitleUp)
    bool loading = false;    // Sequence_IsLoadingBanc
    bool link = false;       // our Link was read this frame
};

enum class Step {
    none,
    stuck,     // the load has run kStuckMs: log, record, chat line (once a load)
    ended,     // a load that was marked stuck ended with Link in the world: take its record back
    unclear,   // no Link, no load, no title for kStuckMs: log only (once a stretch)
};

class Watch {
public:
    Step Frame(const Sample& s, std::uint64_t now_ms) {
        if (s.title_up) {
            title_seen_ = true;
            load_since_ = unclear_since_ = 0;
            loading_ = unclear_said_ = false;
            return Step::none;   // (a marked load stays marked: the player quit to the title from a black screen)
        }
        if (!title_seen_) return Step::none;
        if (s.loading) {
            unclear_since_ = 0, unclear_said_ = false;
            if (!loading_) loading_ = true, load_since_ = now_ms;
            if (!marked_ && now_ms - load_since_ >= kStuckMs) {
                marked_ = true;
                return Step::stuck;
            }
            return Step::none;
        }
        loading_ = false;
        if (s.link) {
            unclear_since_ = 0, unclear_said_ = false;
            if (marked_) {
                marked_ = false;
                return Step::ended;
            }
            return Step::none;
        }
        if (unclear_since_ == 0) unclear_since_ = now_ms == 0 ? 1 : now_ms;
        if (!unclear_said_ && now_ms - unclear_since_ >= kStuckMs) {
            unclear_said_ = true;
            return Step::unclear;
        }
        return Step::none;
    }
    // How long the current load has run (0: none), for the log line.
    std::uint64_t LoadMs(std::uint64_t now_ms) const { return loading_ ? now_ms - load_since_ : 0; }
    bool Marked() const { return marked_; }

private:
    bool title_seen_ = false;
    bool loading_ = false;
    bool marked_ = false;
    bool unclear_said_ = false;
    std::uint64_t load_since_ = 0;
    std::uint64_t unclear_since_ = 0;
};

}  // namespace CoopStuckLoad
