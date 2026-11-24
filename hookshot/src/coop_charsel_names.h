// SPDX-License-Identifier: GPL-3.0-or-later
// Readable names for Character Select's unnamed characters (menu-customize-01113, audit item 14).
//
// OoT Character Select (snaildev99, MIT) names its game characters (Saria, Rauru, ...) but gives the game's unnamed
// townsfolk their model ids ("Gerudo_GEN_002", "Hylia_MN_005", "Kokiri_WC_001") in his characters.json and his
// picker. Co-op's Settings rows "Character by age" list them by that name (coop_pause.cpp FeedCharacterAges), and the
// kit's merge renames them in his picker (tools/puppet_pack/charsel_names.py). This is the same rule as that file,
// so a character has one name everywhere; tests/hookshot/charsel_names_test.cpp checks the names both give.
//
// A name <Race>_<CODE>_<digits> (an optional _<Word> after it) is a model id; any other name is his and stays.
//   Hylia M.. -> Townsman, W.. -> Townswoman, MO -> Old man, WO -> Old woman, MS -> Soldier
//   Kokiri MC -> Kokiri boy, WC -> Kokiri girl
//   any other race -> the race's word (Hylia -> Hylian); a trailing word -> "<kind> <word in lower case>"
// A kind with more than one character is numbered 1, 2, ... in the list's order; a kind of one has no number.
#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace CoopCharselNames {

namespace detail {
inline bool Upper(char c) { return c >= 'A' && c <= 'Z'; }
inline bool Letter(char c) { return Upper(c) || (c >= 'a' && c <= 'z'); }
inline bool Digit(char c) { return c >= '0' && c <= '9'; }
// The run of characters from `at` for which `ok` holds; its end.
template <class F>
std::size_t Run(const std::string& s, std::size_t at, F ok) {
    while (at < s.size() && ok(s[at])) ++at;
    return at;
}
}  // namespace detail

// The kind (the readable name before its number) of a model id, or nullopt for a name of his own.
inline std::optional<std::string> KindOf(const std::string& name) {
    using namespace detail;
    const std::size_t race_end = Run(name, 0, Letter);
    if (race_end == 0 || race_end >= name.size() || name[race_end] != '_') return std::nullopt;
    const std::size_t code_end = Run(name, race_end + 1, Upper);
    if (code_end == race_end + 1 || code_end >= name.size() || name[code_end] != '_') return std::nullopt;
    const std::size_t digits_end = Run(name, code_end + 1, Digit);
    if (digits_end == code_end + 1) return std::nullopt;
    std::string word;
    if (digits_end < name.size()) {
        if (name[digits_end] != '_') return std::nullopt;
        const std::size_t word_end = Run(name, digits_end + 1, Letter);
        if (word_end == digits_end + 1 || word_end != name.size()) return std::nullopt;
        word = name.substr(digits_end + 1);
        for (char& c : word) {
            if (Upper(c)) c = static_cast<char>(c - 'A' + 'a');
        }
    }
    const std::string race = name.substr(0, race_end), code = name.substr(race_end + 1, code_end - race_end - 1);
    const bool man = code[0] == 'M', woman = code[0] == 'W';
    std::string kind;
    if (race == "Hylia" && (man || woman)) {
        if (code.size() > 1 && code[1] == 'O') {
            kind = man ? "Old man" : "Old woman";
        } else if (code == "MS") {
            kind = "Soldier";
        } else {
            kind = man ? "Townsman" : "Townswoman";
        }
    } else if (race == "Kokiri" && (code == "MC" || code == "WC")) {
        kind = man ? "Kokiri boy" : "Kokiri girl";
    } else {
        kind = race == "Hylia" ? "Hylian" : race;
    }
    return word.empty() ? kind : kind + " " + word;
}

// Each name of the list (in its order) as shown: model ids as their kind (numbered when the kind has more than one),
// the others unchanged.
inline std::vector<std::string> Readable(const std::vector<std::string>& names) {
    std::vector<std::optional<std::string>> kinds;
    std::map<std::string, int> total, seen;
    for (const auto& n : names) {
        kinds.push_back(KindOf(n));
        if (kinds.back()) ++total[*kinds.back()];
    }
    std::vector<std::string> out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (!kinds[i]) {
            out.push_back(names[i]);
            continue;
        }
        const std::string& k = *kinds[i];
        const int n = ++seen[k];
        out.push_back(total[k] == 1 ? k : k + " " + std::to_string(n));
    }
    return out;
}

}  // namespace CoopCharselNames
