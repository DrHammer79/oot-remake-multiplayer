// The OoT Remake randomizer as the mod loader's first built-in mod
// (docs/mods.md "The randomizer"). The randomizer itself is OOTRR
// (github.com/REALAZK/ootrr, GPL-3.0-or-later since 0.5.0): a program that makes a seed and builds a
// RomFS mod from it and the player's own game files. This file drives it:
//
// - mods\oot-remake-randomizer\mod.json (written here when missing) holds the
//   switch and the settings: the seed and the preset are "shared", so in co-op
//   the host's are everyone's (MOD_SETTINGS);
// - each seed is built once, by `ootrr generate` + `ootrr build --host
//   hookshot`, into its own mod folder mods\randomizer-<seed>; the Mods tab's
//   list turns that one on and every other seed off. Hookshot reads the mods
//   at its start, so a new seed plays after a restart of the game, never after
//   a game build;
// - nothing of the game is in this repository or the kit: OOTRR reads the
//   player's own game files, by default the ones the kit's build unpacked
//   beside the game (<game>\romfs). The kit brings OOTRR (<game>\tools\ootrr,
//   hookshot\OOTRR.txt), so a player sets nothing up.
//
// The rules (what to build, which folders to switch, the command lines) are
// plain functions for the tests; Rando runs them in the game.
#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <map>
#include <vector>

#include "coop_mods.h"

namespace CoopRando {

inline constexpr const char* kFolder = "oot-remake-randomizer";  // the switch and the settings
inline constexpr const char* kId = "oot-remake-randomizer";
inline constexpr const char* kSeedPrefix = "randomizer-";          // + the seed word: one built seed
inline constexpr const char* kSeedFile = "coop-seed.txt";          // OOTRR writes it into a Hookshot mod
inline constexpr const char* kPresetFile = "coop-rando.txt";       // we write the preset it was built with
inline constexpr const char* kLogFile = "last-build.txt";          // in kFolder: OOTRR's output
inline constexpr const char* kTablesFile = "coop-tables.txt";      // in a seed: the size tables it was built beside
inline constexpr const char* kPresets[] = {"default", "easy", "open_world", "keysanity", "full_shuffle"};
inline constexpr const char* kPresetsFile = "presets-ok.txt";   // in kFolder: which presets the installed OOTRR builds

// The control mod's mod.json.
std::string ManifestText();

// The seed as OOTRR writes it into coop-seed.txt and as the folder is named: ASCII letters, digits, '_' and '-',
// at most 63, "seed" when nothing is left.
std::string SeedWord(std::string_view seed);
// The seed's folder: "randomizer-<word>", with "-<preset>" for a preset other than "default".
std::string FolderFor(std::string_view seed, std::string_view preset);
// A new seed for a host who has none: 8 letters and digits from `entropy`.
std::string NewSeed(std::uint64_t entropy);

// A seed built before, as found in the mods folder.
struct Built {
    std::string name;    // the mod's folder name (randomizer-...)
    std::string seed;    // coop-seed.txt
    std::string preset;  // coop-rando.txt; empty when not ours
    std::string tables;  // coop-tables.txt (TablesStamp); empty when built before it
};
std::vector<Built> Scan(const std::filesystem::path& mods_root);

struct Want {
    bool on = false;
    std::string seed;    // empty: none yet
    std::string preset;
};

struct Plan {
    // use: only switch the folders (on / off). refresh: the seed is built, but beside other size tables than now
    // (a puppet pack made or made again after it): it is built again and only its size table is taken.
    enum class Do { nothing, build, refresh, use } what = Do::nothing;
    std::string folder;                 // the seed's folder (build / use)
    std::vector<std::string> on;        // folders the list turns on (first wins: the seed's)
    std::vector<std::string> off;       // folders the list turns off (every other seed; all when the randomizer is off)
    std::string why;                    // one plain line for the log and the tab
};
// What the game must do for `want`, given the seeds built and which mods are on now (Loader rows). `tables`: the
// TablesStamp of the other mods' size tables now; a built seed with another one is refreshed (none: not compared).
Plan Decide(const Want& want, const std::vector<Built>& built, const std::vector<CoopMods::Row>& rows,
            const std::optional<std::string>& tables = std::nullopt);

// OOTRR's command lines (arguments after the program).
std::vector<std::string> GenerateArgs(const std::string& preset, const std::string& seed,
                                      const std::filesystem::path& seed_file);
std::vector<std::string> BuildArgs(const std::filesystem::path& seed_file, const std::filesystem::path& game_folder,
                                   const std::filesystem::path& mods_root, const std::string& folder);
// The game uses one resource size table, the one of the mod that wins (the seed), so the seed's must hold every
// other mod's entries. OOTRR merges those only from mods/<name>/romfs, and the puppet pack is deeper
// (mods/contents/<title>/coop-puppet): without its entries the game aborts while it loads. So the seed is built in
// `stage`, where this copies the size tables (romfs/System/Resource/*.rsizetable.zs) of every other mod under
// `mods_root` the game loads next time, each into <stage>/<n>/romfs/System/Resource. Left out: seeds
// (randomizer-*), the control folder, a folder whose name starts with '-' or '_' (Longshot skips those) and a row
// switched off. The count of tables copied; -1 when one could not be copied.
int StageSizeTables(const std::filesystem::path& mods_root, const std::vector<CoopMods::Row>& rows,
                    const std::filesystem::path& stage);
// Those size tables (the same choice as StageSizeTables), in the order found.
std::vector<std::filesystem::path> OtherSizeTables(const std::filesystem::path& mods_root,
                                                   const std::vector<CoopMods::Row>& rows);
// What a seed was built beside, as kept in its coop-tables.txt: each table's path under `mods_root`, length and a
// hash of its bytes, sorted. A seed is only as good as its size table: the game reads that one table (the seed sorts
// first), so a puppet pack made after the seed has no sizes there and its actors cannot load (the other players
// invisible with the randomizer on).
std::string TablesStamp(const std::filesystem::path& mods_root, const std::vector<std::filesystem::path>& tables);
// The mod whose resource size table this run of the game reads, when it is none of co-op's own: any mod with its
// own size table above the puppet pack (player report #48: a seed made on OOTRR's own page, put into mods by hand). Longshot serves one size table, the winning
// mod's: with the Mods list (ExeReadsList, `list_order`) the first row of `rows` (the puppet pack is never listed and
// sits under every listed mod), else Hookshot's own order (by name, the last wins; the pack is
// contents/0400486024DEC000/coop-puppet). Only a mod the game indexed at its start (Row::on_now) and that holds
// romfs/System/Resource/*.rsizetable.zs counts. That mod's table was made without the pack's entries, so the puppets
// cannot load (invisible, or a crash when the map loads). Empty when the winner is one of our seeds, the control
// folder or the pack. (Whether its table happens to hold the pack's entries is not looked at: the module reads no
// zstd. A seed of ours is built beside every other mod's table, StageSizeTables.)
std::optional<std::string> OutsideSizeTable(const std::filesystem::path& mods_root,
                                            const std::vector<CoopMods::Row>& rows, bool list_order);
// This game's chat line for it (it repeats when joining), and the line the other players get (at most kTellChars).
std::string OutsideLine(std::string_view mod);
std::string OutsideTellLine(std::string_view mod);

// The Windows command line of a program and its arguments (each quoted as CommandLineToArgvW reads it back).
std::wstring CommandLine(const std::filesystem::path& program, const std::vector<std::string>& args);

// Runs a program with no window to its end, its output appended to `log_file`; its exit code, -1 when it could not
// start, kStopped when it ran longer than `limit` or `stop` turned true (it is then ended) (Rando::Hooks::run).
// Windows and Linux.
inline constexpr int kStopped = -2;
int RunProgram(const std::filesystem::path& program, const std::vector<std::string>& args,
               const std::filesystem::path& log_file, std::chrono::milliseconds limit, const std::atomic<bool>* stop);

// OOTRR's program name on this system: ootrr.exe (Windows) or ootrr.
const char* OotrrName();
// Where OOTRR is: the "ootrr" setting when set, else <game>\tools\ootrr (where the kit puts it, hookshot\OOTRR.txt)
// or <mods>\oot-remake-randomizer.
// Where the game's extracted files are: the "game_folder" setting when set, else OOT_ROMFS, else <game>\romfs (the
// files the kit's build unpacked: OOTRR only reads them). A folder counts when it has Pack and System.
std::optional<std::filesystem::path> FindOotrr(const std::string& setting, const std::filesystem::path& game_dir,
                                               const std::filesystem::path& mods_root);
std::optional<std::filesystem::path> FindGameFolder(const std::string& setting, const std::filesystem::path& game_dir);

// What to do about a failed `ootrr build`, from its output: game files of another version than 1.1.1 (OOTRR says
// "the game folder has no ...") say so plainly; anything else is empty (the caller gives the exit code).
std::string BuildFailureHint(std::string_view ootrr_output);
// Which presets an OOTRR builds (player report #16: OOTRR 0.5.0's own full_shuffle preset exits 1, "This build cannot
// apply these settings yet"). Tried once per OOTRR (its size and write time: `key`), kept in kPresetsFile as
// "ootrr <key>" then "<preset> ok" or "<preset> bad" lines.
struct PresetCache {
    std::string key;
    std::map<std::string, bool> ok;
};
PresetCache ReadPresetCache(std::string_view text);
std::string PresetCacheText(const PresetCache& cache);
std::string OotrrKey(const std::filesystem::path& ootrr);
// The control mod's manifest offering only `presets` (default always, first).
std::string ManifestText(const std::vector<std::string>& presets);
// The last `n` non-empty lines of OOTRR's output (for longshot_log.txt when it fails), oldest first, without carriage returns.
std::vector<std::string> LastLines(std::string_view text, std::size_t n);
// The tab's line for OOTRR's own failure: `head` and where its output is, by file name (player report: a full path
// was cut off in the tab). The full path goes to the chat and the log.
std::string SeeLogLine(const std::string& head);
// A line for the co-op chat (others may see a screenshot): the user's profile folder (Users<name>, /home/<name>)
// masked as <user>, as collect-logs does, and cut to `max_chars` bytes at a whole UTF-8 character.
std::string ChatSafe(std::string line, std::size_t max_chars = 100);
// Whether a probe's run means OOTRR refused the preset: a real exit code above 0 with something said. A stop, a
// timeout, a program that did not start (kStopped, -1) or a silent exit are not known: never cached as refused.
bool PresetRefused(int code, std::string_view output);
// At most the last `max_bytes` of a file (OOTRR's output can be long): empty when it cannot be read.
inline constexpr std::size_t kTailBytes = 64 * 1024;
std::string ReadTail(const std::filesystem::path& file, std::size_t max_bytes);
// OOTRR's own error lines (player report #16: "This build cannot apply these settings yet: ..."): the first `n` lines
// that say error, cannot, can't or failed; none: the last `n` lines.
std::vector<std::string> ErrorLines(std::string_view text, std::size_t n);
// The chat line a guest sends when it cannot build the host's seed (Hooks::tell_others): at most kTellChars bytes
// (a chat line's limit), cut at a whole UTF-8 character.
inline constexpr std::size_t kTellChars = 200;
std::string TellLine(std::string_view why);
// The guest's own loud chat line when the host's seed could not be made (#59: guests played the base game without
// knowing it): no seed, the base game, why, and what to do. For this game's chat only (`why` may name a local file).
std::string NoSeedLine(std::string_view seed, std::string_view why);

// What the tab and the chat say when the randomizer is switched (by the player, or the host's switch reaching a guest).
// A seed plays only after a restart, and only from a new save (a save from before has the items of the plain game).
std::string SwitchLine(bool on);
// The chat line once the seed is ready to play (built, or switched on): restart now. Said again when this game joins
// a host and every kRepeat until the restart.
std::string ReadyLine(std::string_view seed);
// The chat line when a seed in use was made again for the size tables of now (the co-op puppets'): restart.
std::string RefreshLine(std::string_view seed);
inline constexpr std::chrono::seconds kRepeat{180};

// In the game: watches the randomizer's settings and builds a seed in the background when they change.
class Rando {
public:
    struct Hooks {
        std::function<void(const std::string&)> log;
        std::filesystem::path game_dir;
        // Runs a program to its end (any thread); its output appended to `log_file`. The exit code, or -1 when it
        // could not start. Real processes in the game, fakes in the tests.
        std::function<int(const std::filesystem::path& program, const std::vector<std::string>& args,
                          const std::filesystem::path& log_file)>
            run;
        std::function<std::uint64_t()> entropy;
        // A guest whose game could not build the host's seed tells the others, so the host's chat names the player
        // and why: one chat line (any thread). False when it could not be sent (not connected): tried again at the
        // next Recheck. Empty: said only in this game.
        std::function<bool(const std::string& line)> tell_others;
        // The clock for repeating the restart line (tests); empty: steady_clock.
        std::function<std::chrono::steady_clock::time_point()> now;
        // Try every preset once per OOTRR before its first build (#16): the Settings row then offers only the ones it
        // builds. (Off in tests that count OOTRR's runs.)
        bool probe_presets = true;
        // The probe's runs (a shorter time limit than a build's: ~90 s each); empty: `run`.
        std::function<int(const std::filesystem::path& program, const std::vector<std::string>& args,
                          const std::filesystem::path& log_file)>
            probe_run;
    };
    Rando(CoopMods::Loader& loader, Hooks hooks);
    ~Rando();  // waits for a build that runs
    Rando(const Rando&) = delete;
    Rando& operator=(const Rando&) = delete;

    // Once, after the Loader started: writes the control mod when missing, switches folders, and listens.
    void Start();
    // Whether a build runs now, and the last line said (for the tab).
    bool Building() const;
    std::string Status() const;
    // Looks at the settings again: after the host's values came or went (a host without the randomizer changes no
    // value of ours, so no setting call comes).
    void Recheck() {
        Check();  // first: drops a line no longer true (the host left, another seed)
        SayOutside();  // joined: the others hear about an outside size table too
        TellOthers();
        RepeatRestart();  // joined: a restart still due is said again, in the chat the player now looks at
    }
    // Whether the puppet pack's actors must not be made in this run: the seed it plays was built beside other size
    // tables than there are now, so the size table the game read at its start may lack the pack's entries, and making
    // a puppet then crashes the game when the map loads (lane1 2026-11-15: a tunic model's buffer too small). Set at
    // Start until the seed is checked (a refresh); cleared when its table was right after all, kept until the restart
    // when it was made again (RefreshLine) or could not be. The other players show as their fairies meanwhile.
    // Also kept for the whole run when another mod's size table wins (OutsideSizeTable).
    bool HoldPack() const { return hold_pack_.load() || outside_.load(); }
    // The chat lines since the last call (SwitchLine when the switch changed).
    std::vector<std::string> TakeLines();
    // Waits until no build runs (tests, exit).
    void Wait();

private:
    Want Wanted(bool* seed_locked) const;
    void Check();                         // settings -> Plan -> act (a setting's thread)
    void Switch(const Plan& plan);        // the list's switches for a plan that builds nothing
    void Work(Plan plan, Want want);      // the worker: builds until the settings stop changing
    void Build(const Plan& plan, const Want& want);
    void Say(std::string line);
    void TellOthers();                    // sends tell_ and outside_tell_ (Hooks::tell_others)
    void SayOutside();                    // outside_line_ into the chat again, outside_tell_ armed (mutex_ not held)
    void SayRestart(std::string line);    // a restart line for the chat, kept to repeat (mutex_ not held)
    void RepeatRestart();                 // restart_ into the chat now (mutex_ not held)
    std::chrono::steady_clock::time_point Now() const;
    std::string CurrentTables() const;    // TablesStamp of the other mods' size tables now
    // The presets the OOTRR at `ootrr` builds, tried and kept in kPresetsFile when it is new (#16); the manifest
    // offers only those.
    std::map<std::string, bool> ProbePresets(const std::filesystem::path& ootrr, const std::filesystem::path& control);

    CoopMods::Loader& loader_;
    const Hooks hooks_;
    mutable std::mutex mutex_;
    std::thread worker_;
    bool building_ = false;    // a worker runs (mutex_)
    bool again_ = false;       // the settings changed while it ran (mutex_)
    std::string last_failed_;  // the folder whose build failed last (mutex_)
    std::string status_;
    bool on_now_ = false;      // the switch as the game started (mutex_)
    bool on_said_ = false;     // the switch as last said (mutex_)
    std::vector<std::string> lines_;  // mutex_
    std::string probed_key_;               // the OOTRR the presets were tried for in this run (mutex_)
    std::map<std::string, bool> probed_;  // what that found (mutex_)
    bool log_in_chat_ = false;        // OOTRR's log path said in the chat this run (mutex_)
    std::string tell_;                // a line for the others not sent yet (mutex_): never a path
    std::string tell_seed_;           // the host's seed it is about (mutex_)
    std::atomic<bool> hold_pack_{false};  // HoldPack
    std::atomic<bool> outside_{false};    // another mod's size table wins this run (HoldPack)
    std::string outside_mod_;         // that mod (mutex_): empty when none
    bool outside_tell_ = false;       // OutsideTellLine not sent yet (mutex_)
    std::string restart_;             // the restart line until this game restarts (mutex_): empty when none
    std::chrono::steady_clock::time_point restart_said_{};  // when it was last put in the chat (mutex_)
};

}  // namespace CoopRando
