"""A live two-game play-test session: start the server and both games once, then act and look one step at a time.

The two-game runner (hookshot.py run) plays a whole script blind. This keeps the games open after their boot script
(`run --keep-open`) and lets a tester (a person, or Claude through tools/autotest/mcp_playtest.py) take one step at a
time and see the result before the next: the step's own script lines (hookshot.py's test script format, `;` between
lines), then what each game says now as JSON (the control bridge's `status` snapshot: map, Link's place, facing, life,
event, loading, prompt) and the new co-op log lines since the last look. A checkpoint writes each game's shared flags
(the module's `dump TAG` test command) beside the session; tools/autotest/desync_check.py compares them.

The games run in real time between steps (the bridge has no frame step): a step's lines are what the game does, and
a long think between steps is time the game goes on (Link stands still, enemies do not). Standard library only.

    python -I tools/autotest/live.py start --session DIR --game-a A --game-b B --server coop_server.exe
                                           [--boot-a boot.A.txt --boot-b boot.B.txt] [--env K=V ...]
    python -I tools/autotest/live.py do --session DIR A "goto 10.0 20.0 0.5 300; press A"
    python -I tools/autotest/live.py state --session DIR [A|B]
    python -I tools/autotest/live.py shot --session DIR [A|B]
    python -I tools/autotest/live.py checkpoint --session DIR TAG
    python -I tools/autotest/live.py stop --session DIR
"""
import argparse
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hookshot  # noqa: E402  (the same folder)

TAGS = ("A", "B")
PIPE = "coop-live"
# Log lines a tester never needs (renderer, GPU, controller and frame-time chatter).
NOISE = re.compile(r"\] (vk:|nvn:|hid:|vi:|vcores:|barrier:|\[debug\])")
MAX_LINES = 80


def session_file(d):
    return os.path.join(d, "session.json")


def load(d):
    with open(session_file(d), encoding="utf-8") as f:
        return json.load(f)


def save(d, s):
    tmp = session_file(d) + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(s, f, indent=1)
    os.replace(tmp, session_file(d))


def start(a):
    d = os.path.abspath(a.session)
    os.makedirs(d, exist_ok=True)
    argv = ["run", "--game-a", a.game_a, "--game-b", a.game_b, "--server", a.server, "--port", str(a.port),
            "--out", d, "--name", "live", "--pipe", PIPE, "--keep-open", "--no-restore", "--every", "0",
            "--scenario-timeout", str(a.boot_timeout)]
    if a.boot_a:
        argv += ["--script-a", a.boot_a]
    if a.boot_b:
        argv += ["--script-b", a.boot_b]
    if a.no_load:
        argv.append("--no-load")
    elif a.load:
        argv += ["--load", a.load]
    for kv in a.env:
        argv += ["--env", kv]
    code = hookshot.main(argv)
    s = {"started": time.time(), "dir": d, "games": {"A": a.game_a, "B": a.game_b}, "offsets": {}}
    for x in TAGS:
        s["games"][x] = {"folder": os.path.abspath(getattr(a, "game_" + x.lower())),
                         "pipe": "%s-%s" % (PIPE, x),
                         "commands": os.path.join(d, "commands-%s.txt" % x),
                         "log": os.path.join(d, "game-out-%s.txt" % x)}
        s["offsets"][x] = os.path.getsize(s["games"][x]["log"]) if os.path.exists(s["games"][x]["log"]) else 0
    save(d, s)
    return {"ok": code == 0, "session": d, "boot": "played" if code == 0 else "FAILED (see steps.txt)"}


def bridge(s, x):
    return hookshot.Bridge.connect(s["games"][x]["pipe"], timeout=15)


def new_lines(d, s, x):
    """The game's co-op log lines since the last look (noise left out), at most MAX_LINES (the newest)."""
    path = s["games"][x]["log"]
    try:
        with open(path, "rb") as f:
            f.seek(s["offsets"].get(x, 0))
            data = f.read()
            s["offsets"][x] = f.tell()
    except OSError:
        return []
    lines = [ln for ln in data.decode("utf-8", "replace").splitlines() if "coop:" in ln and not NOISE.search(ln)]
    lines = [re.sub(r"^\[hos [^\]]*\] ", "", ln) for ln in lines]
    if len(lines) > MAX_LINES:
        lines = ["(%d older lines left out)" % (len(lines) - MAX_LINES)] + lines[-MAX_LINES:]
    return lines


def snapshot(b):
    r = b.request("status", reply_timeout=10.0)
    g = r.get("game") or {}
    out = {"frame": r.get("frame"), "state": r.get("state"), "map": r.get("map")}
    for k in ("pos", "facing", "camera", "life", "max_life", "rupees", "age", "event", "loading", "prompt"):
        if k in g:
            out[k] = g[k]
    return out


def look(d, s, x):
    b = bridge(s, x)
    try:
        st = snapshot(b)
    finally:
        b.close()
    st["log"] = new_lines(d, s, x)
    return st


def state(a):
    d = os.path.abspath(a.session)
    s = load(d)
    out = {x: look(d, s, x) for x in ([a.player] if a.player else TAGS)}
    save(d, s)
    return out


def do(a):
    d = os.path.abspath(a.session)
    s = load(d)
    x = a.player
    text = "\n".join(part.strip() for part in a.script.split(";") if part.strip()) + "\n"
    steps = hookshot.parse(text)
    b = bridge(s, x)
    err = None
    try:
        p = hookshot.Player(b, hookshot.Log(os.path.join(d, "player-%s.txt" % x)),
                            connect=lambda: bridge(s, x), commands=s["games"][x]["commands"])
        p.play(steps)
    except Exception as e:  # (a step the game refused: said, and the look still comes back)
        err = "%s: %s" % (e.__class__.__name__, e)
    finally:
        b.close()
    out = {x: look(d, s, x)}
    if err:
        out["error"] = err
    save(d, s)
    return out


def shot(a):
    d = os.path.abspath(a.session)
    s = load(d)
    os.makedirs(os.path.join(d, "shots"), exist_ok=True)
    out = {}
    for x in ([a.player] if a.player else TAGS):
        path = os.path.join(d, "shots", "%s-%d.png" % (x, int(time.time() * 1000)))
        b = bridge(s, x)
        try:
            b.request("shot", reply_timeout=30.0, path=path, timeout=20)
            out[x] = path
        except Exception as e:
            out[x] = "no shot: %s" % e
        finally:
            b.close()
    return out


def checkpoint(a):
    d = os.path.abspath(a.session)
    s = load(d)
    if not re.fullmatch(r"[A-Za-z0-9_-]+", a.tag):
        raise SystemExit("a tag is letters, digits, - and _")
    os.makedirs(os.path.join(d, "checkpoints"), exist_ok=True)
    out = {"tag": a.tag}
    for x in TAGS:
        dump = s["games"][x]["commands"] + ".dump-%s.json" % a.tag
        try:
            os.remove(dump)
        except OSError:
            pass
    for x in TAGS:
        b = bridge(s, x)
        try:
            p = hookshot.Player(b, hookshot.Log(os.path.join(d, "player-%s.txt" % x)),
                                commands=s["games"][x]["commands"])
            p.play(hookshot.parse("dump %s\n" % a.tag))
            out[x] = {"status": snapshot(b)}
        finally:
            b.close()
    for x in TAGS:
        dump = s["games"][x]["commands"] + ".dump-%s.json" % a.tag
        deadline = time.monotonic() + 10
        while not os.path.exists(dump) and time.monotonic() < deadline:
            time.sleep(0.1)
        try:
            with open(dump, encoding="utf-8") as f:
                out[x]["flags"] = json.load(f).get("flags", {})
        except (OSError, ValueError) as e:
            out[x]["flags"] = None
            out[x]["error"] = "no dump: %s" % e
    with open(os.path.join(d, "checkpoints", "%s.json" % a.tag), "w", encoding="utf-8") as f:
        json.dump(out, f, indent=1)
    save(d, s)
    return {"tag": a.tag, "A": {"map": out["A"]["status"].get("map"), "flags": len(out["A"]["flags"] or {})},
            "B": {"map": out["B"]["status"].get("map"), "flags": len(out["B"]["flags"] or {})}}


def stop(a):
    d = os.path.abspath(a.session)
    s = load(d)
    out = {}
    for x in TAGS:
        try:
            b = bridge(s, x)
            b.request("quit", reply_timeout=10.0, check=False)
            b.close()
            out[x] = "quit asked"
        except Exception as e:
            out[x] = "not reached: %s" % e
    return out


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    st = sub.add_parser("start")
    st.add_argument("--session", required=True)
    st.add_argument("--game-a", required=True)
    st.add_argument("--game-b", required=True)
    st.add_argument("--server", required=True)
    st.add_argument("--port", type=int, default=27593)
    st.add_argument("--boot-a")
    st.add_argument("--boot-b")
    st.add_argument("--load", help="a load script (A until play); default: the boot scripts load")
    st.add_argument("--no-load", action="store_true", help="the boot scripts take the title themselves")
    st.add_argument("--env", action="append", default=[], help="K=V, or A:K=V / B:K=V for one game")
    st.add_argument("--boot-timeout", type=int, default=900)
    for name in ("do", "state", "shot", "checkpoint", "stop"):
        q = sub.add_parser(name)
        q.add_argument("--session", required=True)
        if name == "do":
            q.add_argument("player", choices=TAGS)
            q.add_argument("script", help="test script lines, ';' between them")
        elif name in ("state", "shot"):
            q.add_argument("player", nargs="?", choices=TAGS)
        elif name == "checkpoint":
            q.add_argument("tag")
    a = p.parse_args(argv)
    out = {"start": start, "do": do, "state": state, "shot": shot, "checkpoint": checkpoint, "stop": stop}[a.cmd](a)
    print(json.dumps(out, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
