# Live play-testing (two games, step by step)

The unit tests prove the sync logic; only a game run shows what two players actually see. The two-game runner
(`tools/autotest/hookshot.py run`, [testing.md](testing.md)) plays a whole script blind. The live kit keeps both games
open and takes one step at a time, so a tester (a person or Claude Code) acts, looks at both games, and decides the
next step: the way agent-driven play of other games works, with the game handing over its state as data.

Maintainer only, like the rest of `tools/autotest`: it needs two Hookshot test copies with the game.

## The pieces

| Piece | What it does |
| --- | --- |
| `tools/autotest/live.py start` | Starts the server and both games (the runner with `--keep-open`), plays each game's boot script (e.g. warp to a dungeon, past the title), and leaves them running. Each game's save is restored to its baseline first (`--no-restore` keeps the copies' saves). `--lane N` uses a test lane's pair, port and pipes (`tools/autotest/lanes.py`). Starts the session's watchdog. Writes `session.json` in the session folder. |
| `live.py do DIR A "lines"` | Plays test script lines in one game (`goto`, `toward`, `press`, `hold`, `warp`, `place`, `setflag`, `kill`, `hit`, ...; `;` between lines), then returns that game's state. |
| `live.py state DIR [A\|B]` | Each game's control-bridge snapshot (map, Link's place, facing, camera, life, rupees, age, event, loading, A prompt) and its new `coop:` log lines since the last look. |
| `live.py shot DIR [A\|B]` | A screenshot of each game (a PNG path). |
| `live.py checkpoint DIR TAG` | Both games write the value of every flag in the shared dungeon and story lists (the module's `dump TAG` test command, next to its commands file; `CoopDungeon::Flags` and `CoopStory::Flags`, not the live-only or autogen lists), and a screenshot of each game is taken (`checkpoints/TAG-A.png`, `TAG-B.png`); all are kept as `checkpoints/TAG.json` with each game's status. The games dump one after the other, so a flag still on its way between them can differ in one checkpoint: a real desync still differs in the next one. |
| `live.py stop DIR` | Ends the session: the watchdog quits both games, kills what did not quit and the server, and lets the lane lock go. |
| `tools/autotest/desync_check.py DIR` | Compares the checkpoints in order: the first tag where the two games' shared flags differ, and which flags, with the checkpoint's shots. Exit 0 every checkpoint compared and agrees, 1 desync, 2 nothing compared (no checkpoint, or none with both games' flags), 3 incomplete (one could not be compared). A plain script, no judgment. |
| `tools/autotest/mcp_playtest.py --session DIR` | An MCP server (stdio, standard library only) with the tools `state`, `act`, `shot`, `checkpoint` and `desync_check`, so Claude Code calls them by name. |

Register the MCP server once from the repository folder, for a session started with `live.py start`:

```sh
claude mcp add oot-playtest -- python -I tools/autotest/mcp_playtest.py --session <session folder>
```

## Where the game windows go

`live.py start` puts both game windows side by side on one monitor and does not take the focus. Choose the monitor
with `--display \\.\DISPLAY2` (a device name), or the environment variable `OOT_TEST_DISPLAY`; the default is
`\\.\DISPLAY2`. A name that no monitor has stops the start with the list of monitors. Details and what to check:
[testing.md](testing.md#where-the-test-games-windows-go). `live.py shot` and the MCP `shot` tool go through the
game's own control bridge, so they work wherever the windows are.

## On a test lane: the lock and the watchdog

`live.py start --lane N` takes the lane's game pair (`<kit root>\ls\laneN-A` / `-B`), its server port and its control
pipes (`coop-laneN-A` / `-B`) from `tools/autotest/lanes.py`, and holds the lane's lock (`laneN.lock`) for the whole
session: the games stay open after the boot, so the lock must too. A busy lane refuses the session at once
(`--lock-wait S` waits for it). The lock is held by the session's watchdog (`live.py watchdog`, a process of its own
that `start` starts), under the watchdog's own pid. The watchdog ends the session when:

- `live.py stop` asks;
- the maximum duration is reached (`--max-minutes`, 30 by default: a lane slot, not an evening);
- no tester command came for `--idle-minutes` (10 by default; counted from the end of the boot): the tester handed
  off, crashed or paused;
- a game ended by itself;
- the watchdog is stopped by a signal, or fails.

Then it asks both games to quit, kills what is still running 15 s later and the server, and lets the lock go;
`watchdog.json` in the session folder says why and what happened. Commands on an ended session are refused. A
watchdog killed outright cannot clean up, but the lock names its pid, so the lock is stale as soon as it is gone.
Without `--lane` the same watchdog runs (the end rules and the clean-up), with no lock.

The lane rules still hold: one game run at a time on the lane, only in a slot the coordinator gave, no local builds
or tests while it runs.

## Rules for a tester

- The games run in real time between steps: the bridge has no frame step yet. A step's own lines are what Link
  does; between steps he stands still and the rest of the world goes on (enemies included). Put the moves that must
  happen together in one `act`.
- Set a test up with `warp`, `place` and `setflag`; do not walk across Hyrule to reach it. `warp` takes a real scene name (`Market_Child`, not `Market`); an unknown one is refused with an `unknown scene` line and nothing is written ([testing.md](testing.md)).
- Take a `checkpoint` on both games after every action that should sync, then run `desync_check`.
- Use `state` to look; `shot` only when the picture matters (a puppet in the wrong place, a door drawn open). A
  checkpoint takes its own shots: anything visible is proven by them, never by flags or log lines alone.
- A report says: the scenario, the steps, what was expected, what happened, and the first checkpoint that differs.
- A test run reports; it does not change the mod. Fixing is a separate session, so a run cannot quietly hide what it
  was meant to catch.

## Proven (2026-11-22)

Both test copies booted into the Forest Temple and left open (`live.py start`); `state` read both games (map, place,
life, rupees, age); `do A "goto ..."` walked A to within 0.4 m of the point; `checkpoint` wrote 1204 shared flags
from each game; `setflag ForestTemple_Room18_Picture_Shot 1` in A alone was 1 in both by the next checkpoint, and
`desync_check` said every checkpoint agreed. The MCP server answered `initialize`, `tools/list` and real `state` and
`desync_check` calls against the running games.

## Not there yet

- **Frame step.** Pausing the game between steps (so a slow tester can play real-time fights) needs the control
  bridge to hold the game loop: a change in the recomp's bridge, not the module.
- **Enemies and owners in the state.** The state has Link's side only. The module's debug walk already reads every
  actor's name, place, life and animation, and the objects sync knows each one's owner; a `dump` of those beside the
  flags is the next step (then the checker compares positions and owners too).
- **More than two games** (the runner starts A and B).
- **Lag on purpose.** The runner's `--test-delay-ms` and `--test-jitter-ms` give the server a delay; a live session
  can pass them through `start` once it takes them.

## Scenarios worth running first

- Both players open the same chest in the same second.
- One player picks up an item while the other changes scene.
- Both players hit the same switch, or kill the last enemy of a room.
- One player disconnects in a dungeon and comes back.
- One player saves and reloads while the other plays on.
- The same flag set by both players with the server's delay on.
- Child and adult Link in different eras at once.
- Tonight's survey fixes ([boss-multiplayer.md](boss-multiplayer.md), "Whole-game survey"): the Forest Temple
  basement floor turned by one player, a Lizalfos pair's owner, a Like Like grab while another player is near.
