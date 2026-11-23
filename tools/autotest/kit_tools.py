"""Pieces of the play-test kit that need no game and no build (docs/playtest.md, "Kit helpers"). Standard library only.

    step_until(client, "event == false")      step the game until a state field matches (or the frames run out)
"""
import json
import re

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
