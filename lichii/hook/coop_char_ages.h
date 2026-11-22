// A Character Select pick per age (player request, 2026-11-22): one character for child Link, another for adult Link,
// switched by itself when Link changes age (the Master Sword, the Door of Time, a save loaded at the other age).
//
// Snail's mod keeps one pick for both ages (CharacterMenu_SelectedCharacter, an Int in the save) and builds the player
// model's path from it whenever the game makes the player (a scene or area load, a save load, his own swap as his page
// closes): the age only chooses Player_Child or Player_Adult. So this keeps two picks of its own and writes the one for
// the age Link is (the game's Sequence_IsChildhood) into the mod's Int; the next time the player is made, the model is
// that age's. The page stays the one picker: what the player picks there becomes the pick for the age they are now.
//
// Rules, each frame with the values read (Picks::Frame):
// - Off, no mod, the age unknown, or the title: nothing.
// - Snail's page open or its swap in flight: a changed pick is the player's choice for the current age (stored, not
//   written: his page writes it).
// - This age has no pick yet: the current pick becomes it (turning the feature on changes nothing on screen).
// - Else the stored pick for this age is the one wanted (Link when its model is not installed; the stored pick is
//   kept for when it is); when the mod's value differs, it is written, again at most every kRewriteFrames.
// Any other change of the mod's value (an age change, a save load, a host's save) is written over, never stored.
// Header only, standard C++ only; tests/lichii/char_ages_test.cpp.
#pragma once

#include <charconv>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "coop_char_slots.h"

namespace CoopCharAges {

struct Stored {
    bool on = false;
    std::optional<std::int32_t> child, adult;
    bool operator==(const Stored&) const = default;
};

// The settings file's one line: "on child adult" (1 or 0, then each pick or -1 for none).
inline std::string Format(const Stored& s) {
    return std::to_string(s.on ? 1 : 0) + " " + std::to_string(s.child.value_or(-1)) + " " +
           std::to_string(s.adult.value_or(-1)) + "\n";
}

inline std::optional<Stored> Parse(std::string_view text) {
    std::int32_t v[3] = {0, -1, -1};
    std::size_t at = 0;
    for (int i = 0; i < 3; ++i) {
        while (at < text.size() && (text[at] == ' ' || text[at] == '\t')) ++at;
        const char* first = text.data() + at;
        const char* last = text.data() + text.size();
        const auto r = std::from_chars(first, last, v[i]);
        if (r.ec != std::errc{}) return std::nullopt;
        at = static_cast<std::size_t>(r.ptr - text.data());
    }
    if ((v[0] != 0 && v[0] != 1) || v[1] < -1 || v[2] < -1) return std::nullopt;
    Stored s;
    s.on = v[0] == 1;
    if (v[1] >= 0) s.child = v[1];
    if (v[2] >= 0) s.adult = v[2];
    return s;
}

struct Input {
    std::optional<bool> childhood;     // Sequence_IsChildhood (nothing: not read)
    std::optional<std::int32_t> pick;  // CharacterMenu_SelectedCharacter (nothing: no mod)
    bool page_open = false;            // his page is up (CharacterMenu_IsPageOpen)
    bool swap_busy = false;            // his page's camera, slide or field swap still running
    bool title = false;                // the title screen (the age read there is a stale save's)
};

struct Step {
    std::optional<std::int32_t> write;  // store this in the mod's value now
    bool stored_changed = false;        // the picks changed: save them
};

class Picks {
public:
    static constexpr int kRewriteFrames = 30;

    explicit Picks(Stored s = {}) : s_(s) {}

    const Stored& stored() const { return s_; }
    void SetOn(bool on) {
        s_.on = on;
        wait_ = 0;
    }
    // A pick chosen outside his page (the settings rows): 0 is Link.
    void SetPick(bool child, std::int32_t n) {
        (child ? s_.child : s_.adult) = n < 0 ? 0 : n;
        wait_ = 0;
    }

    Step Frame(const Input& in, const std::map<std::uint16_t, std::uint16_t>& files) {
        Step out;
        if (!s_.on || !in.pick || !in.childhood || in.title) {
            last_seen_.reset();
            return out;
        }
        std::optional<std::int32_t>& mine = *in.childhood ? s_.child : s_.adult;
        if (in.page_open || in.swap_busy) {
            if (last_seen_ && *in.pick != *last_seen_ && CoopCharSlots::Installed(*in.pick, files) && mine != *in.pick) {
                mine = *in.pick;   // the player picked on his page: this age's character
                out.stored_changed = true;
            }
            last_seen_ = *in.pick;
            wait_ = 0;
            return out;
        }
        last_seen_ = *in.pick;
        if (!mine) {
            mine = *in.pick;   // the first frame at this age: what is shown now becomes its pick
            out.stored_changed = true;
            return out;
        }
        const std::int32_t want = CoopCharSlots::Installed(*mine, files) ? *mine : 0;
        if (*in.pick == want) {
            wait_ = 0;
            return out;
        }
        if (wait_ > 0) {
            --wait_;
            return out;
        }
        out.write = want;
        wait_ = kRewriteFrames;
        return out;
    }

private:
    Stored s_;
    std::optional<std::int32_t> last_seen_;
    int wait_ = 0;
};

// --- The pickable characters of the mod's characters.json ---
struct Character {
    std::int32_t number = 0;
    std::string name;
    bool operator==(const Character&) const = default;
};

namespace detail {

inline bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

struct Reader {
    std::string_view t;
    std::size_t at = 0;
    void Space() {
        while (at < t.size() && IsSpace(t[at])) ++at;
    }
    bool Eat(char c) {
        Space();
        if (at < t.size() && t[at] == c) {
            ++at;
            return true;
        }
        return false;
    }
    std::optional<std::string> String() {
        Space();
        if (at >= t.size() || t[at] != '"') return std::nullopt;
        std::string out;
        for (++at; at < t.size(); ++at) {
            if (t[at] == '"') {
                ++at;
                return out;
            }
            if (t[at] == '\\' && at + 1 < t.size()) ++at;   // (an escaped character is kept as it is)
            out += t[at];
        }
        return std::nullopt;
    }
    // Skips one value of any kind (an object or array by its brackets, a string, or a bare word up to , } ]).
    bool Skip() {
        Space();
        if (at >= t.size()) return false;
        if (t[at] == '"') return String().has_value();
        if (t[at] == '{' || t[at] == '[') {
            int depth = 0;
            while (at < t.size()) {
                if (t[at] == '"') {
                    if (!String()) return false;
                    continue;
                }
                if (t[at] == '{' || t[at] == '[') ++depth;
                if (t[at] == '}' || t[at] == ']') --depth;
                ++at;
                if (depth == 0) return true;
            }
            return false;
        }
        while (at < t.size() && t[at] != ',' && t[at] != '}' && t[at] != ']') ++at;
        return true;
    }
};

}  // namespace detail

// The characters a player can pick on the mod's page: the "characters" rows with "picker": true, in file order, by the
// mod's stored number and name (a row without a number or a name is left out; a list that cannot be read is empty).
// Which of them have a model on this PC is CoopCharSlots::Installed's question (Pickable).
inline std::vector<Character> ParseCharacters(std::string_view text) {
    std::vector<Character> out;
    const auto key = text.find("\"characters\"");
    if (key == std::string_view::npos) return out;
    detail::Reader r{text, key + 12};
    if (!r.Eat(':') || !r.Eat('[')) return out;
    while (r.Eat('{')) {
        Character c;
        bool has_number = false, picker = false;
        std::string name;
        while (!r.Eat('}')) {
            const auto k = r.String();
            if (!k || !r.Eat(':')) return out;
            r.Space();
            if (*k == "number" || *k == "picker") {
                const std::size_t from = r.at;
                if (!r.Skip()) return out;
                std::string_view v = text.substr(from, r.at - from);
                while (!v.empty() && detail::IsSpace(v.back())) v.remove_suffix(1);
                if (*k == "picker") {
                    picker = v == "true";
                } else {
                    std::int32_t n = 0;
                    const auto res = std::from_chars(v.data(), v.data() + v.size(), n);
                    has_number = res.ec == std::errc{} && n >= 0;
                    c.number = n;
                }
            } else if (*k == "name") {
                const auto v = r.String();
                if (!v) return out;
                name = *v;
            } else if (!r.Skip()) {
                return out;
            }
            r.Eat(',');
        }
        if (has_number && picker && !name.empty()) {
            c.name = std::move(name);
            out.push_back(std::move(c));
        }
        if (!r.Eat(',')) break;
    }
    return out;
}

// The rows the settings page can offer: the pickable characters whose model is installed (`files` nothing: unknown,
// all of them). Link (0) is always there, first.
inline std::vector<Character> Pickable(const std::vector<Character>& list,
                                       const std::optional<std::map<std::uint16_t, std::uint16_t>>& files) {
    std::vector<Character> out{{0, "Link"}};
    for (const auto& c : list) {
        if (c.number == 0) continue;
        if (!files || CoopCharSlots::Installed(c.number, *files)) out.push_back(c);
    }
    return out;
}

// --- The module's glue (coop_hook.cpp; any thread): the picks kept in coop-character-ages.txt beside coop.ini ---
Stored Current();
void SetOn(bool on);
void SetPick(bool child, std::int32_t n);

} // namespace CoopCharAges
