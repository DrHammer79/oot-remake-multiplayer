// Tests for each game's quests catching up with the shared story
// (lichii/hook/coop_quest.cpp, issue 173).
#include <doctest/doctest.h>

#include <map>
#include <iterator>
#include <optional>
#include <string>

#include "coop_quest.h"
#include "coop_story.h"

namespace {

// Murmur3 x86 32, seed 0 (as coop_story's names).
std::uint32_t Hash(const std::string& s) {
    const auto* d = reinterpret_cast<const unsigned char*>(s.data());
    const std::size_t n = s.size() / 4;
    std::uint32_t h = 0;
    const auto rotl = [](std::uint32_t x, int r) { return (x << r) | (x >> (32 - r)); };
    for (std::size_t i = 0; i < n; ++i) {
        std::uint32_t k = d[4 * i] | (d[4 * i + 1] << 8) | (d[4 * i + 2] << 16) |
                          (static_cast<std::uint32_t>(d[4 * i + 3]) << 24);
        k *= 0xcc9e2d51u;
        k = rotl(k, 15);
        k *= 0x1b873593u;
        h ^= k;
        h = rotl(h, 13);
        h = h * 5 + 0xe6546b64u;
    }
    std::uint32_t k = 0;
    const auto* t = d + 4 * n;
    switch (s.size() & 3) {
    case 3:
        k ^= t[2] << 16;
        [[fallthrough]];
    case 2:
        k ^= t[1] << 8;
        [[fallthrough]];
    case 1:
        k ^= t[0];
        k *= 0xcc9e2d51u;
        k = rotl(k, 15);
        k *= 0x1b873593u;
        h ^= k;
    }
    h ^= static_cast<std::uint32_t>(s.size());
    h ^= h >> 16;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    h *= 0xc2b2ae35u;
    h ^= h >> 16;
    return h;
}

const CoopQuest::Quest& Q(const char* name) {
    for (const auto& q : CoopQuest::Quests()) {
        if (std::string(q.name) == name) return q;
    }
    CHECK_MESSAGE(false, name);
    return CoopQuest::Quests().front();
}

struct Game {
    std::map<std::uint32_t, bool> bools;
    std::map<std::uint32_t, std::uint32_t> enums;
    CoopQuest::ReadBool flag = [this](std::uint32_t h) -> std::optional<bool> {
        auto it = bools.find(h);
        if (it == bools.end()) return std::nullopt;
        return it->second;
    };
    CoopQuest::ReadEnum value = [this](std::uint32_t h) -> std::optional<std::uint32_t> {
        auto it = enums.find(h);
        if (it == enums.end()) return std::nullopt;
        return it->second;
    };
    // One frame of the hook: every quest in order, written at once.
    void Frame() {
        for (const auto& q : CoopQuest::Quests()) {
            if (const auto r = CoopQuest::CatchUp(q, flag, value)) enums[q.hash] = *r;
        }
    }
    void Played(const char* scene) { bools[Hash(std::string("HavePlayedEvent.") + scene)] = true; }
};

const std::uint32_t kGoto = Hash("Goto_HyruleCastle");  // not the Enum's hash: see below

} // namespace

TEST_CASE("quest: the step hashes are the step names, and each quest's steps are distinct") {
    for (const auto& q : CoopQuest::Quests()) {
        CHECK(q.steps.front() == Hash("NotReady"));
        CHECK(q.steps.back() == Hash("Complete"));
        for (std::size_t i = 0; i < q.steps.size(); ++i) {
            CHECK(CoopQuest::Rank(q, q.steps[i]) == i);
        }
        for (const auto& r : q.rules) {
            CHECK(CoopQuest::Rank(q, r.step).has_value());
            // Every rule flag is a shared story flag; a rule without one
            // (the stones, issue 60) names the quests that must be Complete.
            if (r.flag != 0) {
                CHECK(CoopStory::Find(r.flag) != nullptr);
            } else {
                CHECK(r.done_a != 0);
            }
        }
    }
    const auto& g = Q("Goto_HyruleCastle");
    CHECK(g.hash == 0xdb89c1a9u);
    CHECK(g.steps[3] == Hash("Step02"));
    CHECK(g.steps[8] == Hash("Step04"));
    const auto& d = Q("DoorOfTime");
    CHECK(d.hash == 0x378913cau);
    CHECK(d.steps[2] == Hash("Step01"));
    CHECK(d.steps[5] == Hash("Srep2_1_1"));
    CHECK(kGoto != g.hash);  // the Enum's hash is not its plain name's
}

TEST_CASE("quest: after A met Zelda, B's quests reach Complete and Step01 (Impa comes)") {
    const auto& g = Q("Goto_HyruleCastle");
    const auto& d = Q("DoorOfTime");
    Game b;
    b.enums[g.hash] = Hash("Step03_02");  // B is in the castle grounds
    b.enums[d.hash] = Hash("NotReady");
    b.Frame();
    CHECK(b.enums[g.hash] == Hash("Step03_02"));  // nothing shared yet
    CHECK(b.enums[d.hash] == Hash("NotReady"));
    // A's meeting: scenes 0030..0060 reach B as shared flags.
    for (const char* s : {"Dm_HrlCastle_0030", "Dm_HrlCastle_0035", "Dm_HrlCastle_0045",
                          "Dm_HrlCastle_0050", "Dm_HrlCastle_0060"}) {
        b.Played(s);
    }
    b.Frame();
    CHECK(b.enums[g.hash] == Hash("Complete"));
    CHECK(b.enums[d.hash] == Hash("Step01"));
    // Lullaby played by A: B stays at Step01, so B's Impa still takes B out.
    b.Played("Dm_MelodyGet_Zelda");
    b.Frame();
    CHECK(b.enums[d.hash] == Hash("Step01"));
}

TEST_CASE("quest: never lowered, never from before our own Deku Tree, unknown left alone") {
    const auto& g = Q("Goto_HyruleCastle");
    const auto& d = Q("DoorOfTime");
    Game ahead;
    ahead.enums[g.hash] = Hash("Complete");
    ahead.enums[d.hash] = Hash("Step2_2");
    ahead.Played("Dm_HrlCastle_0060");
    ahead.Played("Dm_HrlField_0010");
    ahead.Frame();
    CHECK(ahead.enums[g.hash] == Hash("Complete"));
    CHECK(ahead.enums[d.hash] == Hash("Step2_2"));

    Game early;  // a game still before the Deku Tree: Goto NotReady
    early.enums[g.hash] = Hash("NotReady");
    early.enums[d.hash] = Hash("NotReady");
    early.Played("Dm_HrlCastle_0060");
    early.Frame();
    CHECK(early.enums[g.hash] == Hash("NotReady"));
    CHECK(early.enums[d.hash] == Hash("NotReady"));  // needs our Goto complete

    Game odd;
    odd.enums[g.hash] = 0x12345678u;
    odd.enums[d.hash] = Hash("NotReady");
    odd.Played("Dm_HrlCastle_0060");
    odd.Frame();
    CHECK(odd.enums[g.hash] == 0x12345678u);
    CHECK(odd.enums[d.hash] == Hash("NotReady"));

    Game unread;  // nothing readable: nothing written
    unread.Played("Dm_HrlCastle_0060");
    unread.Frame();
    CHECK(unread.enums.empty());
}

TEST_CASE("quest: the owl and Talon move Goto_HyruleCastle on, the highest rule wins") {
    const auto& g = Q("Goto_HyruleCastle");
    Game b;
    b.enums[g.hash] = Hash("Step01");  // after B's Deku Tree
    b.Played("Dm_HrlField_0010");
    b.Frame();
    CHECK(b.enums[g.hash] == Hash("Step02"));
    b.Played("Dm_HrlCastle_0020");
    b.Frame();
    CHECK(b.enums[g.hash] == Hash("Step04"));

    Game c;
    c.enums[g.hash] = Hash("Step01");
    c.Played("Dm_HrlField_0010");
    c.Played("Dm_HrlCastle_0060");
    c.Frame();
    CHECK(c.enums[g.hash] == Hash("Complete"));
}

TEST_CASE("quest: Get_StoneOfFire's Enum and steps are the game's names") {
    const auto& f = Q("Get_StoneOfFire");
    CHECK(f.hash == Hash("Step_Get_StoneOfFire"));
    CHECK(Q("DoorOfTime").hash == Hash("Step_DoorOfTime"));
    CHECK(Q("Goto_HyruleCastle").hash == Hash("Step_Goto_HyruleCastle"));
    const char* names[] = {"NotReady", "Ready", "KakarikoVillageNpc", "KakarikoVillageLetter",
                           "DeathMtTrailIn", "DeathMtTrailNpc", "GoronCityIn",
                           "GoronCityDarniaroomNpcTalk01", "GoronCityDarniaroomOpen",
                           "GoronCityDarniaTalk", "DarniaRoomKokiriTalk", "KokiriForestNPCTalk",
                           "KokiriForestFire", "KokiriForestIn", "SariaMelodyGet", "SariaMelodyPlay",
                           "DodongoCavernOpne", "DungeonStart", "BossBattle", "EscapeDungeon", "Complete"};
    REQUIRE(f.steps.size() == std::size(names));
    for (std::size_t i = 0; i < f.steps.size(); ++i) {
        CHECK_MESSAGE(f.steps[i] == Hash(names[i]), names[i]);
    }
    CHECK(f.rules.size() == 1);
    CHECK(f.rules[0].flag == Hash("Npc_Hylia_Soldier_003_GateOpen"));
}

TEST_CASE("quest: the letter shown at A's gate opens B's (issue 171), once B's Impa sent B") {
    const auto& f = Q("Get_StoneOfFire");
    const std::uint32_t gate = Hash("Npc_Hylia_Soldier_003_GateOpen");
    Game b;
    b.enums[f.hash] = Hash("KakarikoVillageNpc");  // B met Impa: sent to Kakariko
    b.Frame();
    CHECK(b.enums[f.hash] == Hash("KakarikoVillageNpc"));
    b.bools[gate] = true;  // A showed the letter
    b.Frame();
    CHECK(b.enums[f.hash] == Hash("KakarikoVillageLetter"));

    Game early;  // before Impa: the quest is not B's yet
    early.enums[f.hash] = Hash("Ready");
    early.bools[gate] = true;
    early.Frame();
    CHECK(early.enums[f.hash] == Hash("Ready"));

    Game ahead;  // already up the mountain: never lowered
    ahead.enums[f.hash] = Hash("GoronCityIn");
    ahead.bools[gate] = true;
    ahead.Frame();
    CHECK(ahead.enums[f.hash] == Hash("GoronCityIn"));
}

TEST_CASE("quest: the held steps are the table's boss and minigame steps") {
    using CoopQuest::HeldStep;
    CHECK(HeldStep(Hash("Get_StoneOfFire"), Hash("BossBattle")));
    CHECK(HeldStep(Hash("Get_StoneOfFire"), Hash("EscapeDungeon")));
    CHECK(HeldStep(Hash("DoorOfTime"), Hash("Step04")));
    CHECK(HeldStep(Hash("Get_Epona"), Hash("GambleRacePlaying")));
    CHECK(HeldStep(Hash("Get_StoneOfForest"), Hash("GoToBossRoom")));
    CHECK(HeldStep(Hash("Get_StoneOfForest"), Hash("DefeatBoss")));
    CHECK(HeldStep(Hash("Get_StoneOfForest"), Hash("GoToWarpPoint")));
    CHECK(HeldStep(Hash("Get_StoneOfWater"), Hash("Step12")));
    CHECK(HeldStep(Hash("Get_StoneOfWater"), Hash("Step13_01")));
    CHECK(HeldStep(Hash("Get_StoneOfWater"), Hash("Step14")));
    CHECK(HeldStep(Hash("SageOfFire"), Hash("DefeatBoss")));
    CHECK(HeldStep(Hash("SageOfFire"), Hash("GoToWarpPoint")));
    for (const char* s : {"Step06", "Step06_1_1", "Step07", "Step08", "Step09"}) {
        CHECK_MESSAGE(HeldStep(Hash("SageOfForest"), Hash(s)), s);
    }
    CHECK(HeldStep(Hash("SubEvent_GetStormSong"), Hash("BeforeBossRoom")));
    CHECK(HeldStep(Hash("SubEvent_GetStormSong"), Hash("step04")));
    CHECK_FALSE(HeldStep(Hash("Get_StoneOfFire"), Hash("KakarikoVillageLetter")));
    CHECK_FALSE(HeldStep(Hash("Get_StoneOfForest"), Hash("Complete")));
    CHECK_FALSE(HeldStep(Hash("Goto_HyruleCastle"), Hash("Step04")));  // another quest's Step04
}

namespace {

// Guest memory, byte by byte: unwritten bytes cannot be read.
class Memory : public CoopPuppet::Guest {
public:
    std::map<std::uint64_t, std::uint8_t> bytes;
    bool Read(std::uint64_t address, void* out, std::size_t size) override {
        auto* o = static_cast<std::uint8_t*>(out);
        for (std::size_t i = 0; i < size; ++i) {
            const auto it = bytes.find(address + i);
            if (it == bytes.end()) return false;
            o[i] = it->second;
        }
        return true;
    }
    bool Write(std::uint64_t, const void*, std::size_t) override {
        return false;
    }
    template <typename T>
    void Put(std::uint64_t address, T v) {
        const auto bits = static_cast<std::uint64_t>(v);
        for (std::size_t i = 0; i < sizeof v; ++i) {
            bytes[address + i] = static_cast<std::uint8_t>(bits >> (8 * i));
        }
    }
};

constexpr std::uint64_t kMain = 0x80020000;
constexpr std::uint64_t kMgr = 0x90000000;
constexpr std::uint64_t kEntries = 0x91000000;

// A manager whose second array holds Get_StoneOfFire as its entry 3, at `at`.
struct Mgr {
    Memory m;
    std::uint64_t entry = kEntries + 3 * 0x160;
    explicit Mgr(const char* at) {
        m.Put<std::uint64_t>(kMain + CoopQuest::kChallengeMgr, kMgr);
        for (std::uint64_t a = 0; a < 3; ++a) {
            m.Put<std::int32_t>(kMgr + 0x40 + 0x10 * a, a == 1 ? 5 : 0);
            m.Put<std::uint64_t>(kMgr + 0x48 + 0x10 * a, a == 1 ? kEntries : 0);
        }
        for (std::uint64_t i = 0; i < 5; ++i) {
            const std::uint32_t name = i == 3 ? Hash("Get_StoneOfFire") : Hash("Other" + std::to_string(i));
            m.Put<std::uint32_t>(kEntries + i * 0x160 + 0x08, name);
        }
        m.Put<std::uint32_t>(entry + 0x10, Hash(at));
        m.Put<std::uint16_t>(entry + 0x16, 0);
        m.Put<std::uint32_t>(entry + 0xa4, 0);
    }
};

CoopQuest::StepWant Fire(const char* step) {
    const auto& f = Q("Get_StoneOfFire");
    return {Hash("Get_StoneOfFire"), Hash(step), {f.steps.begin(), f.steps.end()}, "Get_StoneOfFire"};
}

} // namespace

TEST_CASE("quest: a GameData step ahead of the game's ChallengeMgr is asked through setStep") {
    Mgr g("KakarikoVillageNpc");
    CoopQuest::Challenges c;
    c.Want({Fire("KakarikoVillageLetter")});
    const auto call = c.Step(g.m, false, 0, kMain);
    REQUIRE(call.has_value());
    CHECK(call->function == kMain + 0x2cf54a0);
    CHECK(call->x[0] == kMgr);
    CHECK(call->x[1] == Hash("Get_StoneOfFire"));
    CHECK(call->x[2] == Hash("KakarikoVillageLetter"));
    CHECK(call->x[3] == 0);
    CHECK(call->x[4] == 0);
    CHECK(call->x[5] == 0);  // no force, as the game's own calls
    CHECK_FALSE(c.Step(g.m, true, 1, kMain).has_value());  // one call, then done
    const auto notes = c.TakeNotes();
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].find("asked of the game") != std::string::npos);
    // A list is used once: the next frame asks nothing.
    CHECK_FALSE(c.Step(g.m, false, 0, kMain).has_value());
}

TEST_CASE("quest: the ChallengeMgr is never lowered, never moved into a held step or asked twice") {
    CoopQuest::Challenges c;
    {
        Mgr g("GoronCityIn");  // ahead of GameData
        c.Want({Fire("KakarikoVillageLetter")});
        CHECK_FALSE(c.Step(g.m, false, 0, kMain).has_value());
    }
    {
        Mgr g("KakarikoVillageLetter");  // the same step
        c.Want({Fire("KakarikoVillageLetter")});
        CHECK_FALSE(c.Step(g.m, false, 0, kMain).has_value());
    }
    {
        Mgr g("DungeonStart");
        c.Want({Fire("BossBattle")});  // a boss fight: played, never given
        CHECK_FALSE(c.Step(g.m, false, 0, kMain).has_value());
    }
    {
        Mgr g("KakarikoVillageNpc");  // the step is already asked
        g.m.Put<std::uint16_t>(g.entry + 0x16, 0x400);
        g.m.Put<std::uint32_t>(g.entry + 0xa4, Hash("KakarikoVillageLetter"));
        c.Want({Fire("KakarikoVillageLetter")});
        CHECK_FALSE(c.Step(g.m, false, 0, kMain).has_value());
    }
    {
        Mgr g("SomethingElse");  // a step we do not know
        c.Want({Fire("KakarikoVillageLetter")});
        CHECK_FALSE(c.Step(g.m, false, 0, kMain).has_value());
    }
    {
        Mgr g("KakarikoVillageNpc");  // a quest the manager does not have
        auto w = Fire("KakarikoVillageLetter");
        w.challenge = Hash("NoSuchQuest");
        c.Want({w});
        CHECK_FALSE(c.Step(g.m, false, 0, kMain).has_value());
    }
    {
        Memory none;  // no game loaded: no manager
        c.Want({Fire("KakarikoVillageLetter")});
        CHECK_FALSE(c.Step(none, false, 0, kMain).has_value());
        Mgr g("KakarikoVillageNpc");
        c.Want({Fire("KakarikoVillageLetter")});
        CHECK_FALSE(c.Step(g.m, false, 0, 0).has_value());  // main not known
    }
    CHECK(c.TakeNotes().empty());
}

TEST_CASE("quest: nothing is asked from NotReady, and a refusal is noted") {
    {
        Mgr g("NotReady");  // the game refuses it without force; none is passed
        CoopQuest::Challenges c;
        c.Want({Fire("KakarikoVillageLetter")});
        CHECK_FALSE(c.Step(g.m, false, 0, kMain).has_value());
    }
    Mgr g("KakarikoVillageNpc");
    CoopQuest::Challenges c;
    c.Want({Fire("KakarikoVillageLetter"), Fire("DeathMtTrailIn")});
    const auto first = c.Step(g.m, false, 0, kMain);
    REQUIRE(first.has_value());
    CHECK(first->x[2] == Hash("KakarikoVillageLetter"));
    const auto second = c.Step(g.m, true, 0, kMain);  // the first was refused (0)
    REQUIRE(second.has_value());
    CHECK(second->x[2] == Hash("DeathMtTrailIn"));
    CHECK_FALSE(c.Step(g.m, true, 1, kMain).has_value());
    const auto notes = c.TakeNotes();
    REQUIRE(notes.size() == 2);
    CHECK(notes[0].find("refused") != std::string::npos);
    CHECK(notes[1].find("asked") != std::string::npos);
}

TEST_CASE("quest: a player in a boss fight or minigame step is never moved out of it") {
    CoopQuest::Challenges c;
    for (const char* in : {"BossBattle", "EscapeDungeon"}) {
        Mgr g(in);  // B is fighting King Dodongo (or escaping) when A finishes
        c.Want({Fire("Complete")});
        CHECK_MESSAGE(!c.Step(g.m, false, 0, kMain).has_value(), in);
    }
    Mgr g("DungeonStart");  // not in the fight: the step after it is fine
    c.Want({Fire("Complete")});
    CHECK(c.Step(g.m, false, 0, kMain).has_value());
}

TEST_CASE("quest: Get_StoneOfFire and DoorOfTime go to the live manager, each from its floor") {
    Game g;
    const auto& f = Q("Get_StoneOfFire");
    const auto& d = Q("DoorOfTime");
    for (const auto& q : CoopQuest::Quests()) {
        g.enums[q.hash] = q.steps[q.steps.size() - 2];  // every quest far along
    }
    for (const auto& q : CoopQuest::Quests()) {
        const auto w = CoopQuest::LiveWant(q, g.value);
        CHECK_MESSAGE(w.has_value() == (q.hash == f.hash || q.hash == d.hash), q.name);
        if (w) {
            CHECK(w->challenge == Hash(q.name));
            CHECK(w->step == g.enums[q.hash]);
            CHECK(w->steps.size() == q.steps.size());
        }
    }
    g.enums[f.hash] = Hash("Ready");  // below the floor: not B's quest yet
    CHECK_FALSE(CoopQuest::LiveWant(f, g.value).has_value());
    g.enums[f.hash] = Hash("KakarikoVillageNpc");
    CHECK(CoopQuest::LiveWant(f, g.value).has_value());
    g.enums[f.hash] = 0x12345678u;  // unknown
    CHECK_FALSE(CoopQuest::LiveWant(f, g.value).has_value());
    g.enums.erase(f.hash);  // unread
    CHECK_FALSE(CoopQuest::LiveWant(f, g.value).has_value());
    // DoorOfTime from the stone steps on: Step01 and Step02 stay GameData only
    // (each player meets Impa, who moves the manager herself).
    for (const char* s : {"NotReady", "Step01", "Step02"}) {
        g.enums[d.hash] = Hash(s);
        CHECK_MESSAGE(!CoopQuest::LiveWant(d, g.value).has_value(), s);
    }
    for (const char* s : {"Step2_1", "Step2_2", "Step03"}) {
        g.enums[d.hash] = Hash(s);
        const auto w = CoopQuest::LiveWant(d, g.value);
        REQUIRE_MESSAGE(w.has_value(), s);
        CHECK(w->challenge == Hash("DoorOfTime"));
        CHECK(w->step == Hash(s));
    }
    // Step04 (Zelda's escape) is a held step: never asked of the manager.
    CHECK(CoopQuest::HeldStep(Hash("DoorOfTime"), Hash("Step04")));
}

TEST_CASE("quest: the Spiritual Stones move DoorOfTime on, as DoorOfTime_CheckStoneNum (issue 60)") {
    const auto& g = Q("Goto_HyruleCastle");
    const auto& d = Q("DoorOfTime");
    const auto& f = Q("Get_StoneOfFire");
    const std::uint32_t water = 0x5bca02a6u;
    CHECK(Hash("Step_Get_StoneOfWater") == water);
    // B met Zelda and was sent off by Impa, then stayed in Hyrule Field while
    // A got both stones. The stones are shared quests: B's are Complete, but
    // CheckStoneNum runs only in each dungeon's escape, which B never played.
    Game b;
    b.enums[g.hash] = Hash("Complete");
    b.enums[d.hash] = Hash("Step02");
    b.enums[f.hash] = Hash("DeathMtTrailIn");
    b.enums[water] = Hash("Step07");
    b.Frame();
    CHECK(b.enums[d.hash] == Hash("Step02"));  // no stone yet
    b.enums[f.hash] = Hash("Complete");
    b.Frame();
    CHECK(b.enums[d.hash] == Hash("Step2_1"));  // the Goron Ruby
    b.enums[water] = Hash("Complete");
    b.Frame();
    CHECK(b.enums[d.hash] == Hash("Step03"));  // all three: Zelda's escape can start
    // The water stone alone gives Step2_2.
    Game c;
    c.enums[g.hash] = Hash("Complete");
    c.enums[d.hash] = Hash("Step02");
    c.enums[f.hash] = Hash("Ready");
    c.enums[water] = Hash("Complete");
    c.Frame();
    CHECK(c.enums[d.hash] == Hash("Step2_2"));
    // Never lowered: a player past the escape keeps their step.
    Game e;
    e.enums[g.hash] = Hash("Complete");
    e.enums[d.hash] = Hash("Step05");
    e.enums[f.hash] = Hash("Complete");
    e.enums[water] = Hash("Complete");
    e.Frame();
    CHECK(e.enums[d.hash] == Hash("Step05"));
    // Not before our own meeting with Zelda (Goto_HyruleCastle Complete).
    Game n;
    n.enums[g.hash] = Hash("Step03");
    n.enums[d.hash] = Hash("NotReady");
    n.enums[f.hash] = Hash("Complete");
    n.enums[water] = Hash("Complete");
    n.Frame();
    CHECK(n.enums[d.hash] == Hash("NotReady"));
    // An unread stone quest moves nothing.
    Game u;
    u.enums[g.hash] = Hash("Complete");
    u.enums[d.hash] = Hash("Step02");
    u.enums[f.hash] = Hash("Complete");
    u.Frame();
    CHECK(u.enums[d.hash] == Hash("Step2_1"));
}

TEST_CASE("quest: the live step is read from the game's ChallengeMgr, not GameData (issue 58)") {
    Mgr g("KakarikoVillageNpc");
    const CoopQuest::ReadMemory read = [&g](std::uint64_t a, void* out, std::size_t n) { return g.m.Read(a, out, n); };
    CHECK(CoopQuest::LiveStep(read, kMain, Hash("Get_StoneOfFire")) == Hash("KakarikoVillageNpc"));
    CHECK_FALSE(CoopQuest::LiveStep(read, kMain, Hash("Get_StoneOfWater")));  // not in this manager
    CHECK_FALSE(CoopQuest::LiveStep(read, 0, Hash("Get_StoneOfFire")));
    Memory none;  // no game loaded: no manager
    const CoopQuest::ReadMemory nothing = [&none](std::uint64_t a, void* out, std::size_t n) {
        return none.Read(a, out, n);
    };
    CHECK_FALSE(CoopQuest::LiveStep(nothing, kMain, Hash("Get_StoneOfFire")));
}
