// SPDX-License-Identifier: GPL-3.0-or-later
//
// Down but not out: our downed Link's body, through the game's own systems
// (docs/play-online.md "Down but not out"; coop_dbno.h has the rules).
//
// - Crawl. The player's scheduler AI (Player.scheduler.*.module.ainb) enters a
//   crawlspace by asking its BSA for action verb 234 ("Crawl"); that runs
//   Player.action.crawl (ExecutePlayerCrawl) with its own animation and the
//   PlayerCamera's "Crawl" camera. A downed Link is asked for that verb through
//   the game's own "request action verb" (main+0xc08f0, the wrap of
//   hos_coop_bosses.h), with the scheduler's own asking node. Once the game has
//   taken it, the other actions the game asks for our Link are skipped at that
//   wrap (a hit's reaction no longer stands it up, row 34), except the ones the
//   world forces (kPassVerbs: falling, landing, sliding, swimming, diving, the
//   void, an event, the game's death): those run, and the crawl is asked again
//   once our Link is back on the ground (its next Walk or Wait).
//   The game takes ONE request per slot a frame: its accept path sets bit 1
//   of the slot record's byte +0x7c, the BSA update (main+0xcb9c0) clears it,
//   and a request meanwhile gets 3. The scheduler asks our Link's slot nearly
//   every frame before our caller runs, so while a crawl round is on (lying,
//   no event, not in the air, not really refused, asks left) the game's own
//   requests (except kPassVerbs) are skipped too, leaving the slot free for
//   our Crawl; a 3 is asked again next frame. A real refusal (any other
//   result) skips nothing; the next round starts on the ground after the
//   world moved our Link, or kRetryMs after the last ask.
//   Up again: the game's "Wait" (40) is asked once.
//   The verb values are read from the scheduler modules' selector cases.
// - Shield. The game's own named invincibility, as the AI node
//   OneShotLifeSetInvincible gives it (run main+0x8d7520; inputs Bool,
//   InvincibleFlagName, IsNoReaction). IsNoReaction picks the list and so the
//   function: main+0x8d75e0 keeps the no-reaction keys (Life+0x1c60),
//   main+0xb34800 the others (Life+0x1c28); each takes (x0 the Life component,
//   w1 Bool: 1 adds the key, 0 takes it out, x2 -> the name's u32 key). Ours:
//   our own key (kOurKey) in the no-reaction list: a hit takes nothing and
//   plays no reaction. We never touch the other list.
// - Unseen. The game's own perception switch, as the AI node
//   OneShotPerceptionSetEnableOrDisable does it with ApplyToEmitter (run
//   main+0x974a60): main+0x1278e0 (x0 the PerceptionEmitter component
//   [[actor+0x270]+0x328] + 0x20, x1 -> a u32 key, w2 IsEnable) keeps a set of
//   keys that turn the emitter off. Ours goes in while downed, so no enemy or
//   boss in our game perceives our Link (the same perception search picks
//   every enemy's and boss's target) and they go for someone else.
//   Shield and unseen go on while downed and off when up, each only on the
//   component it was put on; the game's own keys are never touched.
// - Heal. The revive's life goes in through the game's own Life step (patch
//   0007) as a negative sum, the game's heal (as LifeWrite::heal for bosses);
//   written directly only when the step has not taken it in kHealWaitUs.
//
// The crawl needs Hookshot's action verb wrap (patch 0013) to learn our
// Link's own node: elsewhere no crawl is asked. Solo play never gets here:
// downed exists only in a session.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "coop_boss_native.h"
#include "coop_puppet.h"

namespace CoopDbno::Body {

namespace addr {
inline constexpr std::uint64_t kComponentBsa = CoopBossNative::addr::kComponentBsa;  // [actor+0x270]+0x188
inline constexpr std::uint64_t kComponentEmitter = 0x328;  // [actor+0x270]+0x328, PerceptionEmitter
inline constexpr std::uint64_t kEmitterKeys = 0x20;        // its set of "off" keys
inline constexpr std::uint64_t kInvincibleNoReaction = 0x8d75e0;  // main+, (life, add, &key): the no-reaction list
inline constexpr std::uint64_t kInvincibleList = 0xb34800;  // main+, the same for the other list (not used: the test's check)
inline constexpr std::uint64_t kEmitterSwitch = 0x1278e0;  // main+, (emitter + 0x20, &key, enable)
// Our key for both (the game keys each by a name's hash; any value no game name has).
inline constexpr std::uint32_t kOurKey = 0xC0DB0D01u;
// Our block in guest memory: the action request, then the key.
inline constexpr std::size_t kBlockSize = CoopBossNative::addr::kRequestSize + 8;
inline constexpr std::size_t kBlockKey = CoopBossNative::addr::kRequestSize;
}  // namespace addr

// Player action verbs (Player.scheduler.*.module.ainb, ExecuteBSASetActionVerb).
namespace verb {
inline constexpr std::int32_t kWalk = 1;
inline constexpr std::int32_t kWaterDive = 18;
inline constexpr std::int32_t kLand = 32;
inline constexpr std::int32_t kFall = 33;
inline constexpr std::int32_t kSlippingDown = 34;
inline constexpr std::int32_t kSlide = 35;
inline constexpr std::int32_t kSlideLand = 36;
inline constexpr std::int32_t kWait = 40;
inline constexpr std::int32_t kSwim = 84;
inline constexpr std::int32_t kPreDie = 141;
inline constexpr std::int32_t kPreAbyss = 156;
inline constexpr std::int32_t kPreEvent = 159;
inline constexpr std::int32_t kCrawl = 234;
}  // namespace verb
// What the world forces on a downed Link: never skipped.
inline constexpr std::array<std::int32_t, 10> kPassVerbs{verb::kFall,     verb::kLand,      verb::kSlippingDown,
                                                          verb::kSlide,    verb::kSlideLand, verb::kSwim,
                                                          verb::kWaterDive, verb::kPreAbyss, verb::kPreEvent,
                                                          verb::kPreDie};
constexpr bool Passes(std::int32_t v) {
    for (const std::int32_t p : kPassVerbs) {
        if (p == v) {
            return true;
        }
    }
    return false;
}

// A raise of our Link's life (a revive, a respawn) for the Life step: the
// step takes it as a negative sum; the frame writes it directly when the step
// has not within kHealWaitUs (it skips frames with no life value).
inline constexpr std::uint64_t kHealWaitUs = 250'000;

constexpr std::int32_t Raised(std::int32_t life, std::int32_t sum, std::int32_t to) {
    return to >= 0 && life - sum < to ? life - to : sum;
}

class Raising {
public:
    // The frame: raise to `to` from now (a new ask replaces an old one).
    void Start(std::int32_t to, std::uint64_t now_us) noexcept;
    // The frame, each frame: the life to write directly now (the step did not
    // take it in time), once; nothing while waiting or when done.
    std::optional<std::int32_t> Frame(std::optional<std::int32_t> life, std::uint64_t now_us) noexcept;
    void Drop() noexcept;
    // The Life step (any game thread): the life to raise to, or -1.
    std::int32_t Peek() const noexcept { return to_.load(std::memory_order_acquire); }

private:
    std::atomic<std::int32_t> to_{-1};
    std::uint64_t since_us_ = 0;
};

// Our Link's crawl, shield and unseen. Want() from the frame; Skip() from the
// action verb wrap (a game thread); Next() from the calc-thread caller.
class Crawl {
public:
    struct Wanted {
        bool lying = false;          // downed (or spectating): crawl, shielded, unseen
        bool event = false;          // our Link in an event: the event drives it
        std::uint64_t bsa = 0;       // our Link's BSA component (0: none)
        std::uint64_t life = 0;      // our Link's Life component (0: none)
        std::uint64_t emitter = 0;   // our Link's PerceptionEmitter component (0: none)
    };
    void Want(const Wanted& w);

    // A request the game makes on any BSA; true: skip it (our downed Link in
    // the game's Crawl, any verb but Crawl and kPassVerbs, not our own call).
    // Learns our Link's own asking node.
    bool Skip(std::uint64_t bsa, const CoopBossNative::Request& request, bool own);
    // Whether `bsa` is our Link's (no lock; the wrap reads a request only then).
    bool IsOurs(std::uint64_t bsa) const noexcept {
        return bsa != 0 && bsa == bsa_.load(std::memory_order_acquire);
    }

    // The next game call (CoopCallers::Caller).
    std::optional<CoopPuppet::Call> Next(CoopPuppet::Guest& guest, bool after_call, std::uint64_t result,
                                         std::uint64_t now_ms, std::uint64_t main_base);

    struct Counts {
        std::uint64_t skipped = 0;     // the game's own requests skipped while crawling
        std::uint64_t passed = 0;      // kPassVerbs let through while crawling
        std::uint64_t asked = 0;       // our Crawl / Wait requests made
        std::uint64_t refused = 0;     // ... that the game refused (result not 0)
        std::uint64_t busy = 0;        // ... of those: the slot had taken a request this frame (3)
        std::uint64_t cleared = 0;     // the game's own requests skipped to free the slot for our Crawl
        std::uint64_t shield_on = 0;   // our invincibility put on
        std::uint64_t unseen_on = 0;   // our emitter key put in
        std::vector<std::int32_t> skipped_verbs;  // the first distinct ones (the log)
    };
    Counts Totals() const;
    // Log lines since the last call.
    std::vector<std::string> TakeNotes();

    // Ask again at most this often while lying and not known to crawl.
    static constexpr std::uint64_t kAskEveryMs = 500;
    static constexpr int kMaxAsks = 6;  // then left until our Link's next Walk/Wait (see kRetryMs)
    // A round that ended refused (or out of asks) on the spot starts again on
    // the ground only this long after its last ask (moved by the world: at once).
    static constexpr std::uint64_t kRetryMs = 5000;
    // The request's result when the slot already took one this frame.
    static constexpr std::uint64_t kSlotBusy = 3;

private:
    enum class Pending : std::uint8_t { none, malloc, crawl, stand, shield, unseen };
    // A keyed state of ours on one component: on while lying.
    struct Hold {
        std::uint64_t on_at = 0;  // the component it is on (0: off)
        bool tried = false;       // this frame's step done
    };
    std::optional<CoopPuppet::Call> Ask(CoopPuppet::Guest& guest, std::int32_t verb, std::uint64_t main_base);
    std::optional<CoopPuppet::Call> Step(CoopPuppet::Guest& guest, Hold& h, Pending kind, std::uint64_t now_at,
                                         std::uint64_t main_base);
    void Note(std::string line);

    mutable std::mutex mutex_;
    Wanted want_;
    std::atomic<std::uint64_t> bsa_{0};  // want_.bsa, read without the lock by Skip
    // Learnt from our Link's own scheduler requests (Walk / Wait: its main slot).
    std::uint64_t node_ = 0;
    std::int32_t slot_ = 0;
    std::uint16_t flags_ = 0;
    bool learnt_ = false;
    bool crawling_ = false;      // our Crawl was taken (until up, an event or a pass verb)
    // The caller's (calc thread).
    bool was_lying_ = false;
    bool stand_due_ = false;     // up again after a crawl: ask Wait once
    int asks_ = 0;
    std::uint64_t next_ask_ms_ = 0;
    std::uint64_t last_ask_ms_ = 0;
    bool blocked_ = false;      // this round's crawl was really refused (not kSlotBusy)
    bool airborne_ = false;     // a kPassVerbs verb let through, no Walk/Wait since
    bool moved_ = false;        // the world moved our Link since this round started
    bool ground_seen_ = false;  // a Walk/Wait of the game's while not crawling (Next may start a round)
    bool cleared_noted_ = false;
    // A crawl round is on: the game's own requests are skipped to free the slot.
    bool Trying() const noexcept {
        return want_.lying && !want_.event && learnt_ && !crawling_ && !airborne_ && !blocked_ &&
               asks_ < kMaxAsks;
    }
    Hold shield_;
    Hold unseen_;
    bool pending_on_ = false;    // the hold call in flight turns on (else off)
    std::uint64_t pending_at_ = 0;
    std::uint64_t block_ = 0;    // our request block in guest memory
    Pending pending_ = Pending::none;
    Counts counts_;
    std::vector<std::string> notes_;
};

Crawl& TheCrawl();
Raising& TheRaising();

}  // namespace CoopDbno::Body
