# Problem reports

The kit can put the co-op problems it finds in a player's game logs into a small zip, masked as collect-logs masks its
zip, for the player to send with the `/bugreport` bot command. It is opt-in: the start asks once, and the answer is kept.
The kit also writes a crash report after a game that crashed or ended without closing, and, only when the player says
yes to that (asked once, default no), sends its crash and problem reports by itself through our relay to the
developers' Discord channel (see "Sent by itself" below). Nothing ever goes to Longshot or anywhere else.

On Linux (the Linux kit, from 0.11.4) crash reports and sending them work the same way (see "On Linux" below); problem
reports are Windows only: the Linux kit has no `problem-report` script yet.

collect-logs (`collect-logs.cmd`) stays the way to send whole logs for one report. Problem reports are the small, regular
side: a few lines a session, and only problems not reported before.

## How a player meets it

- **Asked once.** The first start after the update (`start.ps1`, step 5b) asks: `Make problem reports? [y/N]`. No is the
  default. The answer is kept in `logs\problem-reports.json`.
- **With yes:** each start looks at the game's logs of the sessions since the last report. When they hold a problem never
  reported before, it writes `logs\coop-report-<date>.zip` and says: send it with `/bugreport` in the Discord. Nothing is
  written when there is nothing new; the newest 10 zips are kept. Nothing here can stop the start (any error skips this
  step).
- **At any time:** `scripts\problem-report.cmd` makes a report at once, whatever the answer.
- **Changing the answer:** `scripts\problem-report.cmd on` or `scripts\problem-report.cmd off` (the kit's README says so
  too).

## What is in the zip

`problem-report.txt`: the kit's version, then one line per new problem with how many times it came
(`2x [hos Presentation Thread] coop: boss: could not move into the boss room ... giving up`).

The problems are the module's own warnings and errors, lines saying something failed, could not be done, was refused,
gave up, timed out or was lost, faults the guard caught, and crashes. The words are matched as whole words and as the
module writes them (lower case), so a scene's name (`Lost Woods`) or a note that only mentions a crash is not a problem.
Routine status lines that only contain such words (the pose and actor counters, `camera lost, markers off`, the invite
note) are left out.

Every line is masked with collect-logs' own `Protect-LogText` (`scripts\log-mask.ps1`): IP addresses become their kind
(`<lan-1>`, `<public-1>`, ...), MAC addresses `<mac>`, host names `<host-1>` (`myhost.duckdns.org`, with its port or in a URL; file names
such as `hookshot_coop.dll` stay), the Windows user name `<user>`, the PC name `<pc>`, invite codes and passwords
`(removed)`. Players' names become `<name>` (log-mask.ps1 `Protect-LogNames`, so collect-logs masks them too): the ones
the module or coop.ini writes where a name goes (`player 2 joined: X`,
`player 2 left: X (...)`, `player 2 'X' joined`, `Mods: player 2 (X)`, our own `name 'X'`, coop.ini's `name = X`; common words such as Link are never a name), and those
names anywhere
later in the same log. After that, a last gate drops any line in which anything is still an address by the masking's
own rules (`Get-MaskKind4`, `Get-MaskKind6`), a host name (`myhost.duckdns.org`; file names such as `hookshot_coop.dll`
pass) or a host with a port (`myhost:27510`): nothing that may be one goes into the zip.

## A problem in one report only

Each masked line becomes a fingerprint: its thread tag, the masks' numbers (`<lan-1>`, `<lan-2>`), addresses and numbers
blanked, and names already `<name>`, so the same problem at another address, frame, count or with another player is one
fingerprint. `logs\problem-reports.json` keeps the sessions read (each log's `run_id` line, else its size and first
64 KB) and the fingerprints reported, oldest first (the newest 5000 are kept): a session is read once, and a problem
reported once is never in a later report, from any session. The log of a game still running is left for the next start.

## Reading the logs

A session's key comes from the start of its log only, so a session read before is not read again (a start with nothing
new reads 64 KB a log). A new session is read a line at a time. Logs are opened as collect-logs opens them (shared for
reading, writing and deleting), so a log another program still has open is read; one that cannot be opened at all is
skipped, and tried again at the next start, while the others are still read.

Tests: `tools/release/tests/problem_report_test.ps1` (made-up log lines only).

## Crash reports (from 0.11.4)

At each start (`start.ps1` step 5c, `scripts\send-report.ps1 -AtStart`) the kit looks at the game's finished sessions not
looked at before: the newest two of the last 7 days (older ones are only marked as looked at). It reads the last 512 KB of
each log only, and tells how the session ended from Longshot's own lines (its source: `host\mkw_glue.c`,
`host\hos_thread.c`, `gpu\vk\vk_replay.cpp`, `host\control.c`, `host\hos_relaunch.c`):

| The log's end has | The session | Report |
| --- | --- | --- |
| `CRASH: exception ...` (the crash handler) or `FATAL: ...` (the game aborted, the GPU device was lost) | crashed | yes: `crash` |
| `control: exiting` or `display: window closed, exiting` (also `... for a restart`) | closed cleanly | none |
| neither | ended from outside (Task Manager, the PC turned off) or died without a word | yes: `unclean` |

(Checked on 451 logs of our test kits: no log had both a crash line and a clean exit.) A report is
`logs\crash-report-<date>-<kind>.zip` with `crash-report.txt`: the kit's version, the log's name and time, the crash lines
(from the first `CRASH: exception` / `FATAL:` on, 150 at most), the log's first 60 lines (versions, graphics card,
settings) and its last 300 lines. Every line is masked as above, with the players' names learnt from the whole log. The
newest 10 are kept. The window says that the last game crashed and where the report is; without sending it says to send
it with `/bugreport`.

## Sent by itself (opt-in)

- **Asked once,** at the first start that has a report to send and a relay set: `Send reports by themselves? [y/N]`, no
  after 20 seconds, not asked at all without a console (it asks at a later start). The answer goes into
  `coop-options.ini`: `send_reports = ask` (the default), `yes` or `no`. Change it there or with
  `scripts\send-report.cmd yes` (`no`, `ask`); the kit's README says so.
- **With yes,** each new crash report, and each problem report of the last day not sent yet (two at most a start), is
  sent by a hidden PowerShell process: the start never waits for it. Sent zips are listed in `logs\reports-sent.txt`,
  failures in `logs\report-sending.log`.
- **What goes out:** one HTTPS `POST` a zip to the relay, `Content-Type: application/zip`, with `X-Squadala-Kind`
  (`crash`, `unclean`, `problem`) and `X-Squadala-Kit` (the kit's version from `VERSION.txt`). Only the masked zip; no
  file, setting or name of the PC besides it.
- **Kill switch:** the relay URL is in `scripts\report-relay.txt`, a kit file the updater replaces. Empty (or not an
  `https://` URL): nothing is sent, whatever the answer, and nothing is asked. A release can empty it to stop sending for
  every kit; the owner can also stop the relay itself (below).
- Nothing is ever sent to Longshot (co-op builds have its send paths removed); the relay is ours.

## On Linux

The Linux kit does the same for crash reports: `start-squadala.sh` runs `scripts/send-report.sh --at-start` before it
starts the game (it never stops the start). The same lines tell how a session ended, from the last 512 KB of the game's
`longshot_log*.txt` (in `data/game` or `data/game/logs`, and any a player copied into `data/logs`); the report is
`data/logs/crash-report-<date>-<kind>.zip`, masked by the same rules as collect-logs (`scripts/log-mask.awk`, shared
with `collect-logs.sh`: addresses, host names, MAC addresses, passwords, codes, the user, PC and players' names, those
learnt from the whole log). It is packed with Python's `zipfile` (the builder's Python or the system's), else `zip`.
The question is asked on a terminal only (no after 20 seconds; started from Steam or a menu entry it is asked at a later
start from a terminal); change it with `bash start-squadala.sh --send-reports yes` (`no`, `ask`) or `send_reports` in
`data/coop-options.ini`. With yes, `curl` sends each new crash report in the background, to the relay in
`scripts/report-relay.txt` (the same kill switch); sent zips are listed in `data/logs/reports-sent.txt`, failures in
`data/logs/report-sending.log`. Tests: `tools/release/tests/linux_longshot_kit_test.sh` (CI).

## The relay

`tools/crash-relay/`: a Cloudflare Worker (`src/relay.js` the rules, `src/worker.js` the Worker, `wrangler.toml` without
any secret). It takes `POST /report` only, a zip only (its first local header and its end record, at least one file),
8 MB at most, the kind and a version-looking `X-Squadala-Kit`, and refuses anything else before Discord is called. Rate
limits, in one Durable Object: 3 reports an hour from one address, 30 an hour in all (429 with `Retry-After`). An address
is kept only as a salted SHA-256 hash, only for the hour it was counted in: each hour has a new salt, and an alarm deletes
the hour's record when it ends. Request logs are off (`[observability] enabled = false`) and the Worker logs nothing.
The zip goes to the `#dev-reports` webhook as one message (`Squadala! crash, kit v0.11.4` and the file), with mentions
off. The webhook URL is a Worker secret (`DISCORD_WEBHOOK`): it is never in git or in a kit, so a kit cannot be used to
post into the Discord. Tests: `node --test tools/crash-relay/tests/relay.test.js` (CI, job linux).

## Owner setup (once; the owner does these steps)

1. **Discord webhook:** in the Squadala! Discord, create the `#dev-reports` channel (private to the developers). Its
   settings, Integrations, Webhooks, New Webhook, name it `Crash relay`, Copy Webhook URL. Keep it private.
2. **Cloudflare account:** sign up for a free account at <https://dash.cloudflare.com/sign-up> (the Workers free plan
   is enough: 100,000 requests a day, Durable Objects on SQLite included).
3. **Node.js** (LTS) installed, then in a terminal in `tools/crash-relay`:
   - `npx wrangler login` (opens the browser to allow wrangler on the account)
   - `npx wrangler deploy` (makes the Worker and its Durable Object; it prints the URL,
     `https://squadala-crash-relay.<your-subdomain>.workers.dev`)
   - `npx wrangler secret put DISCORD_WEBHOOK` and paste the webhook URL from step 1 when it asks
4. **Check it:** `curl -i https://squadala-crash-relay.<your-subdomain>.workers.dev/report` answers `405 POST only`;
   a POST without the secret set answers `503 not set up`.
5. **The kit:** put `https://squadala-crash-relay.<your-subdomain>.workers.dev/report` as the one line of
   `tools/release/kit/scripts/report-relay.txt` (under its comment lines) and release: from then on, kits whose players
   said yes send their reports.

To stop it: empty `report-relay.txt` in the next release, and/or `npx wrangler secret delete DISCORD_WEBHOOK` (the relay
answers 503 at once) or delete the webhook in Discord. To change the limits: `PER_IP_PER_HOUR`, `GLOBAL_PER_HOUR`,
`MAX_BYTES` in `src/relay.js`, then `npx wrangler deploy`.

Tests: `tools/release/tests/send_report_test.ps1` (how a session ended, the report and its masking, each session once,
the relay file, `send_reports`, and a whole start posting to a listener on 127.0.0.1).
