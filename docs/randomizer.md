# The randomizer in co-op: step by step

For 0.11.2 and later. OOTRR comes with the kit: 0.6.0 in 0.11.2, 0.6.1 in 0.11.3. The full reference is [mods.md](mods.md#the-randomizer); this page is only the steps.

The randomizer is **off** until the host turns it on. Each game builds the seed by itself in the background, so every player (the host too) needs **one restart** after their build. Nobody has to download or install anything: do not put OOTRR into `mods` by hand.

## Before you start

- **Everyone on the same kit.** All players update to the same release (Update.bat). Another kit has another OOTRR, and the same seed is then another world (other items in chests). Since 0.11.2 the chat says so: "... has the randomizer seed X built unlike the host's ...".
- **Start the game fresh.** Every player closes the game fully and starts it again with `Start Squadala.bat`. Nobody keeps an old save loaded.

## Host

1. Open the co-op menu (**−** and **+** together, **Home**, or **F1**; on the title screen **−** alone) and go to the **Mods** tab, right after **Multiplayer**. Press **Settings** on the **OoT Remake Randomizer** row.
2. Turn **Randomizer** on. The row that shows "on" in the mod list is only the mod itself; the **Randomizer** switch inside Settings is what turns the randomizer on.
3. Choose the world:
   - **Seed**: leave it empty for a new one, or type a seed word to play a seed again.
   - **Settings**: **default** or **keysanity** (the two presets the kit's OOTRR builds).
   - Or **Settings string**: paste a string from OOTRR's page (it starts with `OOTRR1-`). It replaces Settings.
4. The chat says "Randomizer on: keep the game open until this chat says the seed is ready ...". **Keep the game open** for about a minute.
5. The chat says "Randomizer seed X is ready: RESTART the game now and start a NEW save". Close the game and start it again.
6. Host the session (Multiplayer tab), then start a **New Game**. An old save plays on, but its chests are not this seed's.

## Each guest

1. Start the game fresh and **join from the title screen**. Do not load a save yet.
2. The host's switch and seed reach your game by themselves; there is nothing to set. The chat says "Randomizer on: keep the game open ...". Stay on the title for about a minute while your game builds the seed.
3. The chat says "Randomizer seed X is ready: RESTART the game now ...". Close the game and start it again.
4. Join again from the title. Wait for "Success, load any save.", then load any save: you play the host's file.

Guests can join one after another. Each one waits for its own build and restarts once. A seed is built once and kept, so a guest who built it before skips the wait.

## What the chat lines mean

| Line | Meaning | What to do |
|---|---|---|
| Randomizer on: keep the game open until this chat says the seed is ready ... | The build has started. | Wait. A restart now stops the build. |
| Randomizer seed X is ready: RESTART the game now and start a NEW save ... | Built. This run still plays the base game. | Restart. Host: New Game. Guest: rejoin and load. It repeats every 3 minutes and at each join until you do. |
| Randomizer off: restart the game to play without it | The host turned it off. | Restart. |
| Randomizer: NO SEED - you play the BASE GAME, not the host's seed X ... | Your game could not build it. | Start the game from the kit again (it fetches OOTRR again). If it still fails, the host picks other Settings, or send `mods\oot-remake-randomizer\last-build.txt` with a report. |
| [randomizer] I play without the host's seed: ... | Seen by the host: that guest's build failed, with the reason. | That guest follows the line above. |
| Mods: player N runs WITHOUT the randomizer seed X | That player plays the base game (not built yet, or not restarted). | Wait for their restart. "... runs the host's randomizer seed now" means they are in. |
| Mods: player N has the randomizer seed X built unlike the host's ... | That player is on another kit (another OOTRR), so they play another world. | That player updates to the host's kit. |
| Randomizer seed X was made again for the co-op puppets: RESTART ... | A kit update changed the co-op puppets' files. | Restart. Your save stays. |
| "name" is a mod with its own size table (e.g. a randomizer seed from OOTRR's site) ... | A seed was made on OOTRR's site and put into `mods` by hand. Only you have it, and other players can't be shown. | Make the seed in the Mods tab instead, or turn that mod off. |

## Common mistakes (from player reports)

- **The randomizer was never switched on.** In the logs of reports #96 and #97 the **Randomizer** switch was off in every session. The chests were vanilla because no seed was played. Turn it on inside **Settings**, not just the mod's row.
- **Guests loading old saves.** A guest who joined with an older save loaded brought that save's items into the host's new file (#96, #97; fixed for 0.11.3). Until then: guests close the game, start it fresh, join from the title, and load only after "Success, load any save.". Deleting your save folder does not help, because the items came from another player's running game.
- **Not restarting, or restarting too early.** The seed plays only after the restart that follows "is ready". A restart before that line stops the build; it starts again at the next start.
- **No new save.** The host starts a New Game after the restart.
- **Different kits.** A guest on another release (for example 0.11.1 with OOTRR 0.5.0 next to a 0.11.2 host) builds another world from the same seed word.
- **After a kit update that brings another OOTRR** (0.11.1 to 0.11.2, 0.11.2 to 0.11.3): the new OOTRR cannot use the old seeds, so the kit removes them. Each game builds the seed again at its next start, then needs one more restart, and the host needs a New Game.
- **Even a randomized seed leaves many spots vanilla.** OOTRR does not shuffle everything yet, so some vanilla chests are normal.
