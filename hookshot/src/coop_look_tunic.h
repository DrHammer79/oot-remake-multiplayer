// SPDX-License-Identifier: GPL-3.0-or-later
// Your own Link's tunic and gear in other colours while the game runs
// (docs/puppet.md, "Changing your colour in the game" and "The Link
// customizer").
//
// The colours are in textures (reports puppet-colors-2: no material
// parameter shows). Nothing of the game is written: Longshot's texture
// overrides (patch 0020, hos_coop_texture.h) have its renderer draw a texture
// the game made with other texels, while the game's memory keeps its own.
// Same format, same size, nothing else moves.
//
// The puppet pack has those texels per colour (tools/puppet_pack/look_live.py:
// coop_look/<colour>.bin, green being the game's own). A texture is one the
// game made (Longshot lists them) whose storage holds a colour's texels
// exactly. The game copies texels into its texture memory with no file or name
// in front (lane1, 2026-11-14), so the puppets' copies of the same texels carry
// a mark (look_live.py mark, CoopLookParts::Mark) and are left alone.
//
// Nothing here runs on the game's thread: Wardrobe::Step is the worker's.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace CoopLookTunic {

struct Texture {
    std::string name;            // in the game: "Angin_Tile_A_Alb"
    std::uint64_t offset = 0;    // of the texels in the texture's file
    std::vector<std::uint8_t> texels;
};
// One colour's file: every tunic texture, in the pack's order.
using Texels = std::vector<Texture>;

// The first bytes of a texture's texels, compared before anything else.
inline constexpr std::size_t kHead = 64;
// How much larger than its texels a texture's storage may be (its layout
// rounded up): only storages up to this much larger are compared.
inline constexpr std::uint64_t kStorageSlack = 0x10000;
// A texture file's header is at most this long (the game's are 4096 bytes).
inline constexpr std::uint64_t kMaxOffset = 0x10000;

// A coop_look file. Nothing, and why, for one that is cut short, of another
// version, or with a texture this code cannot place (no room for a header,
// fewer than kHead bytes).
std::optional<Texels> Parse(const std::vector<std::uint8_t>& file, std::string* why = nullptr);
// The file of these texels (Parse reads it back).
std::vector<std::uint8_t> Pack(const Texels& texels);
// Two colours of one pack: the same textures in the same order (a texture of
// another size in one of them is not drawn: Draw).
bool SameLayout(const Texels& a, const Texels& b);

// The game's textures and Longshot's overrides of them (hos_coop_texture.h).
struct Live {
    std::uint64_t address = 0;   // its storage: where its texels start
    std::uint64_t size = 0;
    std::uint64_t serial = 0;    // new each time a texture is made
};
class Textures {
public:
    virtual ~Textures() = default;
    // The textures the game has whose storage is size_min to size_max bytes;
    // *generation changes whenever one is made or finalized.
    virtual std::vector<Live> List(std::uint64_t size_min, std::uint64_t size_max, std::uint64_t* generation) const = 0;
    // The game's memory, read only. False when any byte is not there.
    virtual bool Read(std::uint64_t address, void* out, std::size_t size) const = 0;
    // The renderer draws these bytes in place of [address, address + size)
    // of the texture of that serial; `bytes` null: as the game's memory
    // holds them again. False when the game no longer has that texture.
    virtual bool Override(std::uint64_t address, std::uint64_t serial, const void* bytes, std::size_t size) = 0;
};

// A texture of your Link's, the colour the game's memory holds there and the
// colour it is drawn in.
struct Place {
    std::size_t texture = 0;
    std::uint64_t address = 0;
    std::uint64_t serial = 0;
    std::uint64_t size = 0;   // the bytes compared and drawn: the texels, at most the storage
    std::shared_ptr<const Texels> held;    // in the game's memory
    std::shared_ptr<const Texels> drawn;   // on screen: held, or an override's
    bool overridden = false;
};

struct Search {
    std::vector<Place> places;
    std::size_t listed = 0;   // the game's textures of a fitting size
    std::size_t unread = 0;   // ... whose storage could not be read
    std::size_t heads = 0;    // ... whose first kHead bytes are a colour's, the rest not (a copy's mark, a painting)
};
// Every texture of `colours` the game has made: its storage holds the
// colour's texels exactly, the whole image (the pack's texels are the image's
// own size, as the game's storage; a copy's mark is its last byte); the first
// colour that holds them is the one held.
Search Find(const Textures& textures, const std::vector<std::shared_ptr<const Texels>>& colours);
// The storage sizes a texture of `colours` may have: [first, second] (0, 0 when
// they have no texels).
std::pair<std::uint64_t, std::uint64_t> Sizes(const std::vector<std::shared_ptr<const Texels>>& colours);
// Find among `listed` only (textures the game has, of those sizes).
Search Find(const Textures& textures, const std::vector<Live>& listed,
            const std::vector<std::shared_ptr<const Texels>>& colours);
// Draws the place in `colour`: an override, or none when the game's memory
// holds that colour already. False when Longshot refused it (nothing changed).
bool Draw(Textures& textures, Place& place, const std::shared_ptr<const Texels>& colour);

// How long a texture the game made is looked at again for a colour's texels
// (the game may copy them in after it makes the texture); after that only a
// Kick or another colour looks at it again.
inline constexpr std::uint64_t kSettleMs = 5000;

// Keeps your Link's tunic (or gear) in the colour asked for.
//
// The game makes your Link's textures again now and then (a scene, a
// cutscene, a model streamed in again): the new texture has another serial
// and Longshot took the old one's override away, so it shows the game's
// colour until the wardrobe draws it again. Step is cheap while nothing
// changes (one generation query), so the worker calls it often (every
// kPollMs) and the game's colour shows for a frame or two at most. A texture
// found once keeps its place, and its override, for as long as the game has
// it: a texture is never drawn twice in one colour (v0.11.0 player logs:
// "your Link's tunic is orange now" 47 times).
class Wardrobe {
public:
    struct Host {
        Textures* textures = nullptr;
        // A colour's file from the puppet pack ("blue"); nothing when the
        // pack has none.
        std::function<std::optional<std::vector<std::uint8_t>>(const std::string&)> file;
        std::function<void(const std::string&)> log;
        // The game's own look's file name ("green" for the tunic) and what the
        // log calls the textures ("tunic").
        std::string base = "green";
        std::string what = "tunic";
        // The colour the game's files give your Link at the start, when not the
        // base: the kit put that colour's files in (LICHII_COOP_SELF_TUNIC),
        // so its texels are what the game's memory holds.
        std::string start;
    };

    explicit Wardrobe(Host host) : host_(std::move(host)) {}
    // The colour to wear from now on: "green" (the game's own) or a colour of
    // the pack. Any thread.
    void Wear(std::string colour);
    // One round of the work, every kPollMs or so: `now_ms` on any steady
    // clock. One thread.
    void Step(std::uint64_t now_ms);
    // Look at the game's textures again at the next Step (the pause screen
    // closed: the equipment may have changed). Any thread.
    void Kick();
    // Whether Step has anything to do (a colour other than the game's was
    // asked for at some time). Any thread.
    bool Busy() const;
    // Takes away every override this wardrobe set (the module stops). Step's
    // thread, or once it no longer runs.
    void Clear();

    const std::vector<Place>& places() const { return places_; }   // Step's thread

private:
    std::shared_ptr<const Texels> Load(const std::string& colour);

    Host host_;
    mutable std::mutex mutex_;
    std::string wanted_;                      // guarded
    bool busy_ = false;                       // guarded
    bool kicked_ = false;                     // guarded
    // Step's thread only:
    std::string asked_;                       // the wanted colour of the last round
    std::shared_ptr<const Texels> green_, start_, target_;
    std::string target_name_;
    std::string refused_;                     // a colour the pack does not have (said once)
    std::vector<Place> places_;
    bool look_now_ = true;
    bool looked_ = false;
    std::uint64_t generation_ = 0;
    std::map<std::uint64_t, std::uint64_t> seen_;   // serial -> when first looked at (no colour's texels then)
    bool pending_ = false;                    // a texture of seen_ younger than kSettleMs
    std::string said_drawn_;                  // the colour last said drawn
    std::size_t said_again_ = 0;              // "made again" lines said
    bool said_nothing_found_ = false;
    std::string said_;                        // the last "no texture" line (said again when it changes)
    int said_count_ = 0;
};

}  // namespace CoopLookTunic
