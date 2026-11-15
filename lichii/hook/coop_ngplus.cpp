// SPDX-License-Identifier: GPL-3.0-or-later
//
// (Navi) New Game+ (coop_ngplus.h has the rules).

#include "coop_ngplus.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <system_error>
#include <utility>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "coop/host/game_flags.hpp"
#include "coop/host/player_reader.hpp"
#include "coop/protocol/difficulty.hpp"
#include "coop_every.h"
#include "coop_flags.h"
#include "coop_live_events.h"
#include "coop_objects.h"

namespace CoopNgPlus {
namespace {

namespace fs = std::filesystem;
namespace proto = coop::protocol;

constexpr std::uint32_t kMaxLifeHash = 0xfbe01da1u;  // PlayerStatus.MaxLife (docs/items.md), quarter hearts
constexpr std::int32_t kMostQuarters = 80;           // 20 hearts
constexpr std::uint64_t kBeatenAgainUs = 600'000'000;  // one Ganon death a 10 minutes (the event is asked more than once)

struct Glue {
    std::mutex mutex;
    Saved saved;
    bool read = false;
    std::atomic<std::uint32_t> packed{0};  // level | unlocked << 8 | hearts << 16, for any thread
    std::uint64_t cursor = 0;              // the game's event requests read so far (the hook's thread)
    std::uint64_t beaten_at = 0;
    std::int32_t hearts_said = -1;
    bool test_done = false;
    std::vector<std::string> lines;
};

Glue& TheGlue() {
    static Glue glue;
    return glue;
}

fs::path File() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "coop-ngplus.ini";
}

void Publish(Glue& g) {
    g.packed.store(static_cast<std::uint32_t>(g.saved.level) | static_cast<std::uint32_t>(g.saved.unlocked) << 8 |
                   static_cast<std::uint32_t>(std::clamp(g.saved.hearts, 0, kMostQuarters)) << 16);
}

// Under the mutex.
void ReadOnce(Glue& g) {
    if (g.read) {
        return;
    }
    g.read = true;
    std::ifstream f(File(), std::ios::binary);
    if (f) {
        std::ostringstream text;
        text << f.rdbuf();
        g.saved = Parse(text.str());
    }
    Publish(g);
    LOG_INFO(Core, "coop: New Game+: level {}, unlocked {}, hearts {} (quarters)", g.saved.level, g.saved.unlocked,
             g.saved.hearts);
}

// Under the mutex. Written whole: a new file, then moved over the old one.
void Keep(Glue& g) {
    Publish(g);
    const fs::path file = File();
    fs::path tmp = file;
    tmp += ".new";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f || !(f << Format(g.saved)) || !f.flush()) {
            LOG_ERROR(Core, "coop: New Game+: could not write {}", tmp.string());
            return;
        }
    }
    std::error_code ec;
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(tmp, ec);
        LOG_ERROR(Core, "coop: New Game+: could not write {}", file.string());
    }
}

Glue& Ready() {
    Glue& g = TheGlue();
    if (!g.read) {
        std::lock_guard lock(g.mutex);
        ReadOnce(g);
    }
    return g;
}

int IntOf(std::string_view v) {
    int n = 0;
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
    std::from_chars(v.data(), v.data() + v.size(), n);
    return n;
}

std::string Hearts(std::int32_t quarters) {
    const std::int32_t whole = quarters / 4, part = quarters % 4;
    return std::to_string(whole) + (part ? " " + std::to_string(part) + "/4" : std::string()) + " hearts";
}

}  // namespace

Saved Parse(std::string_view text) {
    Saved s;
    while (!text.empty()) {
        const auto end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        const auto eq = line.find('=');
        if (line.empty() || line.front() == '#' || eq == std::string_view::npos) continue;
        std::string_view key = line.substr(0, eq);
        while (!key.empty() && key.back() == ' ') key.remove_suffix(1);
        const int v = IntOf(line.substr(eq + 1));
        if (key == "level") s.level = static_cast<std::uint8_t>(std::clamp(v, 0, int{proto::kMaxNgPlus}));
        if (key == "unlocked") s.unlocked = static_cast<std::uint8_t>(std::clamp(v, 0, int{proto::kMaxNgPlus}));
        if (key == "hearts") s.hearts = std::clamp(v, 0, kMostQuarters);
    }
    s.level = std::min(s.level, s.unlocked);
    return s;
}

std::string Format(const Saved& s) {
    return "# New Game+ (Navi): the level chosen (0 off), the highest unlocked by beating Ganon, the hearts carried\n"
           "# (quarter hearts). Chosen on the Multiplayer tab's Settings page.\n"
           "level = " + std::to_string(s.level) + "\nunlocked = " + std::to_string(s.unlocked) +
           "\nhearts = " + std::to_string(s.hearts) + "\n";
}

Saved Beaten(Saved s, std::int32_t max_life) {
    const int next = std::min(int{s.level} + 1, int{proto::kMaxNgPlus});
    s.unlocked = static_cast<std::uint8_t>(std::max(int{s.unlocked}, next));
    if (max_life > 0) {
        s.hearts = std::max(s.hearts, std::min(max_life, kMostQuarters));
    }
    return s;
}

Saved Choose(Saved s, int level) {
    s.level = static_cast<std::uint8_t>(std::clamp(level, 0, int{s.unlocked}));
    return s;
}

bool IsGanonDeath(std::string_view event) {
    // Ganon's death setting and flow (coop_boss_scenes.cpp: "Ganon"), and the ending's flow.
    return event == "InsideGanonCastle_Dungeon_BossDie_2" || event == "Dm_FinalBattle_BossOut" ||
           event == "Defeat_Ganon";
}

std::uint8_t Level() {
    return static_cast<std::uint8_t>(Ready().packed.load() & 0xff);
}

std::uint8_t Unlocked() {
    return static_cast<std::uint8_t>((Ready().packed.load() >> 8) & 0xff);
}

std::int32_t Hearts() {
    return static_cast<std::int32_t>(Ready().packed.load() >> 16);
}

bool SetLevel(int level) {
    Glue& g = Ready();
    std::lock_guard lock(g.mutex);
    const Saved now = Choose(g.saved, level);
    if (now == g.saved) {
        return false;
    }
    g.saved = now;
    Keep(g);
    const proto::DifficultyValues v = proto::ngplus_difficulty({}, now.level);
    LOG_INFO(Core, "coop: New Game+: level {} chosen", now.level);
    if (g.lines.size() < 16) {
        g.lines.push_back(now.level == 0 ? std::string("New Game+ off")
                                         : "New Game+ " + std::to_string(now.level) + ": enemies " +
                                               std::to_string(v.hp_pct) + "% life, " + std::to_string(v.damage_pct) +
                                               "% damage; you start with " + Hearts(now.hearts));
    }
    return true;
}

void Frame(Core::System& system, CoopHook::EmulatorMemory& memory, bool in_game) {
    Glue& g = Ready();
    const std::uint64_t now_us = CoopEvery::NowUs();
    bool beaten = false;
    for (const auto& r : CoopLiveEvents::TheGameRequests().Since(g.cursor)) {
        if (r.taken && IsGanonDeath(r.name)) {
            beaten = true;
            LOG_INFO(Core, "coop: New Game+: Ganon's death seen ({})", r.name);
        }
    }
    // A test without beating the game: NAVI_NGPLUS_TEST_BEAT=1 counts Ganon beaten once Link is in a save.
    if (!g.test_done && in_game) {
        g.test_done = true;
        if (const char* t = std::getenv("NAVI_NGPLUS_TEST_BEAT"); t != nullptr && t[0] == '1') {
            beaten = true;
            LOG_INFO(Core, "coop: New Game+: Ganon counted beaten (NAVI_NGPLUS_TEST_BEAT)");
        }
    }
    coop::host::GameFlagStore store(memory);
    const proto::FlagKey max_key{kMaxLifeHash, 0};
    auto max_life = in_game ? store.read(max_key) : std::nullopt;
    const bool known = max_life && max_life->kind == proto::FlagKind::int32;
    if (beaten && (g.beaten_at == 0 || now_us - g.beaten_at >= kBeatenAgainUs)) {
        g.beaten_at = now_us;
        std::lock_guard lock(g.mutex);
        const Saved was = g.saved;
        g.saved = Beaten(g.saved, known ? static_cast<std::int32_t>(max_life->value) : 0);
        Keep(g);
        LOG_INFO(Core, "coop: New Game+: Ganon beaten at level {}: unlocked {}, hearts {}", was.level, g.saved.unlocked,
                 g.saved.hearts);
        if (g.saved.unlocked != was.unlocked && g.lines.size() < 16) {
            g.lines.push_back("New Game+ " + std::to_string(g.saved.unlocked) + " unlocked, with " +
                              Hearts(g.saved.hearts) + ": Multiplayer, Settings, New Game+, then start a new file");
        }
    }
    if (!in_game) {
        return;
    }
    static CoopEvery::Every every{1'000'000};
    if (!every.Due(now_us)) {
        return;
    }
    const std::uint32_t packed = g.packed.load();
    const std::int32_t hearts = static_cast<std::int32_t>(packed >> 16);
    if ((packed & 0xff) == 0 || hearts <= 0) {
        return;
    }
    // The save's MaxLife (shared in a session by the max rule) and Link's own Life component, raised, never lowered.
    if (known && static_cast<std::int32_t>(max_life->value) > 0 &&
        static_cast<std::int32_t>(max_life->value) < hearts) {
        auto set = *max_life;
        set.value = static_cast<std::uint32_t>(hearts);
        const bool ok = store.write(set);
        LOG_INFO(Core, "coop: New Game+: MaxLife {} -> {}{}", static_cast<std::int32_t>(max_life->value), hearts,
                 ok ? "" : ": the write failed");
    }
    coop::host::PlayerReader reader(memory);
    if (const auto actor = reader.find_player_actor()) {
        if (const auto before = CoopObjects::RaiseLinkMax(system, *actor, hearts)) {
            LOG_INFO(Core, "coop: New Game+: our Link's max life {} -> {}", *before, hearts);
            std::lock_guard lock(g.mutex);
            if (g.hearts_said != hearts && g.lines.size() < 16) {
                g.hearts_said = hearts;
                g.lines.push_back("New Game+: your " + Hearts(hearts) + " carried over");
            }
        }
    }
}

std::vector<std::string> TakeLines() {
    Glue& g = Ready();
    std::lock_guard lock(g.mutex);
    return std::exchange(g.lines, {});
}

}  // namespace CoopNgPlus
