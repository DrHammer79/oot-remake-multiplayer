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

## Bridge protocol

The play-test bridge is a small part of the co-op module inside each game (`hookshot/src/coop_playtest.cpp`). An agent
or MCP client drives one game per connection. It adds what Longshot's own control bridge lacks: **frame control** (the
game holds still between calls). It also gives the co-op module's test commands and Longshot's input, shots and Link
snapshot through one socket with one frame count.

### Turning it on (off by default)

Nothing listens unless the game is started with:

| Switch | Value |
|---|---|
| `LICHII_COOP_PLAYTEST_PORT` | TCP port, 1024 to 65535. Unset, empty or `0`: the bridge is off and nothing listens |
| `HOS_CONTROL` | Longshot's control socket (Linux) or pipe (Windows). The bridge forwards `input`, `shot` and the Link snapshot in `state` to it. Without it those answer `no_control` |
| `LICHII_COOP_PLAYTEST_IDLE` | Seconds a hold may last with no command before the game resumes by itself (default 600, `0` never), in case the tester crashed |

The module has the bridge built in but off. A tester turns it on for one game with
these variables; `live.py start` sets them for each game it starts. The log says
`coop: playtest: listening on 127.0.0.1:<port>` or `coop: playtest: off (<why>)`.

**Ports, one per game:** player N on `5000 + N` (p1 5001, p2 5002, ... p4 5004). On test lane L: `5000 + 10*L + N`
(lane 1: 5011, 5012). The MCP server takes the port as its argument.

**Security:** bound to `127.0.0.1` only, never another address. One client at a time: a second connection gets one
`busy` reply and is closed. **Keep one connection open for the whole session:** when the client's connection closes
(it quit or crashed), a held game runs again at once, so a lost tester never leaves a game frozen. Dumps and shots go
only into the game's own `playtest/dumps/` and `playtest/shots/` folders, named by a tag; no path is taken from the
client. It can do what Longshot's control bridge and the cheats panel can do (move Link, warp, set
flags, give items), so it is for test copies with a throwaway save.

### Messages

One JSON object per line (UTF-8, `\n`), at most 64 KB a line. A request has `cmd` and an optional `id` (any JSON value,
given back). Requests are answered in order, one reply each. Every reply carries:

| Field | Meaning |
|---|---|
| `id` | The request's `id`, or `null` |
| `ok` | `true`, or `false` with `error` (a code below) and `message` (for a person) |
| `frame` | The game's frame count: calc frames since the game started (the module's per-frame hook). It is what `step` counts and what lines up two games' dumps |
| `paused` | Whether the game is held now |

`input`, `step` and `warp` replies hold the end state nested as `"state": {...}`; `state` itself is flat.

Error codes: `bad_request` (unknown `cmd`, a missing or bad field), `busy` (another client), `no_control`
(`HOS_CONTROL` unset or unreachable), `not_in_play` (no Link: title, file select, a load), `timeout`, `failed` (the
game refused; `message` says why).

```
> {"id":1,"cmd":"hello"}
< {"id":1,"ok":true,"frame":5321,"paused":false,"protocol":1,"control":true}
```

### Frame control

The game holds inside its own per-frame hook: the calc thread waits there before the frame's update, so a held game
does not advance at all (no Link, no enemies, no timers). The window, the sound thread, Longshot's control bridge and
the co-op connection keep running (the network thread answers the server's pings, so a held game is never dropped
as timed out). The other game's puppet of this player stands still while it is held. The server does not know a game
is held: an enemy or boss this game owns stays with it while the other game runs. So **always hold both games** and
step them in turn, the same number of frames.

| Command | Fields | Does |
|---|---|---|
| `pause` | | Holds the game at its next frame. Replies once it is held |
| `resume` | | Lets it run in real time again |
| `step` | `frames` (1 to 3600, default 1) | Lets exactly that many frames run, then holds. Replies when held again, with `from` (`frame - from == frames`). A running game is held first |

### Commands

| Command | Fields | Does / replies |
|---|---|---|
| `state` | | The state below. Does not advance the game |
| `input` | `buttons` (list of names, default none), `stick` `[angle, strength]` (left stick: angle in degrees, 0 = up, 90 = right, clockwise; strength 0 to 1), `rstick` (the same for the right stick), `frames` (1 to 3600, default 1), `exclusive` (default `true`: the real pad and keys are shut out meanwhile) | Holds that pad for `frames` frames from the next frame, lets them run, then holds. Replies with `from` and the `state` at the end. Buttons (Switch names): `A B X Y L R ZL ZR PLUS MINUS LS RS UP DOWN LEFT RIGHT` |
| `warp` | `scene` (a map name, e.g. `KokiriForest_Child`), `entrance` (a start position name in that scene; omitted: the scene's own), `timeout` (seconds, default 60) | The co-op test warp (the game's own `Sequence_ReservedNextBanc`, as the test script's `warp`). The game runs (10 frames at a time) until Link is in that scene with no event or load, then holds again. Replies with `"arrived": true` and the `state`; after the timeout, still held, `timeout` with the state in `message` |
| `flag` | `name` (a GameData flag name or a `0x` hash), `value` (number, `true`/`false`, or an enum value's name) | Sets the flag through the game's own setter (the test script's `setflag` / `setenum`). Done inside the held frame: no frame passes. Replies with `was` and `value` |
| `give` | `item` (`bombs`, `bow`, `hookshot`, or an item flag name the cheats panel knows) | The test script's `give` (the cheats panel's queue, so it needs `LICHII_COOP_CHEATS=1`): queued inside the held frame, in the inventory after the next frame runs |
| `act` | `lines`: ONE test line, at most 256 characters, no control characters, no `;` or `#`. Allowed kinds: `kill`, `hit`, `hurt`, `place`, `face`, `aim`, `wear`, `dress`, `assign`, `chat`, `menu`, `setflag`, `setenum` | The test script's line, run on the game's own thread inside the held frame (the same parser as `live.py do`). Any other kind (`goto`, `warp`, `press`, ...) is refused with `line N: WORD is not allowed here (allowed: ...)`; use the commands above for those. Replies with `lines` and a `note`: the game does it on its next frames, so `step` a few and read the `state`. Behind the verb gate below |
| `dump` | `tag` (1 to 64 letters, digits, `-` and `_`) | Writes `playtest/dumps/<tag>.json` in the game's folder: `{"frame", "tag", "state", "flags": {name: value}}`, the shared flags (the checkpoint's lists) read inside the held frame. Replies with `path` (absolute) and `flags` (the count) |
| `shot` | `name` (optional; letters, digits, `-` and `_`; default `shot-f<frame>`) | A screenshot through Longshot's bridge into `playtest/shots/<name>.png`. A held game presents nothing, so it runs 1 frame at a time (at most 5) until the picture comes: the reply's `advanced` says how many. Replies with `path`, `width`, `height` |

**The verb gate.** `act`, `warp`, `flag` and `give` reach the wire and the other players' games, so they run only when
the test runner asked for them and cheats are on: `LICHII_COOP_PLAYTEST_VERBS=1` and `LICHII_COOP_CHEATS=1`. The runner
(`tools/autotest/hookshot.py`, so `live.py start --playtest` too) sets both next to `LICHII_COOP_PLAYTEST_PORT`. No file
a player starts from names `LICHII_COOP_PLAYTEST_VERBS`: not `play.ps1`, not `start.sh`, not `coop-options.ini`
(`test_playtest_bridge.py` checks it). Otherwise the reply is `refused` with one of:

- `refused: this is not a test run (LICHII_COOP_PLAYTEST_VERBS is not set by the test runner), and these commands reach the wire and other players`
- `refused: cheats are off (LICHII_COOP_CHEATS is not set)`

`OOT_COOP_KIT_START` is no sign of a test: the kit's start files set it for every player's game. Nor is a loopback
server: a real host's game connects to its own server at 127.0.0.1. At start the module logs `coop: playtest: verbs
on|off (test runner: yes|no, cheats: on|off)`. `state`, `dump`, `input`, `step`, `pause`, `resume` and `shot` are not
gated.

### State

```
{"frame":5400,"paused":true,"scene":"KokiriForest_Child","room":null,"pos":[-7.5,0.0,71.1],"facing":90.0,
 "camera":180.0,"life":12,"max_life":12,"rupees":5,"age":"child","event":false,"loading":false,"prompt":"Talk",
 "equips":{"weapon":"KokiriSword","shield":"DekuShield","head":null,"armor":"Kokiri","boots":"Normal"},
 "inventory":{"PouchData.Item_Bomb.StockNum":0,"PouchData.Item_Hookshot.StockNum":1},"coop":{"players":[{"number":1,"name":"A","self":true,"state":"connected","scene":"KokiriForest_Child"}]},
 "snapshot_present":5398,"control":true}
```

`scene`, `pos`, `facing`, `camera`, `life`, `max_life`, `rupees`, `age`, `event`, `loading` and `prompt` are
Longshot's Link snapshot (the control bridge's `status`, `game`): metres, yaw in degrees (0 toward +z, 90 toward +x),
quarter hearts. `snapshot_present` is Longshot's present count when it was taken (Link's last update; on a title,
pause or load it is older). `control` is false when Longshot's bridge did not answer (those fields are then `null`). `equips`, `inventory` and `coop` come from the module. A field it cannot read is `null`; `room`
stays `null` until the room is found in memory. `coop_menu` is where co-op's own menu is (`"closed"`, or the page's name such as
`"customize"`). It does not see the game's own pause screen: `"closed"` says only that co-op's menu is shut, so a
tester who pressed Start reads the screen (`shot`), not this.

**`world`** (the bridge adds it to `state` and to a `dump`): the actors the object walk sees, nearest to our Link first,
the kinds that matter (player, enemy, npc, chest, door, switch, item) before `other` (grass, pots, ...). The walk
reads the place of every actor it sees and keeps up to 200, `other` last, because the sync checks need grass, bushes and
beehives too. `state` lists only the nearest 40 of the known kinds and counts the rest (`total`, `other_count`); `dump`
lists all of them. It is there only while the games are connected and sharing, because the object walk runs only then.

**An empty `actors` list does not mean "nothing here".** Solo (no co-op session) it is empty with `frame` null, and
`why` says so. Each empty list carries a `why`:

- `"no co-op session: the world list is read only while connected and sharing"`: the walk never ran.
- `"stale: no walk for N ms (a load, or the session ended)"`: the last list is older than 1.5 s.
- `"empty: the walk ran and saw no actors"`: connected, and the room really has none.
- `"only other kinds near: dump lists them"`: `state` lists known kinds only, and the room has only grass, pots and the like.

```
"world":{"frame":5400,"age_ms":40,"stale":false,"behind":false,"cost_us":310,"total":31,"other_count":30,"count":1,"actors":[
  {"id":2147483648,"addr":"0x80000000","name":"Enemy_Example","kind":"enemy","pos":[1.5,0.0,-3.2],"dist":4.0,"life":2,"max_life":4,
   "owner":"server","alive":true}]}
```

- `id` is the actor's address in decimal and `addr` the same in hex, as the `LICHII_COOP_DEBUG_OBJECTS` log prints it.
- `owner` is `"server"`, a player number, or `null` (the sharing does not know the actor). `life`, `max_life` and
  `alive` are read for enemies only; for any other kind they are `null` (unknown, not dead).
- `frame` is the bridge's frame count when the walk began; the reply's own `frame` is the game's now. While the game
  is held, `state` waits up to 300 ms for a snapshot of that frame, but only when a fresh one is behind the frame (solo
  or stale: no wait); if none comes, `behind` is true and the older list is returned with its frame. A running game returns the newest list, which is a few frames old.
- A snapshot older than 1.5 s of wall clock (a load, a closed room, the walk stopped) is not shown: `stale` is true,
  `count` 0, `actors` empty, `why` says "stale", `age_ms` says how old.
- The list costs the object walk `cost_us` microseconds about every 100 ms. Without the bridge (the switch is never
  turned on) the walk does two relaxed atomic loads (before and after the actor loop) and the `state` text has no `world` member.

Walking: the stick is camera-relative. To walk toward world yaw `w` (`atan2(dx, dz)` in degrees), push the stick at
angle `w - camera` (the sign is settled by the LANE1 proof run; `walk_to` learns it from how Link moves). The camera turns as Link walks, so steer in short `input` steps (5 to 10 frames) and read `camera`
again each time.

## On a test lane: the lock and the watchdog

`live.py start --lane N` takes the lane's game pair (`<kit root>\ls\laneN-A` / `-B`), its server port and its control
pipes (`coop-laneN-A` / `-B`) from `tools/autotest/lanes.py`, and holds the lane's lock (`laneN.lock`) for the whole
session: the games stay open after the boot, so the lock must too. A busy lane refuses the session at once
(`--lock-wait S` waits for it). The lock is held by the session's watchdog (`live.py watchdog`, a process of its own
that `start` starts), under the watchdog's own pid. The watchdog ends the session when:

- `live.py stop` asks;
- the maximum duration is reached (`--max-minutes`, 30 by default: a lane slot, not an evening; 0 means no limit);
- no tester command came for `--idle-minutes` (10 by default, 0 means never; counted from the end of the boot): the tester handed
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

- With the play-test bridge on ([Bridge protocol](#bridge-protocol)) the games are held between calls: nothing
  happens unless you act. Without it (the older `act` path) they run in real time between steps: put the moves that
  must happen together in one `act`.
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

- **Frame step in a game.** The play-test bridge holds the game in the module ([Bridge protocol](#bridge-protocol));
  that is not proven in a game yet. `tools/autotest/bridge_proof.py` on a test lane checks it first.
- **World list: the client and the checker.** The `world` member is in the bridge's `state` and `dump`. The client's
  `get_world` and `get_actor`, and the checker that compares positions and owners of the two games, come next.
  Not proven in a game: the list needs a lane run (connected games, an enemy in the room).
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

## Playing through the bridge: one MCP server per game

With the play-test bridge on (see "Bridge protocol"), each game gets its own MCP server,
`tools/autotest/mcp_playtest.py --player pN`. The server keeps one bridge connection open for as long as it runs. It
holds the game at its first call, so **the game is paused between calls**: it runs only the frames a call asks for.
When the server exits, the connection closes and the game runs again (so does a crashed tester's). Standard library
only, no `pip install`.

| Tool | What it does |
| --- | --- |
| `get_state` | Scene, room, Link's position `[x, y, z]` in metres, facing and camera yaw (0 = +z, 90 = +x), life, rupees, age, equips, inventory, co-op status, event, loading, A prompt. No frame passes. The MCP tool adds words beside the numbers: `life_word` (full, ok above half, low at half or less, critical at a quarter or less), `enemy_near` (living enemies within 10 m) and `danger` (one within 4 m). The last two are null, never 0 or false, when there is no world list; `enemy_near_at_least` is true when the 40 listed actors were cut inside 10 m (the count is then a lower bound) |
| `get_world [kind] [within_m] [limit]` | The actors near Link from the bridge's world list, nearest first: `id`, `addr`, `name`, `kind`, `pos`, `dist`, `life`, `max_life`, `owner` (`"server"`, a player number, or null = unknown), `alive`. It reads the whole list (up to 200) through a `dump` under the tag `world_query`, not the 40 of `state`. `kind` is one of player, enemy, npc, chest, door, switch, item, other; `within_m` is a distance in metres; `limit` is 1 to 200 (50 by default). The reply has `matched`, `of` and `truncated`, and keeps the list's `why`: **an empty list with a `why` is not "nothing here"** (solo, stale, or no walk yet). No frame passes |
| `get_actor id` | One actor by `id` (the number `get_world` gives) or by `0x...` address. `found` is false with a `why` when it is gone, outside the 200 nearest, or there is no list |
| `walk_to x z [tolerance] [max_frames]` | Walks to the point 8 frames at a time, stopping on arrival (0.5 m by default), a scene change, a load, an event or `max_frames` (600). It says why it stopped, the frames used and the distance left |
| `press [buttons] [stick_angle stick_strength] [frames]` | Holds the buttons (and the stick: 0 = up, away from the camera, 90 = right) for N frames (2 by default), then holds the game |
| `step frames` | Lets N frames run with no input |
| `warp scene [entrance]` | The game's own scene change; it returns once Link is there with no event or load |
| `flag name [value]` | Sets a flag (name or `0x` hash; a number, true/false or an enum name) inside the held frame; replies with the old value |
| `give item` | Gives the item through the cheats queue: it shows after the next frame (`step 1`) |
| `kill actor`, `hit actor amount` (`actor` is a **name**: the game takes the nearest actor of that name with a life, never a chosen id; `get_world` lists them nearest first), `hurt quarter_hearts`, `place x y z`, `wear part name`, `dress [part]`, `assign item`, `chat text`, `menu page` | One `act` line each (the arguments are checked here first: no spaces in a name, `amount` and `quarter_hearts` 1 to 80, `part` one of weapon, shield, head, armor, boots, hand, `page` customize or close). Same gate and refusal text as `act`. They take effect on the next frames: `step`, then `get_state` |
| `checkpoint tag` | The game's `dump` (every shared flag) with its frame and state, appended to `<log dir>/<player>.checkpoints.jsonl` |
| `desync_check [all]` | `desync_check.py --clients` on the log folder: tags matched across every game, each game's frame printed |
| `log [match]` | **Raw, unmasked** (do not paste it into an issue; use `check_logs` for that). The game's new `coop:` log lines since the last call (`match`: only lines with that text). Needs `--session`: games from `live.py start`, p1 = A, p2 = B |
| `check_logs [since] [names]` | The oracle for "did anything go wrong". This game's new `coop:` lines that say `failed`, `error`, `refused` or `resync` (any case, also `fails`, `failure`, `refusal`, `resyncing`), **masked**, with `flagged` (how many), `lines` (the newest 40; `left_out` counts the rest), `read` (all new `coop:` lines) and `verdict`: `clean`, `flagged`, or `no log`. `since` is `last_check` (the default: since the last `check_logs` of this server, or the boot) or `boot`. It keeps its own place in the log, so it and `log` never take lines from each other. `names` (up to 20 words) are hidden wherever they stand. Needs `--session`. **A clean verdict is not proof**: it says only that the game printed no such line. No frame passes |
| `session_status` | Is the run still sound: `bridge` (`connected`, `held`, `last_frame`, `connects`) and, with `--session`, `session` (`running`, or `ended` with the watchdog's reason; each game's `pid` and `alive`; `max_minutes_left`, `idle_minutes_left`). It asks the game nothing, works after the session ended, and does not restart the idle clock. No frame passes |
| `pause`, `resume` | Hold the game, or let it run in real time (a person takes over; the next call holds it again) |
| `screenshot [name]` | A PNG of this game: the game saves it as `<game>/playtest/shots/<player>-<name>-<time>.png` (it runs one frame to show it); a copy goes to `<log dir>/shots/` |

**What `check_logs` masks.** IPv4 and IPv6 addresses (with a port); the user name in `C:/Users/<name>`, `/home/<name>`
and `/Users/<name>` paths; memory addresses (`0x` and six or more hex digits); hex strings of 32 or more characters; the
value after `name`, `nickname`, `password`, `token`, `secret`, `key`, `host`, `server` and `room`; every double-quoted
text; and the words in `names`. It is the filter of the tool, not a guarantee: a player name written plainly in a log
line without one of these words, and not in `names`, stays. Pass the players' names in `names`. Nothing from a log goes
into an issue or a pull request unmasked (AGENTS.md); quote only the lines you need.

Every reply carries the game's `frame`.

**walk_to's stick.** The stick is camera-relative. walk_to starts from the bridge's model (walk toward world yaw
`camera + stick angle`). It learns the real offset and whether the stick is mirrored from how Link actually moves on
each push, and reads the camera again after every 8 frames. On a bend the right model shows itself within a few
pushes. A walk that gets nowhere for three pushes steps aside for three. Into a wall it times out: walk_to does no
pathfinding, so give it points with a clear line between them and use `warp` for anything far.

### Setting up a run

1. Start the games with the bridge on, one port each: player N on `5000 + N`, or `5000 + 10*L + N` on test lane L.
   `live.py start --playtest [--lane L]` does it for its two games (A = p1, B = p2: the runner's
   `--playtest-ports BASE` sets each game's `LICHII_COOP_PLAYTEST_PORT`, `LICHII_COOP_PLAYTEST_VERBS=1` and
   `LICHII_COOP_CHEATS=1` (the gate of `act`, `warp`, `flag` and `give`);
   `HOS_CONTROL` was set already), and its answer lists the ports. Give it `--max-minutes` for a long run (30 by
   default). By hand: set `LICHII_COOP_PLAYTEST_PORT`, `HOS_CONTROL`, `LICHII_COOP_PLAYTEST_VERBS=1` and `LICHII_COOP_CHEATS=1` in each game's
   environment (or `live.py start --env A:LICHII_COOP_PLAYTEST_PORT=5011 --env B:...`). Each game's log says
   `coop: playtest: listening on 127.0.0.1:<port>`.
2. Register one MCP server per game, all with the **same** `--log-dir` (desync_check reads every game's log there),
   and, for games from `live.py start`, the same `--session`: each call then counts as a tester command for the
   session's watchdog (else it ends the session after `--idle-minutes`), and an ended session is refused. In a
   project `.mcp.json` (the shape most MCP clients read):

   ```json
   {"mcpServers": {
     "oot-p1": {"command": "python", "args": ["-I", "tools/autotest/mcp_playtest.py", "--player", "p1", "--lane", "1", "--session", "sessions/run1", "--log-dir", "playtest-logs/run1"]},
     "oot-p2": {"command": "python", "args": ["-I", "tools/autotest/mcp_playtest.py", "--player", "p2", "--lane", "1", "--session", "sessions/run1", "--log-dir", "playtest-logs/run1"]}
   }}
   ```

   The same with the `claude` command, one line per game (run from the repository folder; `--scope project` writes
   `.mcp.json`, `--scope local` keeps it to you; a Claude session sees the servers only if they were registered
   before it started):

   ```sh
   claude mcp add oot-p1 -- python -I tools/autotest/mcp_playtest.py --player p1 --lane 1 --session <session folder> --log-dir <log dir>
   claude mcp add oot-p2 -- python -I tools/autotest/mcp_playtest.py --player p2 --lane 1 --session <session folder> --log-dir <log dir>
   claude mcp list    # both must show Connected
   ```

   `--port N` names a port outright. Three or four players: `oot-p3` and `oot-p4` the same way (games started by
   hand: `live.py` starts two).
3. The tester follows [AGENTS.md](../AGENTS.md), "Play-testing the game", and runs [tests/scenarios.md](../tests/scenarios.md)
   in order.

After a scenario, `python -I tools/autotest/desync_check.py <log dir> --clients` gives the same answer as the
`desync_check` tool (exit 0 agree, 1 desync, 2 nothing compared, 3 incomplete).

### Rules for a tester on the bridge

- The games are held between calls. Nothing happens unless you act. Hold **every** game: a game whose server
  you have not called yet still runs (call `get_state` on each first).
- Set a test up with `warp`, `flag` and `give`. Do not walk across Hyrule.
- Take a `checkpoint` with the same tag on **every** game after each action that should sync. Let the other game
  run a few frames first (`step 30`): a flag reaches it over the network, and a held game receives nothing. Then
  run `desync_check`.
- Use `get_state` to look and `screenshot` only when the picture matters. Anything visible is proven by a picture,
  never by flags alone.
- A bug report has: the scenario, the steps (tool calls), what was expected, what happened, the first tag that
  differs with each game's frame, and the shots.
- Never edit the mod's source during a test run. Report; fixing is a separate session.

## Lag on purpose

Two games on one PC really go through the network stack. Each game's co-op module connects to the server with ENet
(UDP) at `127.0.0.1:<server port>` (`lichii/hook/coop_hook.cpp`, `coop/net/src/client.cpp`), so its packets pass
through loopback sockets like any other program's. The play-test bridge is TCP on loopback too: shape only the
server's UDP port, or the tester's own calls are delayed as well.

- **Longshot (the kits): the server's test delay.** `coop_server --test-delay-ms N --test-jitter-ms J` with
  `OOT_COOP_TEST=1` holds every packet (N is the round trip, half each way). `run.ps1 -Runtime hookshot -Lag N -Jitter J`
  and `groups.py --lag N` pass it on ([testing.md](testing.md), "On Longshot: the server's test delay"). This is the
  repeatable choice: same rules every run, nothing to install.
- **Windows, any build: Clumsy.** Filter on the server's port only, for example
  `udp and (udp.DstPort == 27511 or udp.SrcPort == 27511)` (Clumsy sees loopback packets: both directions go
  through the loopback interface). Then lag, drop, out-of-order. It needs administrator rights (WinDivert), so it
  is for a person's PC, not an unattended lane run.
- **Linux: tc netem on lo, on the server's port only.** Plain `tc qdisc add dev lo root netem delay 150ms loss 5%`
  delays every loopback packet, the bridge included, and each packet crosses `lo` twice (out and in), so it doubles.
  On the port only:

  ```sh
  sudo tc qdisc add dev lo root handle 1: prio
  sudo tc qdisc add dev lo parent 1:3 handle 30: netem delay 75ms loss 2.5%
  sudo tc filter add dev lo parent 1:0 protocol ip u32 match ip protocol 17 0xff match ip dport 27511 0xffff flowid 1:3
  sudo tc filter add dev lo parent 1:0 protocol ip u32 match ip protocol 17 0xff match ip sport 27511 0xffff flowid 1:3
  # ... run the scenarios ...
  sudo tc qdisc del dev lo root
  ```

  Each direction gets 75 ms here, so the round trip is about 150 ms.
- **The emulator build: `tools/netsim/netsim.py`**, the UDP relay ([testing.md](testing.md), "Bad connections").

Run the same scenario without lag first, then with it, and compare the two `desync_check` reports.
