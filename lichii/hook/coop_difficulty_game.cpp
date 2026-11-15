// SPDX-License-Identifier: GPL-3.0-or-later
//
// Difficulty, the session's side (coop_difficulty.h has the rules). The
// server's DIFFICULTY is what is in force; the host's choice from the pause
// menu (Set) is sent to the server and kept in coop-difficulty.ini beside
// coop.ini for this player's next session as host.

#include "coop_difficulty.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <system_error>
#include <utility>
#include <variant>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "coop_every.h"
#include "coop_ngplus.h"
#include "coop/host/session.hpp"
#include "coop/net/client.hpp"

namespace CoopDifficulty {
namespace {

namespace proto = coop::protocol;
namespace fs = std::filesystem;

// The five values in one word, for any thread, 12 bits each (every end is
// below 4096: boss life 1600). (Navi: it held three, so the bosses' own life
// and damage never reached the game: they stayed 100%.)
std::uint64_t Packed(const Values& v) {
    auto f = [](std::uint16_t pct) { return static_cast<std::uint64_t>(pct & 0xfff); };
    return f(v.hp_pct) | f(v.damage_pct) << 12 | f(v.count_pct) << 24 | f(v.boss_hp_pct) << 36 |
           f(v.boss_damage_pct) << 48;
}

Values Unpacked(std::uint64_t word) {
    auto f = [word](int at) { return static_cast<std::uint16_t>((word >> at) & 0xfff); };
    return {f(0), f(12), f(24), f(36), f(48)};
}

struct Glue {
    std::mutex mutex;
    State state;                    // network thread writes, under mutex
    std::atomic<std::uint64_t> now{Packed(Values{})};  // state.values, for any thread
    std::atomic<bool> told{false};  // (Navi) the server said the values (they hold New Game+ then)
    std::optional<Choice> wish;     // Set -> Frame
    std::optional<Choice> kept;     // the file's, read once
    bool kept_read = false;
    bool offered = false;           // the kept choice was sent on this connection
    std::vector<std::string> lines; // any thread -> chat box

    void Line(std::string text) {
        if (lines.size() < 32) {
            lines.push_back(std::move(text));
        }
    }
};

Glue& TheGlue() {
    static Glue glue;
    return glue;
}

fs::path File() {
    return Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "coop-difficulty.ini";
}

// Under the mutex. A missing or broken file is no choice.
const std::optional<Choice>& Kept(Glue& g) {
    if (!g.kept_read) {
        g.kept_read = true;
        std::ifstream f(File(), std::ios::binary);
        if (f) {
            std::ostringstream text;
            text << f.rdbuf();
            g.kept = ParseChoice(text.str());
            if (!g.kept) {
                LOG_ERROR(Core, "coop: difficulty: {} is not a difficulty file; ignored", File().string());
            }
        }
    }
    return g.kept;
}

// Written whole: a new file beside it, then moved over the old one.
void Keep(const Choice& choice) {
    const fs::path file = File();
    fs::path tmp = file;
    tmp += ".new";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f || !(f << FormatChoice(choice)) || !f.flush()) {
            LOG_ERROR(Core, "coop: difficulty: could not write {}", tmp.string());
            return;
        }
    }
    std::error_code ec;
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(tmp, ec);
        LOG_ERROR(Core, "coop: difficulty: could not write {}", file.string());
    }
}

proto::Difficulty Message(const Choice& choice) {
    proto::Difficulty m;
    m.auto_scale = choice.auto_scale;
    m.preset = choice.preset;
    m.values = proto::clamp_difficulty(choice.custom);
    return m;
}

} // namespace

State Get() {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    return g.state;
}

Values Now() {
    Glue& g = TheGlue();
    const Values v = Unpacked(g.now.load());
    // (Navi) New Game+ alone: our own level on the game as it is. In a session the server's values hold the host's.
    return g.told.load() ? v : proto::ngplus_difficulty(v, CoopNgPlus::Level());
}

bool Set(const Choice& choice) {
    Glue& g = TheGlue();
    {
        std::lock_guard lock(g.mutex);
        if (!g.state.connected || !g.state.can_edit) {
            return false;
        }
        g.wish = choice;
        g.kept = choice;
        g.kept_read = true;
    }
    Keep(choice);
    return true;
}

void OnEvent(const coop::net::Event& event, coop::net::Client& client) {
    using coop::net::EventKind;
    Glue& g = TheGlue();
    if (event.kind == EventKind::welcomed || event.kind == EventKind::reconnected ||
        event.kind == EventKind::disconnected) {
        // Each connection starts with the game as it is; the server says the rest.
        std::lock_guard lock(g.mutex);
        g.state = State{};
        g.now.store(Packed(Values{}));
        g.told.store(false);
        g.wish.reset();
        g.offered = false;
        return;
    }
    if (event.kind == EventKind::host_changed) {
        std::lock_guard lock(g.mutex);
        g.state.can_edit = client.is_host();
        return;
    }
    if (event.kind != EventKind::message) {
        return;
    }
    const auto* m = std::get_if<proto::Difficulty>(&event.message);
    if (m == nullptr) {
        return;
    }
    std::lock_guard lock(g.mutex);
    const bool first = !g.state.connected;
    // Worth a chat line: anything but the game as it is at the join, and
    // every change after it.
    const bool changed = first ? m->set_by != 0 || m->values != Values{}
                               : m->values != g.state.values ||
                                     m->auto_scale != g.state.choice.auto_scale ||
                                     m->preset != g.state.choice.preset;
    g.state.connected = true;
    g.state.can_edit = client.is_host();
    g.state.choice.auto_scale = m->auto_scale;
    g.state.choice.preset = m->preset;
    if (!m->auto_scale && m->preset == Preset::custom) {
        g.state.choice.custom = m->values;  // the host's sliders
    }
    g.state.values = m->values;
    g.state.set_by = m->set_by;
    g.now.store(Packed(m->values));
    g.told.store(true);
    const std::size_t players = client.players().size();  // (this client included)
    if (changed) {
        g.Line("Difficulty: " + Describe(g.state.choice, m->values, players));
    }
    LOG_INFO(Core, "coop: difficulty: {} (set by {})", Describe(g.state.choice, m->values, players), m->set_by);
}

// The hits the game's Life step scaled (Kinds::Step), said at most every 5 s
// when there were new ones: the in-game proof that the values reach the game.
static void LogHits() {
    static CoopEvery::Every every{5'000'000};
    static std::uint64_t enemy_said = 0;
    static std::uint64_t link_said = 0;
    if (!every.Due(CoopEvery::NowUs())) {
        return;
    }
    // A full table (Kinds::kSlots in one scene): what it could not mark goes unscaled. Said once a scene.
    static bool full_said = false;
    const std::uint64_t unmarked = TheKinds().Unmarked();
    if (unmarked == 0) {
        full_said = false;
    } else if (!full_said) {
        full_said = true;
        LOG_WARNING(Core, "coop: difficulty: the kinds table is full ({} slots): {} actors or Life components "
                          "in this scene are not scaled",
                    Kinds::kSlots, unmarked);
    }
    const std::uint64_t enemy = TheKinds().EnemyHits();
    const std::uint64_t link = TheKinds().LinkHits();
    if (enemy == enemy_said && link == link_said) {
        return;
    }
    const Values v = Now();
    LOG_INFO(Core, "coop: difficulty: the Life step scaled {} hits on enemies (life {}%) and {} on our Link (damage {}%)",
             enemy - enemy_said, v.hp_pct, link - link_said, v.damage_pct);
    enemy_said = enemy;
    link_said = link;
}

void Frame(coop::host::Session* session) {
    LogHits();
    if (session == nullptr) {
        return;
    }
    Glue& g = TheGlue();
    std::optional<Choice> send;
    {
        std::lock_guard lock(g.mutex);
        if (!g.state.connected) {
            return;
        }
        if (g.wish) {
            send = std::exchange(g.wish, std::nullopt);
            g.offered = true;  // the host chose here: the kept one is this one
        } else if (OffersKept(g.state.can_edit, g.state.set_by, Kept(g), g.offered)) {
            send = g.kept;
            g.offered = true;
        }
    }
    if (!send) {
        return;
    }
    const bool sent = session->with_client([&](coop::net::Client& client) {
        return client.state() == coop::net::State::connected && client.is_host() &&
               client.send(Message(*send));
    });
    if (!sent) {
        std::lock_guard lock(g.mutex);
        g.Line("Difficulty: could not be sent");
    }
}

std::vector<std::string> TakeLines() {
    Glue& g = TheGlue();
    std::lock_guard lock(g.mutex);
    return std::exchange(g.lines, {});
}

} // namespace CoopDifficulty
