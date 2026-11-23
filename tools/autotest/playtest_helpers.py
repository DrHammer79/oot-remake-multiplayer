"""Mid-level play helpers on top of the play-test bridge client (docs/playtest.md, "Helpers").

They are Python, not module verbs: each one calls the bridge's own commands (`state`, `dump` for the world list, `input`,
`step`, `act`) and is bounded: at most MAX_FRAMES frames (120 by default), and each says why it stopped.

Rule Y (the exact actor the tester picked): a helper that goes to or strikes an actor takes the `id` get_world gave and
remembers the actor's name; it looks the actor up again before every leg and every press, and stops with `gone` when the id
is no longer in the list or its name changed. It never falls back to another actor of the same name.

`both`: pass `InStep(this_game, other_game)` as the client and every frame the helper runs in this game also runs in the
other (the held game of a pair must be stepped by the same frames, or the two drift). Standard library only.
"""
import math

import playtest_client as pc

MAX_FRAMES = 120       # one helper call runs at most this many frames in the game
NEAR_M = 1.5           # "near an actor": within this many metres
LEG_M = 4.0            # go_to_actor walks in legs of this length, looking the actor up again after each
LEG_FRAMES = 30
STUCK_FRAMES = 60      # no 0.3 m closer to the actor over this many frames: stuck
STRIKE_FRAMES = 12     # a kill or hit waits at most this many frames for the walk's result
STRIKE_LOOK = 2        # ... looking every this many frames
PRESS_FRAMES = 6
CLOSER_M = 0.3         # a leg that brings Link this much nearer is progress


class InStep:
    """Two games in step: every frame the primary game runs through input() or step() the other game runs too. Anything
    else (state, world, act) goes to the primary only."""

    def __init__(self, primary, other):
        self.primary, self.other = primary, other

    def __getattr__(self, name):
        return getattr(self.primary, name)

    def input(self, stick=None, buttons=(), frames=1, rstick=None):
        r = self.primary.input(stick, buttons, frames, rstick)
        self.other.step(frames)
        return r

    def step(self, frames=1):
        r = self.primary.step(frames)
        self.other.step(frames)
        return r


def _pos(state):
    p = state.get("pos")
    return p if isinstance(p, (list, tuple)) and len(p) >= 3 else None


def _look(client, ident):
    """(the actor, its world list) for the id now. One `dump`: the walk's newest snapshot."""
    w = client.world()["world"] or {}
    return pc.find_actor(w, ident), w


def _no_list(w):
    """Why the world list cannot be used (solo, stale, an older module), or None when the walk ran."""
    if not w:
        return "this module has no world list (an older build)"
    return None if pc.walk_ran(w.get("why")) and not w.get("stale") else (w.get("why") or "no world list")


def _gone(a, name):
    return a is None or (name is not None and a.get("name") != name)


def go_to_actor(client, ident, name=None, near_m=NEAR_M, max_frames=MAX_FRAMES, stuck_after=STUCK_FRAMES,
                leg_m=LEG_M):
    """Walk Link toward the actor `ident` (the `id` from get_world) in legs of `leg_m`, looking it up again after every
    leg. `name` (from get_world, the name that came with the id): the actor must still have it. Stops near it
    (`arrived`), or `reason`: gone (no longer in the list, or another name), stuck (`stuck_after` frames without getting
    0.3 m nearer; `moved_m` says how far Link went in all), timed out, no world list, scene changed, loading, event.
    Returns arrived, reason, stuck, moved_m, frames, distance, actor (the last one seen)."""
    a, w = _look(client, ident)
    out = {"arrived": False, "reason": "", "stuck": False, "moved_m": 0.0, "frames": 0, "distance": None, "actor": a}
    if _no_list(w) and a is None:
        return dict(out, reason="no world list: " + str(_no_list(w)))
    name = name or (a or {}).get("name")
    st = pc.game_of(client.state())
    scene, used, still = st.get("scene"), 0, 0
    start = _pos(st)
    prev, leg_n = None, 0
    while True:
        if _gone(a, name):
            return dict(out, reason="gone")
        out["actor"] = a
        pos, at = _pos(st), a.get("pos")
        if pos is None or not at:
            return dict(out, reason="no position (not in play?)")
        dist = math.hypot(at[0] - pos[0], at[2] - pos[2])
        out.update(distance=round(dist, 2), frames=used,
                   moved_m=round(math.hypot(pos[0] - start[0], pos[2] - start[2]), 2) if start else 0.0)
        if prev is not None:
            still = 0 if dist <= prev - CLOSER_M else still + leg_n
        prev = dist
        if dist <= near_m:
            return dict(out, arrived=True, reason="near")
        if st.get("scene") != scene:
            return dict(out, reason="scene changed")
        if st.get("loading") or st.get("event"):
            return dict(out, reason="loading" if st.get("loading") else "event")
        if still >= stuck_after:
            return dict(out, reason="stuck", stuck=True)
        if used >= max_frames:
            return dict(out, reason="timed out")
        step = max(0.5, min(leg_m, dist - near_m * 0.5))
        r = pc.walk_to(client, pos[0] + (at[0] - pos[0]) / dist * step, pos[2] + (at[2] - pos[2]) / dist * step,
                       tolerance=0.4, max_frames=min(LEG_FRAMES, max_frames - used))
        st = pc.game_of(r["state"]) if isinstance(r.get("state"), dict) else pc.game_of(client.state())
        leg_n = max(1, r.get("frames", 0))
        used += leg_n
        a, w = _look(client, ident)


def face(client, ident, name=None):
    """Turn Link to the actor: the bridge's `face X Z` (the actor's place), then two frames. Looks the actor up first."""
    a, w = _look(client, ident)
    if _gone(a, name):
        return {"faced": False, "reason": "gone", "actor": a}
    at = a.get("pos")
    if not at:
        return {"faced": False, "reason": "no position", "actor": a}
    client.act("face %.2f %.2f" % (at[0], at[2]))
    client.step(2)
    return {"faced": True, "reason": "ok", "actor": a}


def attack(client, ident, button="Y", name=None, max_frames=MAX_FRAMES, press_frames=PRESS_FRAMES):
    """go_to_actor, face, then press `button` (the sword; Y is a guess to confirm in the lane run) in rounds until the
    actor's life is 0 or `max_frames` frames are used. The actor is looked up before the walk and before every press:
    nothing is pressed at an actor that is gone or has another name. Returns result (dead: its life was seen at 0;
    gone: it left the list in the fight, perhaps dead; alive: the frame limit; stopped: the walk or the turn did not
    finish, see `stopped`), life_before, life_after, frames, rounds."""
    a, w = _look(client, ident)
    if _gone(a, name):
        return {"result": "stopped", "stopped": "gone" if not _no_list(w) else "no world list: " + str(_no_list(w)),
                "life_before": None, "life_after": None, "frames": 0, "rounds": 0}
    name = name or a.get("name")
    before = a.get("life")
    out = {"result": "alive", "life_before": before, "life_after": before, "frames": 0, "rounds": 0}
    if isinstance(before, (int, float)) and before <= 0:
        return dict(out, result="dead", note="its life was already 0")
    g = go_to_actor(client, ident, name=name, max_frames=max_frames)
    out["frames"] = g["frames"]
    if not g["arrived"]:
        return dict(out, result="stopped", stopped=g["reason"], moved_m=g["moved_m"], stuck=g["stuck"])
    f = face(client, ident, name)
    if not f["faced"]:
        return dict(out, result="stopped", stopped=f["reason"])
    out["frames"] += 2
    while out["frames"] < max_frames:
        a, w = _look(client, ident)
        if _gone(a, name):
            return dict(out, result="gone")
        life = a.get("life")
        out["life_after"] = life
        if isinstance(life, (int, float)) and life <= 0:
            return dict(out, result="dead")
        client.input(None, [button], press_frames)
        client.step(press_frames)
        out["frames"] += 2 * press_frames
        out["rounds"] += 1
    a, w = _look(client, ident)
    if _gone(a, name):
        return dict(out, result="gone")
    out["life_after"] = a.get("life")
    if isinstance(out["life_after"], (int, float)) and out["life_after"] <= 0:
        out["result"] = "dead"
    return out


CHANGED = ("life", "max_life", "rupees", "event", "prompt", "equips", "inventory", "age")


def _changed(before, after):
    return {k: {"was": before.get(k), "now": after.get(k)} for k in CHANGED if before.get(k) != after.get(k)}


def use_item(client, item, button="Y", frames=PRESS_FRAMES):
    """`assign` the item (the bridge's `assign ITEM`), press `button` for `frames` frames and step as many, and return
    what changed in the state (life, rupees, event, prompt, equips, inventory). The button is a guess to confirm in the
    lane run: it is the one the item was assigned to."""
    before = pc.game_of(client.state())
    client.act("assign " + item)
    client.step(2)
    client.input(None, [button], frames)
    after = pc.game_of(client.step(frames))
    return {"item": item, "button": button, "changed": _changed(before, after), "frames": 2 + 2 * frames,
            "state": after}


def interact(client, what="talk", frames=PRESS_FRAMES):
    """Press A when the state's `prompt` says an action is there (talk to an NPC, open a chest or a door); returns the
    new prompt and event. With no prompt nothing is pressed."""
    before = pc.game_of(client.state())
    if not before.get("prompt"):
        return {"result": "nothing to %s here: the A prompt is empty" % what, "pressed": False,
                "prompt": before.get("prompt"), "event": before.get("event")}
    client.input(None, ["A"], frames)
    after = pc.game_of(client.step(frames))
    return {"result": "pressed A", "pressed": True, "prompt_was": before.get("prompt"), "prompt": after.get("prompt"),
            "event": after.get("event"), "frames": 2 * frames}


def strike(client, line, name, ident=None, frames=STRIKE_FRAMES):
    """Send one `kill [ID] NAME` or `hit [ID] NAME N` line (built and checked by mcp_playtest.act_line) and wait up to
    `frames` frames, looking every STRIKE_LOOK, for the objects walk's result in `state.strike`, matched by the strike's
    number (no retry, no other actor). `ident` (the id in the line) is only for the words. Replies result: `ok` (the walk
    struck it), `refused: actor 7 (NAME) is gone`, `refused: actor 7 (NAME) has no life left`, `refused: no world list ...`
    (solo refuses an id), or `unknown: no result after 12 frames` (the strike may still land: look with get_world)."""
    who = "%d " % ident if ident else ""
    label = "actor %s (%s)" % (who.strip(), name) if who else "an actor named %s" % name
    try:
        r = client.act(line)
    except pc.BridgeError as e:
        why = (e.reply or {}).get("message") or str(e)
        return {"result": "refused: " + why, "line": line}
    seq = r.get("strike_seq")
    if not seq:
        return {"result": "unknown: the bridge gave no strike number (an older module?)", "line": line}
    used, last = 0, None
    while True:
        last = client.state().get("strike")
        if isinstance(last, dict) and last.get("seq", 0) >= seq:
            break
        if used >= frames:
            return {"result": "unknown: no result after %d frames (the strike may still land: look with get_world)"
                    % frames, "line": line, "seq": seq}
        client.step(STRIKE_LOOK)
        used += STRIKE_LOOK
    if last.get("seq") != seq:
        return {"result": "unknown: a newer strike replaced the result", "line": line, "seq": seq}
    word = last.get("result")
    if word == "done":
        return {"result": "ok", "line": line, "seq": seq, "frames": used}
    if word == "gone":
        return {"result": "refused: %s is gone" % label, "line": line, "seq": seq, "frames": used}
    if word == "none with a life":
        return {"result": "refused: %s has no life left" % label if who else "refused: none of that name has a life",
                "line": line, "seq": seq, "frames": used}
    return {"result": "unknown: the walk said %r" % word, "line": line, "seq": seq, "frames": used}
