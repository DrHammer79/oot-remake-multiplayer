"""Pieces of the play-test kit that need no game and no build (docs/playtest.md, "Kit helpers"). Standard library only.

    step_until(client, "event == false")      step the game until a state field matches (or the frames run out)
    StateHistory                              the last states a tester looked at: `since` gives only what changed
"""
import json
import os
import re
import time

# --- step_until: a condition on one state field -----------------------------------------------------------------

OPS = ("==", "!=", "<=", ">=", "<", ">")
CONDITION = re.compile(r"^(?P<neg>!\s*)?(?P<path>[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z0-9_]+)*)"
                       r"(?:\s*(?P<op>==|!=|<=|>=|<|>)\s*(?P<val>.+?))?\s*$")
MAX_FRAMES = 3600
CHUNK = 8       # frames per look: a state is read after each (the game is held between them)
MAX_CHUNK = 60


class ConditionError(ValueError):
    """A condition that cannot be read, or that names a field the state does not have."""


def parse_condition(text):
    """`path`, `!path`, or `path OP value` (OP: == != < <= > >=) as (path parts, op, value, negated). The path is dotted
    (`event`, `equips.weapon`, `pos.1`); the value is JSON (true, false, null, 12, "Name") or a bare word (a string).
    A bare path means "is not null, false, 0 or empty"; `!path` the opposite."""
    m = CONDITION.match(str(text or ""))
    if not m:
        raise ConditionError("condition: `field`, `!field` or `field OP value` with OP one of %s (got %r)"
                             % (" ".join(OPS), text))
    if (m.group("neg") and m.group("op")) or (m.group("op") and m.group("val") is None):
        raise ConditionError("condition: `!` goes with a bare field only (got %r)" % text)
    val = None
    if m.group("op"):
        raw = m.group("val")
        try:
            val = json.loads(raw)
        except ValueError:
            val = raw.strip("'\"")
    return m.group("path").split("."), m.group("op"), val, bool(m.group("neg"))


def lookup(state, parts):
    """(found, value) of a dotted path in a state; a number part indexes a list."""
    cur = state
    for p in parts:
        if isinstance(cur, dict) and p in cur:
            cur = cur[p]
        elif isinstance(cur, list) and p.isdigit() and int(p) < len(cur):
            cur = cur[int(p)]
        else:
            return False, None
    return True, cur


def _number(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def evaluate(cond, state):
    """(met, value, why): met is True or False; why says when the field was not there or not comparable (a null is
    "do not know", never a match for an ordering: docs/playtest.md, "State")."""
    parts, op, want, neg = cond
    found, got = lookup(state, parts)
    if not found:
        return False, None, "no field %s in the state" % ".".join(parts)
    if op is None:
        return (not got) if neg else bool(got), got, None
    if op in ("==", "!="):
        if _number(got) and _number(want):
            same = float(got) == float(want)
        else:
            same = type(got) is type(want) and got == want
        return (same if op == "==" else not same), got, None
    if not (_number(got) and _number(want)):
        return False, got, "%s is %r: %s needs numbers" % (".".join(parts), got, op)
    ok = {"<": got < want, "<=": got <= want, ">": got > want, ">=": got >= want}[op]
    return ok, got, None


def step_until(client, condition, max_frames=300, chunk=CHUNK):
    """Steps the game `chunk` frames at a time (it is held between them) until the condition holds on the state, or
    `max_frames` frames have passed. Looks first, so a condition that holds already costs no frame. The reply says
    `met`, the field's `value` now, the frames run and the last state. A field the first state does not have is an error
    (a typo is not a time-out); a null later is "do not know" and the wait goes on."""
    cond = parse_condition(condition)
    max_frames, chunk = int(max_frames), int(chunk)
    if not 1 <= max_frames <= MAX_FRAMES:
        raise ValueError("max_frames is 1 to %d" % MAX_FRAMES)
    if not 1 <= chunk <= MAX_CHUNK:
        raise ValueError("chunk is 1 to %d" % MAX_CHUNK)
    used, looks = 0, 0
    while True:
        st = client.state()
        looks += 1
        met, got, why = evaluate(cond, st)
        if looks == 1 and why and why.startswith("no field"):
            raise ConditionError("%s (fields: %s)" % (why, ", ".join(sorted(k for k in st if not k.startswith("_")))))
        out = {"met": met, "condition": str(condition).strip(), "value": got, "frames_run": used, "state": st}
        if met:
            return out
        if used >= max_frames:
            out["why"] = "not met after %d frames%s" % (used, " (%s)" % why if why else "")
            return out
        n = min(chunk, max_frames - used)
        client.step(n)
        used += n


# --- delta state: only what changed since an earlier look ---------------------------------------------------------

VOLATILE = ("frame", "id", "ok", "paused", "snapshot_present", "reconnected", "state_id", "log", "world_frame",
            "inventory")   # (`inventory_count` says it changed)
KEEP = 16   # looks kept


def flatten(d, prefix=""):
    """{dotted path: value} of a state: nested objects are walked, lists and scalars are leaves."""
    out = {}
    for k, v in d.items():
        if not prefix and k in VOLATILE:
            continue
        path = prefix + k
        if isinstance(v, dict) and v:
            out.update(flatten(v, path + "."))
        else:
            out[path] = v
    return out


def delta(old, new):
    """What changed from state `old` to `new`: {"changed": {path: new value}, "removed": [paths]}. `frame` and the
    other counters that move on every look are left out."""
    a, b = flatten(old), flatten(new)
    return {"changed": {k: v for k, v in b.items() if k not in a or a[k] != v},
            "removed": sorted(k for k in a if k not in b)}


def _write(path, data):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f)
    for i in range(20):   # (Windows: a reader holding the old file a moment refuses the replace)
        try:
            os.replace(tmp, path)
            return
        except PermissionError:
            if i == 19:
                raise
            time.sleep(0.05)


class StateHistory:
    """The last KEEP looks: an id for each, and what every player showed then. In memory (an MCP server), or in a file
    (`path`: live.py runs once per call, so the history lives beside the session)."""

    def __init__(self, path=None, keep=KEEP):
        self.path, self.keep = path, keep
        self.data = {"next": 1, "looks": {}}
        if path:
            try:
                with open(path, encoding="utf-8") as f:
                    got = json.load(f)
                if isinstance(got, dict) and isinstance(got.get("looks"), dict):
                    self.data = got
            except (OSError, ValueError):
                pass

    def add(self, states):
        """Keeps one look ({player: state}) and returns its id."""
        n = int(self.data["next"])
        self.data["next"] = n + 1
        self.data["looks"][str(n)] = {k: {a: b for a, b in v.items() if a not in ("log",)} for k, v in states.items()}
        for old in sorted(self.data["looks"], key=int)[:-self.keep]:
            del self.data["looks"][old]
        if self.path:
            _write(self.path, self.data)
        return n

    def get(self, n):
        return self.data["looks"].get(str(n))

    def kept(self):
        ids = sorted(int(i) for i in self.data["looks"])
        return (ids[0], ids[-1]) if ids else None

    def reply(self, states, since=None):
        """{player: reply}: with no `since` each state whole and the look's `state_id`; with one, only what changed
        since that look. An id that is not kept (too old, or from another run) gives the whole state and says why."""
        old = self.get(since) if since is not None else None
        n = self.add(states)
        out = {}
        for k, st in states.items():
            if since is None:
                out[k] = dict(st, state_id=n)
            elif old is not None and k in old:
                out[k] = dict(delta(old[k], st), state_id=n, since=int(since), frame=st.get("frame"))
                if "log" in st:
                    out[k]["log"] = st["log"]   # (the log lines are new since the last look already)
            else:
                kept = self.kept()
                out[k] = dict(st, state_id=n, why="no look %s kept (kept: %s): the whole state" % (
                    since, "%d to %d" % kept if kept else "none"))
        return out
