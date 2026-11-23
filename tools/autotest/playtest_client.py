"""The outside half of the play-test bridge: one game's bridge client, walk_to's steering and the checkpoint log.

The game's play-test bridge (hookshot/src/coop_playtest.cpp; off unless LICHII_COOP_PLAYTEST_PORT is set;
docs/playtest.md, "Bridge protocol") listens on 127.0.0.1 only, one port per game, and speaks one JSON object per
line each way. Every reply carries the game's frame count, which lines up two games' checkpoints later
(desync_check.py --clients). A held game runs only the frames a command asks for.

This file holds what is easy to change without a game build: the client, the camera-relative steering of walk_to
(learnt from how Link actually moves), and the per-game checkpoint log. mcp_playtest.py --player turns it into MCP
tools. Standard library only.
"""
import json
import math
import os
import re
import socket
import time

HOST = "127.0.0.1"
PORT_BASE = 5000          # player N: PORT_BASE + N (p1 5001 ... p4 5004); on test lane L: PORT_BASE + 10*L + N
PLAYERS = ("p1", "p2", "p3", "p4")
TAG = re.compile(r"^[A-Za-z0-9_-]{1,64}$")
BUTTONS = ("A", "B", "X", "Y", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "LS", "RS", "UP", "DOWN", "LEFT", "RIGHT")
MAX_FRAMES = 3600


class BridgeError(Exception):
    """The game refused a command (ok false): its error code (bad_request, busy, no_control, not_in_play, timeout,
    failed) and message."""

    def __init__(self, reply):
        super().__init__("%s: %s" % (reply.get("error"), reply.get("message", "")))
        self.reply = reply
        self.code = reply.get("error")


def player_index(player):
    p = str(player).lower()
    if p not in PLAYERS:
        raise ValueError("a player is one of %s" % ", ".join(PLAYERS))
    return PLAYERS.index(p) + 1


def port_for(player, lane=None):
    """The bridge port of player p1..p4, on a test lane when given (tools/autotest/lanes.py)."""
    return PORT_BASE + 10 * int(lane or 0) + player_index(player)


class Client:
    """One game's bridge over ONE connection, kept for the life of the process (the bridge lifts its hold when the
    connection closes, so a crashed tester never leaves a game frozen). It connects at the first request and holds
    the game then (`hold`, unless the request is pause or resume itself), so the game is still between calls. A
    broken connection is opened again at the next request; the game ran in real time meanwhile, so it is held
    again and that reply says "reconnected". Replies are matched by id, in order."""

    def __init__(self, port, host=HOST, timeout=30.0, connect=socket.create_connection, hold=True):
        if host not in ("127.0.0.1", "localhost"):
            raise ValueError("the bridge is local only (127.0.0.1)")
        self.host, self.port, self.timeout, self._connect, self.hold = host, int(port), timeout, connect, hold
        self.sock = self.rfile = None
        self.connects = 0
        self.next_id = 1
        self.last_frame = None
        self.held = False   # the game is held by this connection now (a `resume`, or a closed connection, lets it run)

    def close(self):
        """Closes the connection: the bridge lets the game run again."""
        for f in (self.rfile, self.sock):
            try:
                if f is not None:
                    f.close()
            except OSError:
                pass
        self.sock = self.rfile = None
        self.held = False

    def _open(self):
        self.sock = self._connect((self.host, self.port), self.timeout)
        self.rfile = self.sock.makefile("rb")
        self.connects += 1

    def _send(self, cmd, wait, args):
        rid = self.next_id
        self.next_id += 1
        self.sock.settimeout(wait)
        self.sock.sendall((json.dumps(dict(args, id=rid, cmd=cmd), separators=(",", ":")) + "\n").encode())
        while True:
            line = self.rfile.readline()
            if not line:
                raise ConnectionError("%s: the bridge closed the connection" % cmd)
            reply = json.loads(line.decode("utf-8"))
            if isinstance(reply.get("id"), int) and reply["id"] < rid:
                continue   # an older request's late reply (one that timed out here)
            return reply

    def request(self, cmd, check=True, reply_timeout=None, **args):
        wait = reply_timeout or self.timeout
        reconnected = False
        if self.sock is None:
            self._open()
            reconnected = self.connects > 1
            if self.hold and cmd not in ("pause", "resume"):
                try:
                    held = self._send("pause", self.timeout, {})
                except (OSError, ValueError):
                    self.close()
                    raise
                if not held.get("ok"):
                    self.close()
                    raise BridgeError(held)
                self.held = True
        try:
            reply = self._send(cmd, wait, args)
        except (OSError, ValueError):
            self.close()   # (the next request connects again and holds the game)
            raise
        if reply.get("ok") and cmd in ("pause", "resume"):
            self.held = cmd == "pause"
        if reconnected:
            reply["reconnected"] = True
        if reply.get("frame") is not None:
            self.last_frame = reply["frame"]
        if check and not reply.get("ok"):
            raise BridgeError(reply)
        return reply

    # the commands (docs/playtest.md, "Bridge protocol")
    def hello(self):
        return self.request("hello")

    def pause(self):
        return self.request("pause")

    def resume(self):
        return self.request("resume")

    def state(self):
        return self.request("state")

    def step(self, frames=1):
        return self.request("step", frames=_frames(frames))

    def input(self, stick=None, buttons=(), frames=1, rstick=None):
        """Holds the pad for `frames` frames, then the game is held again. A stick is [angle, strength]: degrees
        clockwise from up (up = away from the camera), strength 0..1."""
        args = {"buttons": list(buttons), "frames": _frames(frames)}
        if stick is not None:
            args["stick"] = [round(float(stick[0]), 2) % 360.0, round(max(0.0, min(1.0, float(stick[1]))), 3)]
        if rstick is not None:
            args["rstick"] = [round(float(rstick[0]), 2) % 360.0, round(max(0.0, min(1.0, float(rstick[1]))), 3)]
        return self.request("input", reply_timeout=self.timeout + frames / 20.0, **args)

    def warp(self, scene, entrance=None, timeout=60):
        args = {"scene": scene, "timeout": timeout}
        if entrance not in (None, ""):
            args["entrance"] = entrance
        return self.request("warp", reply_timeout=timeout + 15, **args)

    def flag(self, name, value=1):
        return self.request("flag", name=name, value=value)

    def give(self, item):
        return self.request("give", item=item)

    def act(self, line):
        """One test line (kill, hit, hurt, place, face, aim, wear, dress, assign, chat, menu, setflag, setenum), run
        inside the held frame. The bridge refuses any other kind, a second line, ';' and '#' (docs/playtest.md)."""
        return self.request("act", lines=line)

    def dump(self, tag):
        return self.request("dump", tag=tag)

    def world(self):
        """The whole world list (up to 200 actors, `state` shows only the nearest 40 known kinds): the bridge's `dump`
        under a fixed tag, read back from the game's own folder. Replies {"frame", "world"}; world is None when this
        build of the module has no world member."""
        r = self.dump(WORLD_TAG)
        try:
            with open(r["path"], encoding="utf-8") as f:
                data = json.load(f)
        except (KeyError, OSError, ValueError) as e:
            raise ValueError("the world list could not be read back from the dump: %s" % e)
        return {"frame": r.get("frame"), "world": (data.get("state") or {}).get("world")}

    def shot(self, name=None):
        """A screenshot; the game saves it under its own playtest/shots/<name>.png and the reply gives the path."""
        if name is not None and not TAG.match(name):
            raise ValueError("a shot name is 1 to 64 letters, digits, - and _")
        return self.request("shot", reply_timeout=60, **({"name": name} if name else {}))


# --- the log oracle (check_logs): the game's new `coop:` lines that say something went wrong, masked ---------------------
# A line is flagged by its level (Longshot prints `warning: `, `error: ` or `critical: ` before the text of a LOG_WARNING,
# LOG_ERROR or LOG_CRITICAL: most of the real warnings contain none of the words) or by one of the four words.
LEVEL = re.compile(r"^(warning|error|critical): ")
FLAG_WORDS = re.compile(r"\b(fail(?:ed|ure|ures|s)?|error|errors|refus(?:ed|es|al)|resync\w*)", re.IGNORECASE)
MAX_FLAGGED = 40
_IPV4 = re.compile(r"(?<![\w.])(?:\d{1,3}\.){3}\d{1,3}(?::\d{1,5})?(?!\.?\d)")
# an IPv6 address has a `::` or a hex letter (a time such as 12:34:56 has neither)
_IPV6 = re.compile(r"(?<![\w:])\[?(?=[0-9A-Fa-f:]*(?:::|[A-Fa-f]))(?:[0-9A-Fa-f]{0,4}:){2,7}[0-9A-Fa-f]{0,4}\]?(?::\d{1,5})?(?![\w:])")
_DOTTED_HOST = re.compile(r"(?<![\w.-])(?:[A-Za-z0-9-]+\.)+[A-Za-z][A-Za-z0-9-]*:\d{1,5}(?!\d)")   # myhouse.duckdns.org:7777
_HOME = re.compile(r"(?i)([A-Z]:[\\/]+Users[\\/]+|/Users/|/home/)[^\\/\s\"']+")
_ADDR = re.compile(r"\b0[xX][0-9A-Fa-f]{6,}\b")
_HEX = re.compile(r"\b[0-9A-Fa-f]{32,}\b")
# `password: x`, `token=x`: the value is hidden after `=` or `:`. `host`, `server`, `room`, `key` and `name` open real
# log prefixes (`coop: host: refused ...`), so they hide a value only after `=`.
_SECRET_STRONG = re.compile(r"(?i)\b(pass(?:word|phrase)?|token|secret|nick(?:name)?)\b(\s*[=:]\s*)(\"[^\"]*\"|\S+)")
_SECRET_WEAK = re.compile(r"(?i)\b(key|name|user(?:name)?|host|server|room)\b(\s*=\s*)(\"[^\"]*\"|\S+)")
_JOIN_PASSWORD = re.compile(r"(?i)\b(join password\s+)\S+")          # `coop: on, ..., join password {}`
_NAMED = re.compile(r"(?i)\b(name|nick(?:name)?|player\s+\d+)(\s+)'[^'\n]*'")   # `player 2 'Bob' joined`, `name 'Bob'`
_QUOTED = re.compile(r'"[^"\n]*"')
_TIME = re.compile(r"^\[hos [^\]]*\] ")
_NOISE = re.compile(r"\] (vk:|nvn:|hid:|vi:|vcores:|barrier:|\[debug\])")   # live.NOISE: the emulator's own chatter
_NAME_IN_LOG = re.compile(r"(?i)\b(?:name|nick(?:name)?|player\s+\d+)\s+'([^'\n]{1,40})'")
MAX_NAME_SCAN = 8 << 20


def mask_text(text, names=()):
    """A log line with nothing a person could be found by: IP addresses (v4 and v6, with a port; also before a full
    stop), a host name with a port, the user name in a home path, game memory addresses, long hex strings (keys), the
    value after `password`, `token`, `secret` and `nickname` (`=` or `:`) and after `name`, `host`, `server`, `room`
    and `key` (`=` only), the join password, a quoted `name '...'` or `player N '...'`, every double-quoted text, and
    each of `names` wherever it stands (whole words; case does not matter, except for a name of 2 characters or
    fewer, which must match its case). Best effort on top of the log's own wording: docs/playtest.md lists what it
    does not catch."""
    t = _HOME.sub(lambda m: m.group(1) + "<user>", text)
    t = _DOTTED_HOST.sub("<host>", t)
    t = _IPV6.sub("<ip>", t)
    t = _IPV4.sub("<ip>", t)
    t = _ADDR.sub("<addr>", t)
    t = _HEX.sub("<hex>", t)
    t = _JOIN_PASSWORD.sub(lambda m: m.group(1) + "<hidden>", t)
    t = _NAMED.sub(lambda m: "%s%s'<name>'" % (m.group(1), m.group(2)), t)
    for rx in (_SECRET_STRONG, _SECRET_WEAK):
        t = rx.sub(lambda m: m.group(1) + m.group(2) + "<hidden>", t)
    t = _QUOTED.sub('"<text>"', t)
    for n in sorted({str(n).strip() for n in names if str(n).strip()}, key=len, reverse=True):
        t = re.sub(r"(?<!\w)" + re.escape(n) + r"(?!\w)", "<name>", t, flags=0 if len(n) <= 2 else re.IGNORECASE)
    return t


def session_names(paths):
    """The player names the games printed in their own logs (`name 'X'`, `player N 'X'`): the default `names` of
    check_logs, so a name that stands plainly in another line is hidden without the tester knowing it."""
    found = []
    for path in paths:
        try:
            with open(path, "rb") as f:
                data = f.read(MAX_NAME_SCAN).decode("utf-8", "replace")
        except (OSError, TypeError):
            continue
        for m in _NAME_IN_LOG.finditer(data):
            n = m.group(1).strip()
            if n and n not in found:
                found.append(n)
    return found


def read_log_since(path, offset):
    """(the `coop:` lines written after byte `offset`, the new offset). Only whole lines are read: a half-written last
    line stays for the next call. A file that is shorter than the offset (a new game on the same path) is read from
    its start. A missing file: ([], offset)."""
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            if f.tell() < offset:
                offset = 0
            f.seek(offset)
            data = f.read()
    except OSError:
        return [], offset
    cut = data.rfind(b"\n") + 1
    data = data[:cut]
    lines = [_TIME.sub("", ln) for ln in data.decode("utf-8", "replace").splitlines()
             if "coop:" in ln and not _NOISE.search(ln)]
    return lines, offset + cut


def flag_reason(line):
    """Why a line is flagged: `level:warning`, `level:error`, `level:critical` or `word:<the word>`; None if it is not."""
    m = LEVEL.search(line)
    if m:
        return "level:" + m.group(1)
    m = FLAG_WORDS.search(line)
    return "word:" + m.group(1).lower() if m else None


def flagged_lines(lines, names=()):
    """The flagged lines, masked, each with its reason, the newest MAX_FLAGGED, and how many were left out."""
    hit = [(r, ln) for ln in lines for r in [flag_reason(ln)] if r]
    return {"flagged": len(hit), "left_out": max(0, len(hit) - MAX_FLAGGED),
            "lines": [{"why": r, "line": mask_text(ln, names)} for r, ln in hit[-MAX_FLAGGED:]]}


WORLD_TAG = "world_query"   # the tag of the dump `Client.world` reads: one file, overwritten each time
KINDS = ("player", "enemy", "npc", "chest", "door", "switch", "item", "other")
LIFE_WORDS = ("full", "ok", "low", "critical")
# the `why` values of an empty list that still count as "the walk ran": the enemy words are then real zeros
# (matched by prefix, so a change of the C++ wording after the colon keeps the words)
WALK_RAN_PREFIXES = ("empty:", "only other kinds")


def walk_ran(why):
    return why is None or (isinstance(why, str) and why.startswith(WALK_RAN_PREFIXES))


def life_word(life, max_life):
    """full: at max; ok: above half; low: half or less; critical: a quarter or less. None when unknown."""
    if not isinstance(life, (int, float)) or not isinstance(max_life, (int, float)) or max_life <= 0:
        return None
    if life >= max_life:
        return "full"
    ratio = life / max_life
    return "critical" if ratio <= 0.25 else "low" if ratio <= 0.5 else "ok"


def world_words(state):
    """Words next to the numbers, from a bridge `state` reply: `life_word`, `enemy_near` (living enemies within 10 m) and
    `danger` (one within 4 m). enemy_near and danger are None, never 0 and false, when the world list is not there
    (solo, stale, a module without it). `state` lists the nearest 40 known actors: when the list was cut inside 10 m
    the count is a lower bound and `enemy_near_at_least` is true; when it was cut inside 4 m and no listed enemy is that
    close, `danger` is None (a True stays True)."""
    out = {"life_word": life_word(state.get("life"), state.get("max_life")), "enemy_near": None, "danger": None}
    w = state.get("world")
    if not isinstance(w, dict) or not walk_ran(w.get("why")):
        return out
    listed = w.get("actors") or []
    foes = [a for a in listed if a.get("kind") == "enemy" and a.get("alive") is not False]
    out["enemy_near"] = sum(1 for a in foes if a.get("dist") is not None and a["dist"] <= 10)
    out["danger"] = any(a.get("dist") is not None and a["dist"] <= 4 for a in foes)
    known = (w.get("total") or 0) - (w.get("other_count") or 0)
    cut = known > len(listed) and bool(listed)
    if cut and (listed[-1].get("dist") or 0) <= 10:
        out["enemy_near_at_least"] = True
    if cut and not out["danger"] and (listed[-1].get("dist") or 0) <= 4:
        out["danger"] = None   # an unlisted enemy may stand nearer than the last one listed: unknown, not false
    return out


def select_actors(world, kind=None, within_m=None, limit=50):
    """The actors of a world list that match: a `kind` and/or a distance (nearest first, ties by id), at most `limit`.
    Replies the list's own members (frame, why, stale, behind, age_ms, total, other_count) beside `matched` (how many
    matched), `of` (how many the list holds) and `actors`. `why` stays, so an empty answer never reads as 'nothing here'
    when the list is just not there."""
    if kind is not None and kind not in KINDS:
        raise ValueError("kind: one of " + ", ".join(KINDS))
    if within_m is not None and (not isinstance(within_m, (int, float)) or within_m < 0):
        raise ValueError("within_m: a number of metres, 0 or more")
    limit = int(limit)
    if not 1 <= limit <= 200:
        raise ValueError("limit: 1 to 200")
    actors = world.get("actors") or []
    hit = [a for a in actors if (kind is None or a.get("kind") == kind)
           and (within_m is None or (a.get("dist") is not None and a["dist"] <= within_m))]
    hit.sort(key=lambda a: (a.get("dist") if a.get("dist") is not None else 1e9, a.get("id", 0)))
    out = {k: world[k] for k in ("frame", "age_ms", "stale", "behind", "why", "total", "other_count") if k in world}
    out.update({"matched": len(hit), "of": len(actors), "truncated": len(hit) > limit, "actors": hit[:limit]})
    return out


def find_actor(world, ident):
    """One actor by id (a number or its decimal text) or by address (`0x...`); None when it is not in the list."""
    text = str(ident).strip().lower()
    for a in world.get("actors") or []:
        if text == str(a.get("id")) or (text.startswith("0x") and text == str(a.get("addr", "")).lower()):
            return a
    return None


def _frames(n):
    n = int(n)
    if not 1 <= n <= MAX_FRAMES:
        raise ValueError("frames is 1 to %d" % MAX_FRAMES)
    return n


def game_of(reply):
    """The state in a reply: its "state" (or "game") object when it has one, else the reply itself (state)."""
    for key in ("state", "game"):
        if isinstance(reply.get(key), dict):
            return reply[key]
    return reply


# --- walk_to -------------------------------------------------------------------------------------------------------

def _wrap(deg):
    return (deg + 180.0) % 360.0 - 180.0


def yaw_to(pos, x, z):
    """The world yaw in degrees from pos toward (x, z): 0 toward +z, 90 toward +x (the state's convention)."""
    return math.degrees(math.atan2(x - pos[0], z - pos[2]))


class Steering:
    """Camera-relative steering. The model: Link walks toward world yaw camera + offset + sign * stick angle. The
    bridge's starting guess is offset 0, sign +1 (docs/playtest.md); both are learnt from Link's real moves. The
    offset is learnt for each sign; the sign whose offset fits the moves best wins (they tell apart once the stick
    angle changes, as it does on any bend). A walk that gets nowhere for `stuck_after` pushes steps aside."""

    def __init__(self, slow_within=3.0, stuck_after=3, detour_for=3, sign=1):
        self.slow_within, self.stuck_after, self.detour_for = slow_within, stuck_after, detour_for
        self.sign = sign
        self.fit = {1: {"offset": 0.0, "err": 0.0, "n": 0}, -1: {"offset": 0.0, "err": 0.0, "n": 0}}
        self.last = None              # (pos, camera, stick angle) of the last push
        self.stuck = self.detour_left = 0
        self.detour = 90.0

    def learn(self, pos, camera):
        if self.last is None:
            return
        lpos, lcam, lang = self.last
        if math.hypot(pos[0] - lpos[0], pos[2] - lpos[2]) <= 0.05:
            self.stuck += 1
            if self.stuck >= self.stuck_after and not self.detour_left:
                self.detour, self.detour_left, self.stuck = -self.detour, self.detour_for, 0
            return
        self.stuck = 0
        seen = yaw_to(lpos, pos[0], pos[2])
        for s, f in self.fit.items():
            got = _wrap(seen - lcam - s * lang)            # what this sign says the offset is
            if f["n"] == 0:
                f["offset"] = got
            else:
                miss = _wrap(got - f["offset"])
                f["err"] = 0.6 * f["err"] + 0.4 * abs(miss)
                f["offset"] = _wrap(f["offset"] + 0.5 * miss)
            f["n"] += 1
        other = -self.sign
        if self.fit[other]["n"] >= 2 and self.fit[other]["err"] + 10.0 < self.fit[self.sign]["err"]:
            self.sign = other

    def push(self, pos, camera, target):
        """The stick [angle, strength] for Link at pos (metres) with the camera yaw (None: unknown, taken as 0) to
        walk toward target (x, z)."""
        camera = float(camera or 0.0)
        self.learn(pos, camera)
        want = yaw_to(pos, target[0], target[1])
        angle = self.sign * _wrap(want - camera - self.fit[self.sign]["offset"])
        if self.detour_left:
            angle += self.detour
            self.detour_left -= 1
        dist = math.hypot(target[0] - pos[0], target[1] - pos[2])
        strength = round(max(0.45, min(1.0, dist / self.slow_within)), 3)
        angle = round(angle, 2) % 360.0
        self.last = (list(pos), camera, angle)
        return [angle, strength]


def walk_to(client, x, z, tolerance=0.5, max_frames=600, chunk=8):
    """Walks Link to (x, z) a few frames at a time (the game is held after each). Stops on arrival (within
    `tolerance` metres), a scene change, a load or an event, or after max_frames. Returns why, the frames used,
    the distance left and the last state."""
    st = game_of(client.state())
    scene = st.get("scene")
    steer = Steering()
    used = 0
    while True:
        pos = st.get("pos")
        if not pos:
            return {"arrived": False, "reason": "no position in the state (not in play?)", "frames": used,
                    "state": st}
        dist = math.hypot(x - pos[0], z - pos[2])
        out = {"frames": used, "distance": round(dist, 3), "state": st}
        if dist <= tolerance:
            return dict(out, arrived=True, reason="arrived")
        if st.get("scene") != scene:
            return dict(out, arrived=False, reason="scene changed")
        if st.get("loading") or st.get("event"):
            return dict(out, arrived=False, reason="loading" if st.get("loading") else "event")
        if used >= max_frames:
            return dict(out, arrived=False, reason="timed out")
        n = min(chunk, max_frames - used)
        st = game_of(client.input(steer.push(pos, st.get("camera"), (x, z)), (), n))
        used += n


# --- the per-game checkpoint log -----------------------------------------------------------------------------------

def checkpoint_file(log_dir, player):
    return os.path.join(log_dir, "%s.checkpoints.jsonl" % player.lower())


def read_dump(reply):
    """The game's dump: (flags, state) from the file the reply names ("path"), or from the reply itself."""
    if isinstance(reply.get("flags"), dict):
        return reply["flags"], reply.get("state")
    path = reply.get("path")
    if not path:
        return None, None
    deadline = time.monotonic() + 10
    while not os.path.exists(path) and time.monotonic() < deadline:
        time.sleep(0.1)
    with open(path, encoding="utf-8") as f:
        d = json.load(f)
    return d.get("flags"), d.get("state")


def checkpoint(client, log_dir, player, tag):
    """The game's shared flags and state under a tag, appended to this game's checkpoint log with its frame."""
    if not TAG.match(tag):
        raise ValueError("a tag is 1 to 64 letters, digits, - and _")
    reply = client.dump(tag)
    try:
        flags, state = read_dump(reply)
        error = None if isinstance(flags, dict) else "the dump has no flags"
    except (OSError, ValueError) as e:
        flags, state, error = None, None, "no dump: %s" % e
    if not isinstance(state, dict):
        state = game_of(client.state())
    entry = {"tag": tag, "player": player.lower(), "frame": reply.get("frame"), "time": time.time(),
             "scene": state.get("scene"), "state": state, "flags": flags if isinstance(flags, dict) else None,
             "dump": reply.get("path")}
    if error:
        entry["error"] = error
    os.makedirs(log_dir, exist_ok=True)
    with open(checkpoint_file(log_dir, player), "a", encoding="utf-8") as f:
        f.write(json.dumps(entry, separators=(",", ":")) + "\n")
    out = {"tag": tag, "frame": entry["frame"], "scene": entry["scene"],
           "flags": len(entry["flags"]) if entry["flags"] is not None else None}
    if error:
        out["error"] = error
    return out
