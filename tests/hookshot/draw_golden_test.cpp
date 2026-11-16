// SPDX-License-Identifier: GPL-3.0-or-later
// Co-op over Hookshot's picture: the WHOLE sequence of drawing calls for a set
// of scenes, against golden records (draw_golden.inc). Written before the
// draw list (docs/ui.md, v1 step 6) took over coop_draw.cpp, from the code as
// it was: the draw list and its replay onto Hookshot's table must keep every
// call, its order and its arguments, so Longshot draws exactly as before.
// Numbers compare within 0.05 px (two compilers' cos/sin); words and colours exactly.
#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "coop_dbno_draw.h"
#include "coop_draw.h"

namespace {

struct Recorder {
    std::string out;
    int font = 0;
    HosCoopDraw d{};

    void Line(const char* kind, std::initializer_list<float> nums, std::uint32_t rgba, const std::string& text = {}) {
        out += kind;
        char buf[48];
        for (const float n : nums) {
            std::snprintf(buf, sizeof buf, " %.2f", static_cast<double>(n));
            out += buf;
        }
        std::snprintf(buf, sizeof buf, " #%08x", static_cast<unsigned>(rgba));
        out += buf;
        if (!text.empty() || std::string(kind) == "text") {
            out += " |" + text;
        }
        out += "\n";
    }

    Recorder(float w, float h, bool draw2) {
        d.width = w;
        d.height = h;
        d.user = this;
        d.rect = [](void* u, float x0, float y0, float x1, float y1, std::uint32_t rgba, float r) {
            static_cast<Recorder*>(u)->Line("rect", {x0, y0, x1, y1, r}, rgba);
        };
        d.circle = [](void* u, float x, float y, float r, std::uint32_t rgba, float t) {
            static_cast<Recorder*>(u)->Line("circle", {x, y, r, t}, rgba);
        };
        d.triangle = [](void* u, float x0, float y0, float x1, float y1, float x2, float y2, std::uint32_t rgba) {
            static_cast<Recorder*>(u)->Line("tri", {x0, y0, x1, y1, x2, y2}, rgba);
        };
        d.text = [](void* u, float x, float y, float px, std::uint32_t rgba, const char* s) {
            auto* r = static_cast<Recorder*>(u);
            r->Line("text", {x, y, px, static_cast<float>(r->font)}, rgba, s);
        };
        d.text_width = [](void*, float px, const char* s) {
            return static_cast<float>(std::string(s).size()) * px * 0.5f;  // half the size per byte
        };
        if (!draw2) {
            return;   // an older Hookshot's table: hos_coop_draw's calls only
        }
        d.font = [](void*, const char*) { return 0; };
        d.use_font = [](void* u, int id) { static_cast<Recorder*>(u)->font = id; };
        d.image = [](void*, int, int, const std::uint8_t*) { return 0; };
        d.draw_image = [](void* u, int id, float x0, float y0, float x1, float y1, std::uint32_t tint) {
            static_cast<Recorder*>(u)->Line("image", {static_cast<float>(id), x0, y0, x1, y1}, tint);
        };
    }
};

CoopDraw::Look GameLookIds() {
    CoopDraw::Look l;
    l.regular = 1;
    l.medium = 2;
    l.hearts = {11, 12, 13, 14, 15};
    l.regular_metrics = {0.85f, 0.65f};
    return l;
}

std::vector<std::string> Tokens(const std::string& line) {
    std::vector<std::string> t;
    const auto bar = line.find(" |");
    std::istringstream in(line.substr(0, bar));
    for (std::string w; in >> w;) {
        t.push_back(w);
    }
    if (bar != std::string::npos) {
        t.push_back(line.substr(bar));
    }
    return t;
}

bool SameLine(const std::string& a, const std::string& b) {
    const auto ta = Tokens(a), tb = Tokens(b);
    if (ta.size() != tb.size()) {
        return false;
    }
    for (std::size_t i = 0; i < ta.size(); ++i) {
        if (ta[i] == tb[i]) {
            continue;
        }
        char* ea = nullptr;
        char* eb = nullptr;
        const double x = std::strtod(ta[i].c_str(), &ea), y = std::strtod(tb[i].c_str(), &eb);
        if (ea == ta[i].c_str() || *ea != '\0' || eb == tb[i].c_str() || *eb != '\0' || std::fabs(x - y) > 0.05) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> Lines(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    for (std::string l; std::getline(in, l);) {
        if (!l.empty()) {
            out.push_back(l);
        }
    }
    return out;
}

const std::map<std::string, std::string>& Golden() {
    static const std::map<std::string, std::string> g = {
#include "draw_golden.inc"
    };
    return g;
}

// Compares a scene's calls with its golden record; prints the whole record when they differ (CI's log is
// where the golden records were first taken from).
void Expect(const std::string& name, const std::string& actual) {
    const auto& g = Golden();
    const auto it = g.find(name);
    bool same = it != g.end();
    if (same) {
        const auto a = Lines(actual), e = Lines(it->second);
        same = a.size() == e.size();
        for (std::size_t i = 0; same && i < a.size(); ++i) {
            same = SameLine(a[i], e[i]);
            if (!same) {
                MESSAGE(name << " line " << i << ": got '" << a[i] << "' want '" << e[i] << "'");
            }
        }
    }
    if (!same) {
        std::printf("GOLDEN-BEGIN %s\n%sGOLDEN-END\n", name.c_str(), actual.c_str());
    }
    CHECK_MESSAGE(same, name);
}

CoopHook::Marker Tag(const char* tag, float x, float y) {
    CoopHook::Marker m;
    m.x = x;
    m.y = y;
    m.tag = tag;
    m.tag_x = x;
    m.tag_y = y - 30;
    m.tag_head = true;
    m.size = 12;
    return m;
}

CoopHook::Markers SomeMarkers() {
    CoopHook::Markers m{640, 360, {}};
    auto head = Tag("Saria", 320, 180);
    auto dot = Tag("", 100, 200);
    dot.tag_head = false;
    auto below = Tag("Mido", 500, 100);
    below.tag_head = false;
    below.tag_below = true;
    below.alpha = 0.5f;
    auto edge = Tag("Darunia", 630, 340);
    edge.edge = true;
    edge.arrow_angle = 0.7f;
    auto self = Tag("you", 300, 300);
    self.self = true;
    auto gone = Tag("Ruto", 10, 10);
    gone.alpha = 0;
    m.items = {head, dot, below, edge, self, gone};
    return m;
}

std::vector<CoopDraw::ChatShown> SomeChat() {
    return {{"Saria: hello there", false, 1.0f},
            {"Mido joined", true, 1.0f},
            {"Darunia: a very long line that will not fit in the chat's width at all, so it is cut with dots", false,
             0.4f}};
}

std::vector<CoopHook::PlayerRow> SomeRows() {
    CoopHook::PlayerRow me;
    me.number = 1;
    me.name = "Saria";
    me.self = true;
    me.state = "connected";
    me.ms = 42;
    me.scene = "Kokiri Forest";
    CoopHook::PlayerRow other;
    other.number = 2;
    other.name = "Mido";
    other.state = "reconnecting";
    other.ms = 120;
    CoopHook::PlayerRow third;
    third.number = 0;
    third.name = "Ruto";
    third.state = "connected";
    third.scene = "Zora's Domain";
    return {me, other, third};
}

coop::overlay::map::Pane MapPane(float size, float x, float y, float k = 1.0f) {
    coop::overlay::map::Pane p;
    p.w = p.h = size;
    p.m = {k, 0, 0, 0, k, 0, 0, 0, 1, x, y, 0};
    return p;
}

coop::overlay::map::Player OnMap(std::uint8_t n, float x, float z, std::uint32_t rgb) {
    coop::overlay::map::Player p;
    p.number = n;
    p.position = {x, 0, z};
    p.scene = "HyruleField";
    p.rgb = rgb;
    return p;
}

struct Case {
    std::string name;
    float w, h;
    bool draw2;
    bool look;
    CoopDraw::Scene scene;
};

std::string Run(const Case& c) {
    Recorder r(c.w, c.h, c.draw2);
    CoopDraw::Draw(r.d, c.scene, c.look ? GameLookIds() : CoopDraw::Look{});
    r.out += "end font " + std::to_string(r.font) + "\n";
    return r.out;
}

}  // namespace

TEST_CASE("hookshot draw golden: every call of each scene, as before the draw list") {
    coop::overlay::map::Capture field;
    field.location = "HyruleField";
    field.height = 240;
    field.floors.push_back({});
    CoopDraw::MapShown map;
    map.scene = "HyruleField";
    map.capture = &field;
    map.minimap = coop::overlay::map::MinimapPanes{};
    map.minimap->picture = MapPane(960, 1536, -690, 0.5f);
    map.minimap->window = MapPane(552, 1536, -690);
    map.players = {OnMap(2, 10, -20, 0xFF0000), OnMap(3, 0, 0, 0x00FF00), OnMap(4, 400, 400, 0x3366FF)};
    map.players[0].rotation = coop::overlay::map::Quat{0, 0, 0, 1};
    map.names = {"Saria", "Darunia", "Ruto"};

    std::vector<Case> cases;
    const auto add = [&](const char* name, float w, float h, bool draw2, bool look, CoopDraw::Scene s) {
        cases.push_back({name, w, h, draw2, look, std::move(s)});
    };
    {
        CoopDraw::Scene s;
        s.chat = SomeChat();
        add("chat-plain", 1280, 720, false, false, s);
        add("chat-game", 1920, 1080, true, true, s);
        s.box = CoopDraw::ChatBoxView{"", false};
        add("chat-box-empty-plain", 1280, 720, true, false, s);
        add("chat-box-empty-game", 1600, 900, true, true, s);
        s.box = CoopDraw::ChatBoxView{"a long message that goes on and on past the end of the box for sure yes", true};
        add("chat-box-failed-plain", 1280, 720, true, false, s);
        add("chat-box-failed-game", 1280, 720, true, true, s);
    }
    {
        CoopDraw::Scene s;
        s.markers = SomeMarkers();
        s.colours = {{"Saria", CoopDraw::Lifted(0x20C040)}, {"Darunia", CoopDraw::Lifted(0xC04020)}};
        add("markers-plain", 1280, 720, false, false, s);
        add("markers-game", 1280, 800, true, true, s);
    }
    {
        CoopDraw::Scene s;
        s.hud = {{"Mido", "Kokiri Forest", {4, 4, 2, 0, 1, 3, 4, 4, 4, 4, 4, 4, 0, 2}, false, CoopVoice::Icon::talking},
                 {"Saria", "", {}, true, CoopVoice::Icon::muted},
                 {"Ruto", "Lake Hylia", {3}, false, CoopVoice::Icon::idle}};
        s.own_voice = CoopVoice::Icon::talking;
        add("hud-game", 1280, 720, true, true, s);
        add("hud-plain", 1280, 720, true, false, s);
    }
    {
        CoopDraw::Scene s;
        s.players = SomeRows();
        add("players-plain", 1280, 720, false, false, s);
        add("players-game", 2560, 1080, true, true, s);
    }
    {
        CoopDraw::Scene s;
        s.panel = std::vector<CoopHook::OverlayLine>{
            {"coop: connected", false}, {"P2 Mido: other scene", true},
            {"P3 Ruto: a line far too long for the panel's width on a small window, cut", false}};
        add("panel", 640, 360, true, true, s);
    }
    {
        CoopDraw::Scene s;
        CoopDraw::CheatsView c;
        c.filter = "bomb";
        c.rows = {"Bombs (20)", "Bomb Bag", "Bombchus (10)"};
        c.selected = 1;
        c.result = "Gave Bomb Bag";
        s.cheats = c;
        add("cheats", 1280, 720, true, true, s);
        s.cheats = CoopDraw::CheatsView{};
        add("cheats-empty", 1280, 720, true, false, s);
    }
    {
        CoopDraw::Scene s;
        s.title = CoopDraw::TitleView{"Multiplayer", true};
        add("title-button-plain", 1280, 720, true, false, s);
        add("title-button-game", 1920, 1200, true, true, s);
        s.title = CoopDraw::TitleView{"Loading the host's save...", false};
        add("title-words-game", 1280, 720, true, true, s);
    }
    {
        CoopDraw::Scene s;
        s.dbno = CoopDbno::View{CoopDbno::View::Kind::downed, "", 32'000'000, 0.25f, false};
        add("dbno-plain", 1280, 720, true, false, s);
        add("dbno-game", 1280, 720, true, true, s);
    }
    {
        CoopDraw::Scene s;
        s.map = &map;
        add("minimap-plain", 1920, 1080, true, false, s);
        add("minimap-game", 1920, 1080, true, true, s);
    }
    {
        CoopDraw::Scene s;   // everything at once: the order of the parts
        s.markers = SomeMarkers();
        s.map = &map;
        s.hud = {{"Mido", "Kokiri Forest", {4, 2}, false, CoopVoice::Icon::none}};
        s.panel = std::vector<CoopHook::OverlayLine>{{"coop: connected", false}};
        s.dbno = CoopDbno::View{CoopDbno::View::Kind::downed, "", 5'000'000, 0.5f, false};
        s.chat = SomeChat();
        s.box = CoopDraw::ChatBoxView{"hi", false};
        s.players = SomeRows();
        s.cheats = CoopDraw::CheatsView{};
        s.title = CoopDraw::TitleView{"Multiplayer", true};
        add("all-game", 1600, 900, true, true, s);
        add("all-plain", 1600, 900, true, false, s);
        add("all-old-table", 1600, 900, false, true, s);
    }
    for (const auto& c : cases) {
        Expect(c.name, Run(c));
    }
    CHECK(cases.size() == Golden().size());
}
