"""Moves the plain walk cannot make: `shoot`, `climb`, `push` and `jump`. Standard library only; the caller adds the `look`.
Built on the bridge's own commands (`state`, `dump` for the world list, `input`, `move`, `act`) and the helpers in
playtest_helpers. Each one is bounded (`max_frames`), stops on a stuck rule and says why in `reason`.

    shoot(client, item, ident=None, name=None, at=None, ...)  ->  result fired/hit/changed/miss/not fired, ammo, aim
    climb(client, up=True, to_y=None, ...)                    ->  result arrived/stuck/not climbing, height trace
    push(client, ident, distance_m, yaw=None, ...)            ->  result pushed/stuck/slipped, block moved
    jump(client, x, z, land_y=None, ...)                      ->  result landed/fell/stuck/respawned, height trace

Buttons are Longshot's own map (host/nn_hid.c; reports/kit-fight.md): the item on ZR (`assign` puts it there), the
lock-on on ZL, jump-attack on X, A for actions. The stick angle is 0 = away from the camera, 90 = right.

No new address is read: the target's life, place and `alive` come from the world list (the objects walk's reads, docs/
addresses.md), the ammo from the state's inventory (`PouchData.<item>.StockNum`), the height from Link's `pos`.

What the kit cannot see: the aim's pitch (the first-person view's tilt is not in the state), so `shoot` reports the yaw
and pitch it wanted and `tilt_frames` (stick frames up or down while aiming) is a hand control, not a measured angle.
`assist` uses the test `aim X Y Z` line (needs LICHII_COOP_TEST_COMMANDS): the module steers the next arrow or seed
that leaves Link onto the target, so a hit then says the target takes the shot, not that the aim was right.
"""
import math

import playtest_client as pc
import playtest_helpers as ph

VERBS = ("shoot", "climb", "push", "jump")
MAX_FRAMES = 600
CHUNK = 8                 # frames per push of the stick between looks
STUCK_LOOKS = 4           # this many looks in a row without progress: stuck
CLIMB_STEP_M = 0.05       # a climb look that changes the height less than this made no progress
PUSH_STEP_M = 0.05        # a block that moves less than this in a look did not move
PUSH_STUCK_LOOKS = 6      # a block starts to slide only after Link has leaned on it a while (about 20 frames)
PUSH_STAND_M = 1.2        # Link starts this far from the block's centre, on the side away from the push
PUSH_DONE_M = 0.15        # the block counts as pushed when it is this close to the distance asked
JUMP_STEP_M = 0.1         # a run look that brings Link less than this nearer made no progress
FALL_M = 1.0              # landing this far below `land_y`: fell
RESPAWN_M = 3.0           # Link moved this far between two looks without running there: put back (a pit, a void)
ON_GROUND_M = 0.02        # two looks with the height this steady: on the ground
DRAW_FRAMES = 60          # ZR held to take the item out and draw (docs/testing.md, the slingshot scenario: 60, then 30)
HOLD_FRAMES = 30
RELEASE_FRAMES = 2
WATCH_FRAMES = 40         # after the release, the shot has this long to reach the target
WATCH_LOOK = 4

ITEMS = {                 # the name a tester types -> (the `assign` name, the ammo item or None)
    "slingshot": ("SlingShot", "Item_DekuSeed"),
    "bow": ("Bow", "Item_Arrow"),
    "boomerang": ("Boomerang", None),
    "hookshot": ("Hookshot", None),
    "longshot": ("Longshot", None),
    "bomb": ("Bomb", "Item_Bomb"),
    "nut": ("DekuNut", "Item_DekuNut"),
}


def _st(client):
    return pc.game_of(client.state())


def _pos(st):
    p = st.get("pos")
    return list(p) if isinstance(p, (list, tuple)) and len(p) >= 3 else None


def _life(st):
    v = st.get("life")
    return v if isinstance(v, (int, float)) else None


class _Watch:
    """What stops every move: the scene changed, a load, an event, Link hurt or down. `hurt_ok` lets a hurt pass."""

    def __init__(self, st, hurt_ok=False):
        self.scene, self.life, self.hurt_ok = st.get("scene"), _life(st), hurt_ok

    def stop(self, st):
        if st.get("scene") != self.scene:
            return "scene changed"
        if st.get("loading"):
            return "loading"
        life = _life(st)
        if life is not None and life <= 0:
            return "Link is down"
        if self.life is not None and life is not None and life < self.life and not self.hurt_ok:
            return "hurt (life %s -> %s)" % (self.life, life)
        if st.get("event"):
            return "event"
        return None


def _base(st):
    return {"frames": 0, "start": _pos(st), "trace": []}


def _out(out, result, reason, st):
    out.update(result=result, reason=reason, pos=_pos(st), life=_life(st))
    out["trace"] = out["trace"][-pc.TRACE_KEEP:]
    return out


# --- shoot ------------------------------------------------------------------------------------------------------------

def ammo_of(st, ammo_item):
    """The count of `ammo_item` (`Item_DekuSeed`) in the state's inventory, or None when the state has none."""
    inv = st.get("inventory")
    if not ammo_item or not isinstance(inv, dict):
        return None
    for key in ("PouchData.%s.StockNum" % ammo_item, ammo_item):
        v = inv.get(key)
        if isinstance(v, (int, float)):
            return v
    return 0 if inv else None


def aim_angles(link, target):
    """(world yaw, pitch) in degrees from Link's place to the target's: yaw 0 = +z, 90 = +x; pitch up is positive.
    Link's place is his feet: the shot leaves about a metre higher (child; adult higher still), which `pitch` leaves out."""
    yaw = pc.yaw_to(link, target[0], target[2]) % 360
    flat = math.hypot(target[0] - link[0], target[2] - link[2])
    pitch = math.degrees(math.atan2(target[1] - link[1], flat)) if flat > 1e-6 else (90.0 if target[1] > link[1] else -90.0)
    return round(yaw, 1), round(pitch, 1)


def _judge(before, after):
    """hit: its life fell, it died or it left the list; changed: another field it shows changed (a switch, an eye);
    None: nothing seen."""
    if after is None:
        return "hit", "gone from the list"
    lb, la = before.get("life"), after.get("life")
    if isinstance(lb, (int, float)) and isinstance(la, (int, float)) and la < lb:
        return "hit", "life %s -> %s" % (lb, la)
    if before.get("alive") and after.get("alive") is False:
        return "hit", "no longer alive"
    keys = [k for k in ("alive", "kind", "name", "owner") if before.get(k) != after.get(k)]
    if keys:
        return "changed", ", ".join("%s %s -> %s" % (k, before.get(k), after.get(k)) for k in keys)
    return None, None


def shoot(client, item, ident=None, name=None, at=None, assist=False, tilt_frames=0, draw_frames=DRAW_FRAMES,
          hold_frames=HOLD_FRAMES, watch_frames=WATCH_FRAMES):
    """Take out `item` (slingshot, bow, boomerang, hookshot, longshot, bomb, nut: put on ZR with `assign`), turn Link to
    the target, hold ZR to draw (`draw_frames` + `hold_frames`), tilt the aim with the stick for `tilt_frames` (+ up the
    stick, - down; how far a frame tilts is not measured), let go to fire, then watch the target for `watch_frames`.

    The target: an actor `ident` (+ `name`) from the world list, or a point `at` [x, y, z] (then nothing can be judged
    beyond `fired`). Results: `hit` (its life fell, it died or left the list), `changed` (another field it shows changed),
    `miss` (the ammo went down or the item left, the target did not change), `fired` (a point target, or an item with no
    ammo count: it left Link, nothing to judge), `not fired` (the ammo did not move: no ammo, a help box opened, the item
    is not Link's), or a stop reason (gone before the shot, no world list, scene changed, event, hurt). `aim` has the
    yaw and pitch from Link to the target (the yaw is what `face` turned to)."""
    key = str(item or "").strip().lower()
    if key not in ITEMS:
        raise ValueError("item: one of %s" % ", ".join(ITEMS))
    assign_name, ammo_item = ITEMS[key]
    if (ident is None) == (at is None):
        raise ValueError("shoot: give the target as `id` (+ `name`) or as `at` [x, y, z], one of them")
    st = _st(client)
    out = _base(st)
    out.update(item=key, assist=bool(assist), target=None, aim=None)
    link = _pos(st)
    if link is None:
        return _out(out, "not fired", pc.no_position_reason(st), st)
    actor = None
    if ident is not None:
        actor, w = ph._look(client, ident)
        stop = ph._no_list(w)
        if stop:
            return _out(out, "not fired", stop, st)
        if ph._gone(actor, name):
            return _out(out, "not fired", "gone", st)
        name = name or actor.get("name")
        point = actor.get("pos")
        if not isinstance(point, (list, tuple)) or len(point) < 3:
            return _out(out, "not fired", "the target has no position", st)
        out["target"] = {"id": actor.get("id"), "name": name, "life": actor.get("life"), "alive": actor.get("alive")}
    else:
        point = [float(v) for v in at]
        if len(point) != 3:
            raise ValueError("at: [x, y, z]")
        out["target"] = {"at": point}
    yaw, pitch = aim_angles(link, point)
    out["aim"] = {"yaw": yaw, "pitch": pitch, "distance_m": round(math.dist(link, point), 2),
                  "pitch_read": False, "note": "the pitch is what the shot needs; the game's aim tilt is not readable"}
    watch = _Watch(st)
    ammo_before = ammo_of(st, ammo_item)
    out["ammo_before"] = ammo_before
    if ammo_item and ammo_before == 0:
        return _out(out, "not fired", "no ammo (%s is 0)" % ammo_item, st)

    used = 0
    client.act("assign %s" % assign_name)
    client.step(2)
    client.act("face %.2f %.2f" % (point[0], point[2]))
    client.step(2)
    used += 4
    if assist:
        out["assist_reply"] = client.act("aim %.2f %.2f %.2f" % tuple(point))
    client.input(None, ["ZR"], draw_frames)
    used += draw_frames
    if tilt_frames:
        client.input([0 if tilt_frames > 0 else 180, 1.0], ["ZR"], abs(int(tilt_frames)))
        used += abs(int(tilt_frames))
    client.input(None, ["ZR"], hold_frames)
    used += hold_frames
    st = _st(client)
    stop = watch.stop(st)
    if stop:
        out["frames"] = used
        return _out(out, "not fired", stop + (" (a help box?)" if stop == "event" else "") + " while drawing", st)
    client.input(None, [], RELEASE_FRAMES)
    used += RELEASE_FRAMES

    seen, judged, why = 0, None, None
    while seen < watch_frames:
        n = min(WATCH_LOOK, watch_frames - seen)
        client.step(n)
        seen += n
        st = _st(client)
        if actor is not None:
            now, w = ph._look(client, ident)
            stop = ph._interrupted(st, watch.scene, w)
            if stop:
                out["frames"] = used + seen
                return _out(out, "fired", stop + " after the shot (not judged)", st)
            if now is not None and name is not None and now.get("name") != name:
                now = None
            judged, why = _judge(actor, now)
            out["trace"].append([used + seen, now.get("life") if now else None])
            if judged == "hit":
                break
        if watch.stop(st) and watch.stop(st) != "event":
            break
    out["frames"] = used + seen
    ammo_after = ammo_of(st, ammo_item)
    out["ammo_after"] = ammo_after
    spent = (ammo_before is not None and ammo_after is not None and ammo_after < ammo_before)
    if judged:
        out["seen"] = why
        return _out(out, judged, "the target: " + why, st)
    if ammo_item and ammo_before is not None and ammo_after is not None and not spent:
        return _out(out, "not fired", "the ammo did not go down (%s %s): the item did not leave Link" %
                    (ammo_item, ammo_after), st)
    if actor is None or not ammo_item:
        return _out(out, "fired", "nothing to judge" if actor is None else
                    "the item has no ammo count; the target did not change", st)
    return _out(out, "miss", "the ammo went down (%s -> %s); the target did not change in %d frames" %
                (ammo_before, ammo_after, seen), st)


# --- climb ------------------------------------------------------------------------------------------------------------

def climb(client, up=True, to_y=None, face=None, max_frames=MAX_FRAMES, chunk=CHUNK, stuck_looks=STUCK_LOOKS):
    """Climb a ladder or vines up (`up` true) or down with the stick held up or down (0 or 180), `chunk` frames a look,
    until Link's height reaches `to_y` (metres; none: until the height stops moving, which is the top or the bottom).
    `face` [x, z]: turn Link there first (the wall) and press toward it to grab on. Down from a ledge: when A offers
    something (`prompt`) it is pressed first (the game's "climb down").

    Results: `arrived` (at `to_y`), `stopped` (no `to_y`: the height stayed for `stuck_looks` looks after it moved:
    the top or the bottom), `stuck` (`to_y` given and the height stayed for `stuck_looks` looks: Link is not on it, or
    something is in the way), `not climbing` (Link moved across, not up or down: he is walking, not on the wall), or a
    stop (scene changed, a load, an event, hurt: a Skulltula on the vines, timed out)."""
    st = _st(client)
    out = _base(st)
    pos = _pos(st)
    if pos is None:
        return _out(out, "stuck", pc.no_position_reason(st), st)
    out["from_y"] = round(pos[1], 2)
    if to_y is not None and ((up and to_y <= pos[1]) or (not up and to_y >= pos[1])):
        return _out(out, "arrived", "already %s %.2f" % ("above" if up else "below", to_y), st)
    watch = _Watch(st)
    if face:
        client.act("face %.2f %.2f" % (face[0], face[1]))
        client.step(2)
        out["frames"] += 2
    if not up and st.get("prompt"):
        client.input(None, ["A"], 2)
        out["frames"] += 2
        out["pressed_a"] = st.get("prompt")
    stick = [0, 1.0] if up else [180, 1.0]
    still, moved_once, last = 0, False, None
    while True:
        st = _st(client)
        pos = _pos(st)
        if pos is None:
            return _out(out, "stuck", pc.no_position_reason(st), st)
        out["trace"].append([out["frames"], round(pos[1], 2)])
        if to_y is not None and ((up and pos[1] >= to_y - 0.1) or (not up and pos[1] <= to_y + 0.1)):
            return _out(out, "arrived", "height %.2f" % pos[1], st)
        stop = watch.stop(st)
        if stop:
            return _out(out, stop.split(" (")[0], stop, st)
        if last is not None:
            dy = pos[1] - last[1]
            flat = math.hypot(pos[0] - last[0], pos[2] - last[2])
            if abs(dy) < CLIMB_STEP_M:
                if flat > 0.3 and not moved_once:
                    return _out(out, "not climbing", "Link moved %.2f m across and %.2f m up or down: he is walking, "
                                "not on a ladder or vines (face the wall with `face`)" % (flat, dy), st)
                still += 1
            elif (dy > 0) != up:
                return _out(out, "not climbing", "the height went the wrong way (%.2f m): Link is not on the wall" % dy,
                            st)
            else:
                still, moved_once = 0, True
        if still >= stuck_looks:
            if to_y is None and moved_once:
                return _out(out, "stopped", "the height stays at %.2f: the %s" % (pos[1], "top" if up else "bottom"),
                            st)
            return _out(out, "stuck", "the height stayed at %.2f for %d looks (%d frames)%s" % (
                pos[1], still, still * chunk, "" if moved_once else ": Link never climbed (not on a ladder or vines?)"),
                st)
        if out["frames"] >= max_frames:
            return _out(out, "timed out", "after %d frames at height %.2f" % (out["frames"], pos[1]), st)
        last = pos
        n = min(chunk, max_frames - out["frames"])
        client.input(stick, [], n)
        out["frames"] += n


# --- push -------------------------------------------------------------------------------------------------------------

def push(client, ident, distance_m, name=None, yaw=None, max_frames=MAX_FRAMES, chunk=CHUNK,
         stuck_looks=PUSH_STUCK_LOOKS, stand_m=PUSH_STAND_M):
    """Push the block `ident` (+ `name`, from the world list) `distance_m` metres along world yaw `yaw` (none: the
    direction from Link to the block). Link walks to the block's far side from the push (`stand_m` from its centre),
    faces it and leans on it with the stick at full, a look every `chunk` frames.

    Results: `pushed` (the block moved the distance), `stuck` (it did not move for `stuck_looks` looks while Link stayed
    on it: it is blocked, at its end, or not a block that moves), `slipped` (Link moved but the block did not: he slid
    off its side), `not reached` (the walk to the block's side stopped), or a stop (gone, no world list, scene changed,
    event, hurt, timed out). `moved_m` is how far the block went along the push."""
    st = _st(client)
    out = _base(st)
    out.update(moved_m=0.0, block=None)
    a, w = ph._look(client, ident)
    stop = ph._no_list(w)
    if stop:
        return _out(out, "stuck", stop, st)
    if ph._gone(a, name):
        return _out(out, "gone", "gone", st)
    name = name or a.get("name")
    start = a.get("pos")
    link = _pos(st)
    if link is None or not start:
        return _out(out, "stuck", pc.no_position_reason(st) if link is None else "the block has no position", st)
    if yaw is None:
        yaw = pc.yaw_to(link, start[0], start[2]) % 360
    yaw = float(yaw) % 360
    dx, dz = math.sin(math.radians(yaw)), math.cos(math.radians(yaw))
    out.update(block={"id": a.get("id"), "name": name, "from": start}, yaw=round(yaw, 1))
    stand = (start[0] - dx * stand_m, start[2] - dz * stand_m)
    r = pc.walk_to(client, stand[0], stand[1], tolerance=0.3, max_frames=min(240, max_frames))
    out["frames"] += r.get("frames", 0)
    st = pc.game_of(r["state"]) if isinstance(r.get("state"), dict) else _st(client)
    if not r.get("arrived"):
        return _out(out, "not reached", "the walk to the block's side: " + str(r.get("reason")), st)
    client.act("face %.2f %.2f" % (start[0], start[2]))
    client.step(2)
    out["frames"] += 2
    st = _st(client)
    watch = _Watch(st)
    still, last_moved, last_link = 0, 0.0, _pos(st)
    while True:
        a, w = ph._look(client, ident)
        st = _st(client)
        stop = ph._interrupted(st, watch.scene, w) or watch.stop(st)
        if stop:
            return _out(out, stop.split(" (")[0], stop, st)
        if ph._gone(a, name):
            return _out(out, "gone", "the block left the list", st)
        p = a.get("pos") or start
        moved = (p[0] - start[0]) * dx + (p[2] - start[2]) * dz
        out["moved_m"] = round(moved, 2)
        link = _pos(st)
        out["trace"].append([out["frames"], round(moved, 2)])
        if moved >= distance_m - PUSH_DONE_M:
            return _out(out, "pushed", "the block moved %.2f m" % moved, st)
        if moved - last_moved < PUSH_STEP_M:
            still += 1
            if link and last_link and math.hypot(link[0] - p[0], link[2] - p[2]) > stand_m + 1.0:
                return _out(out, "slipped", "Link is %.1f m from the block and it did not move: he slid off its side"
                            % math.hypot(link[0] - p[0], link[2] - p[2]), st)
        else:
            still = 0
        if still >= stuck_looks:
            return _out(out, "stuck", "the block did not move for %d frames (moved %.2f m of %.2f): blocked, at its "
                        "end, or not a block that moves" % (still * chunk, moved, distance_m), st)
        if out["frames"] >= max_frames:
            return _out(out, "timed out", "the block moved %.2f m of %.2f" % (moved, distance_m), st)
        last_moved, last_link = moved, link
        n = min(chunk, max_frames - out["frames"])
        client.move(yaw, 1.0, n, check=False)
        out["frames"] += n


# --- jump -------------------------------------------------------------------------------------------------------------

def jump(client, x, z, land_y=None, tolerance=0.6, max_frames=240, chunk=4, stuck_looks=STUCK_LOOKS):
    """Run straight at (x, z) at full stick, so Link jumps off the edge by himself (the game jumps a run off a ledge;
    a walk off it only drops), until he lands near the point. No detours: a side step at a ledge is a fall.

    `land_y`: the height of the far side. Results: `landed` (within `tolerance` of the point and on the ground, and
    not more than FALL_M below `land_y`), `fell` (on the ground more than FALL_M below `land_y`, or past the point and
    below it), `respawned` (Link was put back: a pit or a void; the life may be lower), `stuck` (no nearer for
    `stuck_looks` looks: a wall, or the edge does not jump), or a stop (scene changed, event, timed out). The trace
    has [frames, x, y, z] at each look."""
    st = _st(client)
    out = _base(st)
    pos = _pos(st)
    if pos is None:
        return _out(out, "stuck", pc.no_position_reason(st), st)
    watch = _Watch(st, hurt_ok=True)
    out.update(target=[x, z], land_y=land_y, from_y=round(pos[1], 2))
    still, last, ground, prev_y, best = 0, pos, 0, pos[1], math.hypot(x - pos[0], z - pos[2])
    air = False
    while True:
        st = _st(client)
        pos = _pos(st)
        if pos is None:
            return _out(out, "stuck", pc.no_position_reason(st), st)
        out["trace"].append([out["frames"], round(pos[0], 2), round(pos[1], 2), round(pos[2], 2)])
        dist = math.hypot(x - pos[0], z - pos[2])
        stop = watch.stop(st)
        if stop and stop != "event":
            return _out(out, stop.split(" (")[0], stop, st)
        jumped = math.hypot(pos[0] - last[0], pos[2] - last[2])
        if out["frames"] > 0 and jumped > RESPAWN_M:
            return _out(out, "respawned", "Link was put back %.1f m away (a pit or a void)" % jumped, st)
        if stop == "event":
            return _out(out, "event", "event", st)
        if abs(pos[1] - prev_y) > ON_GROUND_M:
            air, ground = True, 0
        else:
            ground += 1
        out["airborne"] = out.get("airborne") or air
        below = land_y is not None and pos[1] < land_y - FALL_M
        if ground >= 1 and below:
            return _out(out, "fell", "on the ground at %.2f, %.2f below the landing" % (pos[1], land_y - pos[1]), st)
        if dist <= tolerance and ground >= 1:
            return _out(out, "landed", "at the point, height %.2f%s" % (pos[1], "" if air else " (no jump was seen)"),
                        st)
        if dist < best - JUMP_STEP_M:
            best, still = dist, 0
        else:
            still += 1
        if still >= stuck_looks:
            return _out(out, "stuck", "no nearer than %.2f m for %d frames (a wall, or the edge is not a jump)" %
                        (best, still * chunk), st)
        if out["frames"] >= max_frames:
            return _out(out, "timed out", "%.2f m from the point" % dist, st)
        last, prev_y = pos, pos[1]
        n = min(chunk, max_frames - out["frames"])
        client.move(pc.yaw_to(pos, x, z) % 360, 1.0, n, check=False)
        out["frames"] += n
