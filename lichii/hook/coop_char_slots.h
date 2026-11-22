// A pick in Snail's Character Select page whose model is not installed (owner, 0.11.9: his NPC characters are an
// opt-in download; without it his full picker stays). The mod's picker stores the pick (CharacterMenu_SelectedCharacter,
// the mod's own number of the character) and its model / cloth / voice hooks build a path from that number every time
// the game loads the player: a number whose files are not there would load missing files. So the hook reads the pick
// every frame and puts a dead one back at once (the last installed pick, or 0: Link), with one chat line.
// Installed: what the puppet pack found (tools/puppet_pack/build.py find_characters keeps only numbers whose
// romfs/Model/Player_<age>_C<file> file is there; coop_puppet.json "characters": {"numbers": ...}).
// Another player's pick we do not have: their puppet is Link already (Planner::ActorFor finds no file), and the
// same line is said once per player number (not per frame), again only when they pick another missing one.
// Header only, standard C++ only.
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace CoopCharSlots {

// The chat line (plain words; the kit's start shows the NPC line and N switches the download on).
inline std::string MissingLine(bool own, const std::string& who = {}) {
    if (own) {
        return "This character is not downloaded: press N at the kit's start to get Snail's NPC characters.";
    }
    return (who.empty() ? std::string("A player") : who) +
           " plays a character you have not downloaded, shown as Link (press N at the kit's start to get Snail's NPC "
           "characters).";
}

// `pick` (the mod's stored number) can be loaded: 0 or less is Link; else the pack must have its file.
inline bool Installed(std::int32_t pick, const std::map<std::uint16_t, std::uint16_t>& files) {
    return pick <= 0 || (pick <= 0xFFFF && files.count(static_cast<std::uint16_t>(pick)) != 0);
}

// Our own pick.
class Own {
public:
    struct Step {
        std::optional<std::int32_t> set;   // the number to store at once in place of the pick
        bool say = false;                  // say MissingLine(true) now
    };
    // Each frame with the pick read (nothing: no such flag, no mod).
    Step Check(std::optional<std::int32_t> pick, const std::map<std::uint16_t, std::uint16_t>& files) {
        Step s;
        if (!pick) return s;
        if (Installed(*pick, files)) {
            last_good_ = *pick;
            told_ = 0;
            return s;
        }
        s.set = Installed(last_good_, files) ? last_good_ : 0;
        s.say = told_ != *pick;   // (the same missing pick again at once, as the page re-stores it: no second line)
        told_ = *pick;
        return s;
    }

private:
    std::int32_t last_good_ = 0;
    std::int32_t told_ = 0;
};

// Other players' picks: true when the line is to be said for `number` now.
class Others {
public:
    static constexpr std::size_t kNumbers = 17;   // player numbers 1..16
    bool Tell(std::uint8_t number, std::uint16_t character, const std::map<std::uint16_t, std::uint16_t>& files) {
        if (number >= kNumbers) return false;
        if (Installed(character, files)) {
            told_[number] = 0;
            return false;
        }
        if (told_[number] == character) return false;
        told_[number] = character;
        return true;
    }

private:
    std::array<std::uint16_t, kNumbers> told_{};
};

}  // namespace CoopCharSlots
