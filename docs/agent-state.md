# Agent state: what is done, open and waiting

Public coordination state for anyone working on this repository, including the maintainer's agent. It holds facts
only: what is done, what is open, and what we wait on. The handoff for the maintainer's agent is in
[agent-handoff.md](agent-handoff.md). Its address list is in [addresses.md](addresses.md).

Last updated: 2026-11-22.

## Integration and pull request #315

- Pull request [#315](https://github.com/daurosk/oot-remake-multiplayer/pull/315) (the play-test kit, the agent
  handoff, the address index and a build fix for the llvm-mingw toolchain) is reviewed and merged into the 0.11.12
  integration branch as authored; our fixes sit on top. Its branch `test-kit-0117` is no longer the integration branch.
- The play-test kit has two layers: the live kit ([playtest.md](playtest.md)) and the play-test bridge with one MCP
  server per game ("Playing through the bridge" there). The rules for an agent tester are in [AGENTS.md](../AGENTS.md),
  section "Play-testing the game"; this page does not repeat them.
- Nothing is merged to main without the maintainer's go.

## Work in progress: Character Select pick per age

Branch `char-ages`, local and not yet pushed.

- Done: the logic in `lichii/hook/coop_char_ages.h`, with its tests in `tests/lichii/char_ages_test.cpp`; the frame
  glue in `coop_hook.cpp` (`CharAgesFrame`, file `coop-character-ages.txt`); the Settings rows in the pause menu
  ("Character by age", "As a child", "As an adult").
- Open: handle the new request kind in `hookshot/src/coop_pause.cpp`; read the pickable characters from the mod's
  `characters.json`, filtered to the files co-op knows; pause menu tests; docs in [mods.md](mods.md) and
  [puppet.md](puppet.md); a game check (a pick per age switches at the Master Sword); then a pull request.
- Wish: the picks belong on the Characters screen. Today the picker sets the current age's pick; the Settings rows
  set either age.

## Character Select v0.2.2 in the kit

- The mod author released v0.2.2. It has the same characters as v0.2.1, but it ships as a zip that builds the mod on
  the player's own PC from the player's own game files. Its tool needs the game's romfs and four Python packages.
- Done in the 0.11.12 line (pull request #312): the kit installs v0.2.2 by running that tool on the kit's own romfs. The kit's pinned version
  (`CHARACTER-SELECT.txt`) changes with it. Tested on the kit's romfs: 2287 files in 155 s, no errors.
- The kit's script runs the mod's tool on the player's own game files; the NPC and default parts depend on it.

## Open report: Zelda's hair after a spin attack

- Report: an adult Zelda's back hair flips in front of her face after a spin attack.
- Not reproduced in game yet: charged, quick and running spins, with the Master and Kokiri swords, on flat ground.
- Cause, as far as the code shows: her cloth config lacks the guard that the NPC hair code has. The guard resets
  the cloth when the head-to-hair angle goes past 80 degrees. Her back hair's max distance is 60.
- The fix belongs to the mod author: add the guard or lower the range. We wait for exact repro steps before we
  report it.

## Mod author material (models and voices)

Material for a submission to the mod author and to the Squadala community. Nothing is posted publicly yet.

- A. Cow run animation: a fix in the rig tool's leg plan (skip the leg plan when the hip and knee pivot error is
  over 0.06 m), and the Cow and Wolfos added to the legs-as-they-are list, with the Cow's front legs set absolute.
- B. New characters: Stalfos, Stalchild, Stalchild Big and Dinolfos. Their rig needs a neck rename and hair bones.
- C. Voice rows for more characters: Sheik, Impa, Ganondorf (also as a child and as Phantom Ganon), Darunia, King
  Zora, Nabooru, Talon, Ingo, Dampe, Biggoron, the Gerudo, Goron, Zora and Hylian types, Skull Kid, the Twinrova
  sisters, the Cow and Kaepora.
- D. Direct enemy voices (for example Dark Link, Stalfos and Iron Knuckle) that skip the voice conversion step.
- Possible co-op issue: puppets may speak with the viewer's character voice. To check.

## Open reports for co-op

- Customize colours do not reach Character Select characters on the player's own model. Co-op dresses only Link's
  own eyes and hair: `LinkOnly` in `lichii/hook/coop_char_colours.h` gives nothing when a Character Select model is
  shown (`ShownCharacter() != 0`, called from `DressCharacters` in `hookshot/src/coop_hookshot.cpp`).
- The Cow's run animation (see A above).

## Deferred engine items

Listed in [boss-multiplayer.md](boss-multiplayer.md), section "Whole-game survey". They are: the Redead freeze, the
Meg decoys, the Dead Hand hands, presence-gated placements, and the Volvagia rocks.

## Waiting on

- Exact repro steps for the Zelda hair report.
- The maintainer's go before any public post or any change to main.
