// SPDX-License-Identifier: GPL-3.0-or-later
#include "coop_quest.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <utility>

namespace CoopQuest {
namespace {

// Murmur3 x86 32, seed 0, of each step name.
constexpr std::uint32_t kNotReady = 0x57b849f0u;
constexpr std::uint32_t kReady = 0x17bfaa46u;
constexpr std::uint32_t kStep01 = 0xeb62add9u;
constexpr std::uint32_t kStep02 = 0x0e5afad8u;
constexpr std::uint32_t kStep02_01 = 0x608637dfu;
constexpr std::uint32_t kStep2_1 = 0xd984a45eu;
constexpr std::uint32_t kSrep2_1_1 = 0x38170256u;  // sic, the game's name
constexpr std::uint32_t kStep2_2 = 0xb5294bddu;
constexpr std::uint32_t kStep03 = 0x6f917dfeu;
constexpr std::uint32_t kStep03_01 = 0x14376953u;
constexpr std::uint32_t kStep03_02 = 0xd932a31cu;
constexpr std::uint32_t kStep04 = 0x27f660feu;
constexpr std::uint32_t kStep05 = 0x5cde171fu;
constexpr std::uint32_t kStep06 = 0x21d6fa38u;
constexpr std::uint32_t kComplete = 0x4c0a63f4u;

// Shared story flags (coop_story.cpp).
constexpr std::uint32_t kOwlToCastle = 0x98bf10c1u;  // HavePlayedEvent.HyruleField_Goto_HyruleCastle_Kaepora
constexpr std::uint32_t kOwlScene = 0xd4ef7853u;     // HavePlayedEvent.Dm_HrlField_0010
constexpr std::uint32_t kTalonWoke = 0xb9d7d709u;    // HavePlayedEvent.Dm_HrlCastle_0020
constexpr std::uint32_t kImpaCame = 0x8e15ffd6u;     // HavePlayedEvent.Dm_HrlCastle_0060

constexpr std::uint32_t kGateOpen = 0x19898d14u;     // Npc_Hylia_Soldier_003_GateOpen

// Get_StoneOfFire's steps (RSDB Challenge, in order).
constexpr std::uint32_t kKakarikoVillageNpc = 0x4011c4aau;
constexpr std::uint32_t kKakarikoVillageLetter = 0x4d329ec9u;
constexpr std::uint32_t kDeathMtTrailIn = 0x1d5e4b01u;
constexpr std::uint32_t kDeathMtTrailNpc = 0xf05c8740u;
constexpr std::uint32_t kGoronCityIn = 0x4968c687u;
constexpr std::uint32_t kGoronCityDarniaroomNpcTalk01 = 0x49a9bc8cu;
constexpr std::uint32_t kGoronCityDarniaroomOpen = 0xe4167a6au;
constexpr std::uint32_t kGoronCityDarniaTalk = 0xfa1d363au;
constexpr std::uint32_t kDarniaRoomKokiriTalk = 0xdf107a36u;
constexpr std::uint32_t kKokiriForestNPCTalk = 0xf8649356u;
constexpr std::uint32_t kKokiriForestFire = 0x70e44053u;
constexpr std::uint32_t kKokiriForestIn = 0xfef39213u;
constexpr std::uint32_t kSariaMelodyGet = 0x19e79ac0u;
constexpr std::uint32_t kSariaMelodyPlay = 0xc9879474u;
constexpr std::uint32_t kDodongoCavernOpne = 0x1dfb4ab5u;  // sic, the game's name
constexpr std::uint32_t kDungeonStart = 0x22a0fc04u;
constexpr std::uint32_t kBossBattle = 0xb83d269bu;
constexpr std::uint32_t kEscapeDungeon = 0x3ecd8c01u;

// The quests' Enums: Murmur3 of "Step_" + the quest's name.
constexpr std::uint32_t kGotoHyruleCastle = 0xdb89c1a9u;
constexpr std::uint32_t kDoorOfTime = 0x378913cau;
constexpr std::uint32_t kStoneOfFire = 0x1002b466u;
constexpr std::uint32_t kStoneOfFireName = 0xe4c584e6u;  // the challenge: Get_StoneOfFire
constexpr std::uint32_t kStoneOfWater = 0x5bca02a6u;      // Enum Step_Get_StoneOfWater
constexpr std::uint32_t kDoorOfTimeName = 0xe8dafe6cu;    // the challenge: DoorOfTime

constexpr std::array kGotoSteps{kNotReady, kReady,     kStep01,    kStep02, kStep02_01,
                                kStep03,   kStep03_01, kStep03_02, kStep04, kComplete};
constexpr std::array kDoorSteps{kNotReady, kReady,  kStep01, kStep02, kStep2_1, kSrep2_1_1,
                                kStep2_2,  kStep03, kStep04, kStep05, kStep06,  kComplete};
constexpr std::array kFireSteps{kNotReady,
                                kReady,
                                kKakarikoVillageNpc,
                                kKakarikoVillageLetter,
                                kDeathMtTrailIn,
                                kDeathMtTrailNpc,
                                kGoronCityIn,
                                kGoronCityDarniaroomNpcTalk01,
                                kGoronCityDarniaroomOpen,
                                kGoronCityDarniaTalk,
                                kDarniaRoomKokiriTalk,
                                kKokiriForestNPCTalk,
                                kKokiriForestFire,
                                kKokiriForestIn,
                                kSariaMelodyGet,
                                kSariaMelodyPlay,
                                kDodongoCavernOpne,
                                kDungeonStart,
                                kBossBattle,
                                kEscapeDungeon,
                                kComplete};

// Goto_HyruleCastle: the owl at the field entrance (event flow
// Goto_HyruleCastle_Kaepora_00) gives Step02; Talon woken (Dm_HrlCastle_0020)
// Step04; the meeting with Zelda (Goto_HyruleCastle_MeetZelda: scenes 0030,
// 0035, 0045, 0050, then Impa's 0060) Complete.
constexpr std::array kGotoRules{
    Rule{kOwlToCastle, kStep02, "the owl at the Hyrule Field entrance"},
    Rule{kOwlScene, kStep02, "the owl at the Hyrule Field entrance"},
    Rule{kTalonWoke, kStep04, "Talon woken at the castle"},
    Rule{kImpaCame, kComplete, "Zelda met in the courtyard"},
};
// DoorOfTime: the same meeting gives Step01 (Impa waits in the courtyard and
// stops Link when he leaves, then takes him out: Zelda's Lullaby). Step02 is
// not raised: each player then meets Impa, whose send-off (DoorOfTime_SendOff)
// sets Step02 and Get_StoneOfFire KakarikoVillageNpc, the step the Kakariko
// guard needs (issue 171).
//
// The stone steps (issue 60): the game's DoorOfTime_CheckStoneNum sets Step2_1
// (the Goron Ruby), Step2_2 (the Zora Sapphire) or Step03 (all three) from
// Get_StoneOfFire_IsCompleted_Exp and Get_StoneOfWater_IsCompleted_Exp, but it
// runs only in Dm_SY_BancChange_EscapeDungeon, right after the player's own
// Darunia or Jabu-Jabu escape. A player who was elsewhere gets the stone (a
// shared quest) and never runs it, so their DoorOfTime stayed at Step02: no
// Zelda escape in the field (it starts at Step03) and a Door of Time that
// stays shut. These rules do what CheckStoneNum does, from our own GameData.
// Step04 and later (the escape, the Ocarina of Time, the door) stay each
// game's own: they follow from Step03 in that game.
constexpr std::array kDoorRules{
    Rule{kImpaCame, kStep01, "Zelda met in the courtyard (Impa comes)"},
    Rule{0, kStep2_1, "the Goron Ruby (as DoorOfTime_CheckStoneNum)", kStoneOfFire},
    Rule{0, kStep2_2, "the Zora Sapphire (as DoorOfTime_CheckStoneNum)", kStoneOfWater},
    Rule{0, kStep03, "all three Spiritual Stones (as DoorOfTime_CheckStoneNum)", kStoneOfFire,
         kStoneOfWater},
};
// Get_StoneOfFire (issue 171): showing Zelda's Letter to the Kakariko guard
// sets his Bool (shared) and moves the shower's quest from KakarikoVillageNpc
// to KakarikoVillageLetter; the gate's shutter opens on that step, not on the
// Bool. Another player's guard finds the Bool set and skips the letter talk, so
// their quest stayed behind and their gate shut. The Bool now gives
// KakarikoVillageLetter, but only once our own Impa sent us (KakarikoVillageNpc).
constexpr std::array kFireRules{
    Rule{kGateOpen, kKakarikoVillageLetter, "Zelda's Letter shown at the Kakariko gate"},
};

constexpr std::array kQuests{
    // Goto_HyruleCastle only after our own Deku Tree (Step01 comes from the
    // escape from the dungeon): never for a game still before it.
    Quest{kGotoHyruleCastle, "Goto_HyruleCastle", kGotoSteps, kGotoRules, 2},
    // DoorOfTime is NotReady until Goto_HyruleCastle is complete; the
    // meeting with Zelda moves it from there.
    // Its manager entry from Step2_1 (index 4) on: the escape's trigger reads it.
    Quest{kDoorOfTime, "DoorOfTime", kDoorSteps, kDoorRules, 0, kGotoHyruleCastle, kComplete,
          kDoorOfTimeName, 4},
    // From KakarikoVillageNpc (index 2) on: set by our own Impa.
    Quest{kStoneOfFire, "Get_StoneOfFire", kFireSteps, kFireRules, 2, 0, 0, kStoneOfFireName, 2},
};

// The steps the manager is never moved into (RSDB Challenge: IsBossBattle or
// IsMiniGame), as {challenge, step}: Murmur3 of the challenge's and step's names.
constexpr std::array<std::pair<std::uint32_t, std::uint32_t>, 19> kHeld{{
    {0xe8dafe6cu, kStep04},      // DoorOfTime Step04
    {0xe873ce22u, 0x085d18f0u},  // Get_Epona GambleRacePlaying
    {kStoneOfFireName, kBossBattle},  // Get_StoneOfFire BossBattle
    {kStoneOfFireName, kEscapeDungeon},
    {0x628de7a6u, 0xb5df43cbu},  // Get_StoneOfForest GoToBossRoom
    {0x628de7a6u, 0x576277fau},  // Get_StoneOfForest DefeatBoss
    {0x628de7a6u, 0xd52b1014u},  // Get_StoneOfForest GoToWarpPoint
    {0x0aa38424u, 0xc25a29d8u},  // Get_StoneOfWater Step12
    {0x0aa38424u, 0xa6447454u},  // Get_StoneOfWater Step13_01
    {0x0aa38424u, 0x84be6ff1u},  // Get_StoneOfWater Step14
    {0x533999f4u, 0x576277fau},  // SageOfFire DefeatBoss
    {0x533999f4u, 0xd52b1014u},  // SageOfFire GoToWarpPoint
    {0x5a2be4b5u, kStep06},      // SageOfForest Step06
    {0x5a2be4b5u, 0x277efd68u},  // SageOfForest Step06_1_1
    {0x5a2be4b5u, 0xee867c27u},  // SageOfForest Step07
    {0x5a2be4b5u, 0xa594ed54u},  // SageOfForest Step08
    {0x5a2be4b5u, 0x5daeca58u},  // SageOfForest Step09
    {0x7caec227u, 0x50d665b4u},  // SubEvent_GetStormSong BeforeBossRoom
    {0x7caec227u, 0x9bddd011u},  // SubEvent_GetStormSong step04
}};

// ChallengeMgr's layout (coop_quest.h).
constexpr std::uint64_t kArrays = 0x40;  // three {i32 count, pad, u64 entries}, 0x10 apart
constexpr std::uint64_t kEntrySize = 0x160;
constexpr std::uint64_t kEntryChallenge = 0x08;
constexpr std::uint64_t kEntryStep = 0x10;
constexpr std::uint64_t kEntryFlags = 0x16;
constexpr std::uint64_t kEntryAsked = 0xa4;
constexpr std::uint16_t kFlagAsked = 0x400;
constexpr std::int32_t kMostEntries = 256;  // the table has 45 rows

template <typename T>
std::optional<T> Get(CoopPuppet::Guest& guest, std::uint64_t address) {
    T v{};
    if (!guest.Read(address, &v, sizeof v)) return std::nullopt;
    return v;
}

std::optional<std::size_t> IndexOf(const std::vector<std::uint32_t>& steps, std::uint32_t step) {
    const auto it = std::find(steps.begin(), steps.end(), step);
    if (it == steps.end()) return std::nullopt;
    return static_cast<std::size_t>(it - steps.begin());
}

std::string Hex(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%#x", v);
    return buf;
}

// The manager's entry of `challenge`, or 0.
std::uint64_t FindEntry(CoopPuppet::Guest& guest, std::uint64_t mgr, std::uint32_t challenge) {
    for (std::uint64_t a = 0; a < 3; ++a) {
        const auto count = Get<std::int32_t>(guest, mgr + kArrays + 0x10 * a);
        const auto entries = Get<std::uint64_t>(guest, mgr + kArrays + 0x10 * a + 8);
        if (!count || !entries || *count <= 0 || *count > kMostEntries || *entries == 0) continue;
        for (std::int32_t i = 0; i < *count; ++i) {
            const std::uint64_t entry = *entries + kEntrySize * static_cast<std::uint64_t>(i);
            if (Get<std::uint32_t>(guest, entry + kEntryChallenge) == challenge) return entry;
        }
    }
    return 0;
}

// Guest memory through a ReadMemory, never written (LiveStep, LiveDescribe).
class ReadOnly final : public CoopPuppet::Guest {
public:
    explicit ReadOnly(const ReadMemory& read) : read_(read) {}
    bool Read(std::uint64_t address, void* out, std::size_t size) override { return read_(address, out, size); }
    bool Write(std::uint64_t, const void*, std::size_t) override { return false; }

private:
    const ReadMemory& read_;
};

} // namespace

std::optional<std::uint32_t> LiveStep(const ReadMemory& read, std::uint64_t main, std::uint32_t challenge) {
    ReadOnly guest(read);
    if (main == 0) return std::nullopt;
    const auto mgr = Get<std::uint64_t>(guest, main + kChallengeMgr);
    if (!mgr || *mgr == 0) return std::nullopt;
    const std::uint64_t entry = FindEntry(guest, *mgr, challenge);
    if (entry == 0) return std::nullopt;
    return Get<std::uint32_t>(guest, entry + kEntryStep);
}

std::string LiveDescribe(const ReadMemory& read, std::uint64_t main, std::uint32_t challenge) {
    ReadOnly guest(read);
    if (main == 0) return "no main";
    const auto mgr = Get<std::uint64_t>(guest, main + kChallengeMgr);
    if (!mgr) return "manager pointer unread";
    if (*mgr == 0) return "no manager";
    char at[24];
    std::snprintf(at, sizeof at, "%#llx", static_cast<unsigned long long>(*mgr));
    std::string out = std::string("manager ") + at + ", arrays";
    for (std::uint64_t a = 0; a < 3; ++a) {
        const auto count = Get<std::int32_t>(guest, *mgr + kArrays + 0x10 * a);
        out += count ? " " + std::to_string(*count) : " unread";
    }
    const std::uint64_t entry = FindEntry(guest, *mgr, challenge);
    if (entry == 0) return out + ", " + Hex(challenge) + " not found";
    const auto step = Get<std::uint32_t>(guest, entry + kEntryStep);
    return out + ", " + Hex(challenge) + " at " + (step ? Hex(*step) : std::string("unread"));
}

bool HeldStep(std::uint32_t challenge, std::uint32_t step) {
    return std::find(kHeld.begin(), kHeld.end(), std::pair{challenge, step}) != kHeld.end();
}

std::optional<StepWant> LiveWant(const Quest& quest, const ReadEnum& value) {
    if (quest.live == 0) return std::nullopt;
    const auto at = value(quest.hash);
    const auto rank = at ? Rank(quest, *at) : std::nullopt;
    if (!rank || *rank < quest.live_floor) return std::nullopt;
    return StepWant{quest.live, *at, {quest.steps.begin(), quest.steps.end()}, quest.name};
}

void Challenges::Want(std::vector<StepWant> wants) {
    std::lock_guard lock(mutex_);
    waiting_ = std::move(wants);
}

std::vector<std::string> Challenges::TakeNotes() {
    std::lock_guard lock(mutex_);
    return std::exchange(notes_, {});
}

std::optional<CoopPuppet::Call> Challenges::Step(CoopPuppet::Guest& guest, bool after_call,
                                                 std::uint64_t result, std::uint64_t main) {
    std::lock_guard lock(mutex_);
    if (!after_call) {
        // Each list is checked on the one frame after it came, never again:
        // the hook gives none while our Link is in an event.
        work_ = std::exchange(waiting_, {});
        next_ = 0;
    } else if (next_ > 0 && next_ <= work_.size()) {
        const StepWant& w = work_[next_ - 1];
        notes_.push_back("quest " + w.name + " " + Hex(asked_from_) + " -> " + Hex(w.step) +
                         ((result & 1) != 0 ? " asked of the game's ChallengeMgr (issue 171)"
                                            : " refused by the game's ChallengeMgr"));
    }
    if (main == 0) return std::nullopt;
    const auto mgr = Get<std::uint64_t>(guest, main + kChallengeMgr);
    if (!mgr || *mgr == 0) return std::nullopt;  // no game loaded yet
    while (next_ < work_.size()) {
        const StepWant& w = work_[next_++];
        if (HeldStep(w.challenge, w.step)) continue;
        const auto want = IndexOf(w.steps, w.step);
        const std::uint64_t entry = FindEntry(guest, *mgr, w.challenge);
        if (!want || entry == 0) continue;
        const auto at = Get<std::uint32_t>(guest, entry + kEntryStep);
        const auto have = at ? IndexOf(w.steps, *at) : std::nullopt;
        if (!have || *want <= *have) continue;  // never lowered; an unknown step is left alone
        if (*have == 0 || HeldStep(w.challenge, *at)) continue;  // NotReady; a fight or race goes on
        const auto flags = Get<std::uint16_t>(guest, entry + kEntryFlags);
        const auto asked = Get<std::uint32_t>(guest, entry + kEntryAsked);
        if (!flags || !asked || ((*flags & kFlagAsked) != 0 && *asked == w.step)) continue;
        asked_from_ = *at;
        CoopPuppet::Call c;
        c.function = main + kSetStep;
        c.x[0] = *mgr;
        c.x[1] = w.challenge;
        c.x[2] = w.step;  // w3, w4, force: 0, as the game's own calls
        return c;
    }
    return std::nullopt;
}

std::span<const Quest> Quests() {
    return kQuests;
}

std::optional<std::size_t> Rank(const Quest& quest, std::uint32_t value) {
    for (std::size_t i = 0; i < quest.steps.size(); ++i) {
        if (quest.steps[i] == value) return i;
    }
    return std::nullopt;
}

std::optional<std::uint32_t> CatchUp(const Quest& quest, const ReadBool& flag,
                                     const ReadEnum& value) {
    const auto now = value(quest.hash);
    if (!now) return std::nullopt;
    const auto rank = Rank(quest, *now);
    if (!rank || *rank < quest.floor) return std::nullopt;
    if (quest.needs_quest != 0) {
        const Quest* other = nullptr;
        for (const auto& q : kQuests) {
            if (q.hash == quest.needs_quest) other = &q;
        }
        const auto have = value(quest.needs_quest);
        const auto have_rank = other && have ? Rank(*other, *have) : std::nullopt;
        const auto need_rank = other ? Rank(*other, quest.needs_step) : std::nullopt;
        if (!have_rank || !need_rank || *have_rank < *need_rank) return std::nullopt;
    }
    std::optional<std::size_t> best;
    for (const auto& r : quest.rules) {
        if (r.flag == 0 && r.done_a == 0 && r.done_b == 0) continue;
        if (r.flag != 0 && flag(r.flag) != true) continue;
        if (r.done_a != 0 && value(r.done_a) != kComplete) continue;
        if (r.done_b != 0 && value(r.done_b) != kComplete) continue;
        const auto to = Rank(quest, r.step);
        if (to && (!best || *to > *best)) best = to;
    }
    if (!best || *best <= *rank) return std::nullopt;
    return quest.steps[*best];
}

} // namespace CoopQuest
