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
GRAB_FRAMES = 30          # A held this long on a block grabs it (a short tap climbs onto it instead)
GRAB_LEAN_FRAMES = 24     # at most this much leaning on the block, 4 frames a look, for the A prompt to come
JUMP_STEP_M = 0.1         # a run look that brings Link less than this nearer made no progress
FALL_M = 1.0              # landing this far below `land_y`: fell
RESPAWN_M = 3.0           # Link moved this far between two looks without running there: put back (a pit, a void)
ON_GROUND_M = 0.02        # two looks with the height this steady: on the ground
AT_LAND_M = 0.15          # jump: this near `land_y` (or one steady look) and at the point: down, the stick lets go
DRAW_FRAMES = 60          # ZR held to take the item out and draw (docs/testing.md, the slingshot scenario: 60, then 30)
HOLD_FRAMES = 30
RELEASE_FRAMES = 2
WATCH_FRAMES = 40         # after the release, the shot has this long to reach the target
WATCH_LOOK = 4
RETICLE = (0.0, 0.028)    # the slingshot's reticle on the screen (x right, y up, -1..1): about pixel 640,350 of 720p
AIM_TOL = 0.02            # the target within this of the reticle (about half a degree): aimed
AIM_STEPS = 40            # turns of the view, `AIM_FRAMES` each, before giving up
AIM_FRAMES = 2
AIM_PROBE = (0.5, 4)      # a probe of each stick axis: this strength for this many frames, to learn which way it turns
AIM_MOVED = 0.003         # a probe that moves the target less than this on the screen did not turn the view
AIM_MIN = 0.12            # the smallest stick that still turns the view (a dead zone)

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


def _refused(reply):
    """Why a `move` reply says no frame ran (an error such as `no_camera`, or an event), None when it ran."""
    if not isinstance(reply, dict):
        return None
    if reply.get("ok") is False or reply.get("no_camera") is True:
        return str(reply.get("error") or ("no_camera" if reply.get("no_camera") is True else "refused"))
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


def on_screen(view, point):
    """Where `point` [x, y, z] is on the screen of the state's `view` (its `clip`, clip = m * p): (x right, y up),
    -1..1 at the edges; None when it is behind the camera or there is no view."""
    if not isinstance(view, dict) or not isinstance(view.get("clip"), list) or len(view["clip"]) != 16:
        return None
    m = view["clip"]
    p = (float(point[0]), float(point[1]), float(point[2]), 1.0)
    c = [sum(m[4 * i + j] * p[j] for j in range(4)) for i in range(4)]
    if c[3] <= 1e-6:
        return None
    y = c[1] / c[3]
    return (c[0] / c[3], -y if view.get("y_down") else y)


def _aim_error(client, point):
    st = _st(client)
    s = on_screen(st.get("view"), point)
    return st, (None if s is None else (s[0] - RETICLE[0], s[1] - RETICLE[1]))


def _stick_of(x, y):
    """[angle, strength] for the stick axes x (right) and y (up), each -1..1."""
    strength = min(1.0, math.hypot(x, y))
    return [math.degrees(math.atan2(x, y)) % 360, strength]


def _turn(client, which, x, y, frames):
    stick = _stick_of(x, y)
    if which == "right":
        client.input(None, ["ZR"], frames, rstick=stick)
    else:
        client.input(stick, ["ZR"], frames)


def aim_view(client, point, out):
    """With ZR held (drawing), turn the view the way a player aims until `point` sits under the reticle. Which stick
    turns it and which way is learnt first: each axis of the right stick, then the left, is pushed a little and the
    target's move on the screen measured. Then each turn pushes the stick by how far the target is from the reticle.
    Returns None when aimed, else why not. Fills out["aim"] with the sticks, the steps and the last error."""
    aim = out["aim"]
    st, err = _aim_error(client, point)
    if err is None:
        return ("no camera view in the state" if not isinstance(st.get("view"), dict)
                else "the target is behind the camera while drawing")
    aim["error_start"] = [round(err[0], 3), round(err[1], 3)]
    gains, which = None, None
    strength, frames = AIM_PROBE
    for name in ("right", "left"):
        g = []
        for x, y in ((strength, 0.0), (0.0, strength)):
            _turn(client, name, x, y, frames)
            out["frames"] += frames
            st, now = _aim_error(client, point)
            if now is None:
                return "the target left the screen while learning the %s stick" % name
            g.append(((now[0] - err[0]) / (strength * frames), (now[1] - err[1]) / (strength * frames)))
            err = now
        if max(abs(v) for row in g for v in row) * strength * frames >= AIM_MOVED:
            gains, which = g, name
            break
    if gains is None:
        return "neither stick turned the view while drawing (the target stayed at %.3f %.3f)" % err
    aim["stick"] = which
    aim["gains"] = [[round(v, 4) for v in row] for row in gains]
    (ax, ay), (bx, by) = gains          # the screen move per frame for a full stick right (a) and up (b)
    det = ax * by - bx * ay
    if abs(det) < 1e-9:
        return "the %s stick turns the view only one way (gains %s)" % (which, aim["gains"])
    for step in range(AIM_STEPS):
        aim["error"] = [round(err[0], 3), round(err[1], 3)]
        aim["steps"] = step
        if math.hypot(*err) <= AIM_TOL:
            return None
        # The stick (x, y) whose screen move over AIM_FRAMES takes the error away: solve gains * (x, y) = -err.
        wx = -err[0] / AIM_FRAMES
        wy = -err[1] / AIM_FRAMES
        x = (wx * by - bx * wy) / det
        y = (ax * wy - wx * ay) / det
        n = math.hypot(x, y)
        if n > 1.0:
            x, y = x / n, y / n
        elif n < AIM_MIN:
            x, y = x / n * AIM_MIN, y / n * AIM_MIN
        _turn(client, which, x, y, AIM_FRAMES)
        out["frames"] += AIM_FRAMES
        st, now = _aim_error(client, point)
        if now is None:
            return "the target left the screen while aiming"
        err = now
    aim["error"] = [round(err[0], 3), round(err[1], 3)]
    aim["steps"] = AIM_STEPS
    if math.hypot(*err) <= AIM_TOL:
        return None
    return "the view did not reach the target in %d turns (it is %.3f %.3f from the reticle)" % (
        AIM_STEPS, err[0], err[1])


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
    """Take out `item` (slingshot, bow, boomerang, hookshot, longshot, bomb, nut: put on ZR with `assign`) and aim it
    the way a player does: turn Link to the target and tap ZL (the camera goes behind him), hold ZR to draw
    (`draw_frames`), then turn the view with a stick until the target sits under the reticle (`aim_view`, read from
    the state's `view`), hold `hold_frames`, check the aim once more, let go to fire, and watch the target for
    `watch_frames`. A target the view cannot reach is not fired at: B puts the item away and the reason is said.
    `tilt_frames` (+ up the stick, - down) tilts by hand after the aim, for a target that needs an arc.

    The target: an actor `ident` (+ `name`) from the world list, or a point `at` [x, y, z] (then nothing can be judged
    beyond `fired`). Results: `hit` (its life fell, it died or left the list), `changed` (another field it shows changed),
    `miss` (the ammo went down or the item left, the target did not change), `fired` (a point target, or an item with no
    ammo count: it left Link, nothing to judge), `not fired` (the ammo did not move: no ammo, a help box opened, the item
    is not Link's), `not aimed` (the view could not be turned onto it: no view, neither stick turned it, it left the
    screen, too far), or a stop reason (gone before the shot, no world list, scene changed, event, hurt). `aim` has
    the yaw and pitch from Link to the target, the stick that turned the view, the turns it took and the last `error`
    on the screen (x, y from the reticle; 1 is the screen's half). `assist` only nudges the last bit: the shot is
    moved sideways onto the target's line when its flight already points within 4 degrees of it."""
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

    client.act("assign %s" % assign_name)
    client.step(2)
    client.act("face %.2f %.2f" % (point[0], point[2]))
    client.step(2)
    client.input(None, ["ZL"], 2)     # the camera goes behind Link, so the draw (which turns him to it) keeps the way
    client.step(8)
    out["frames"] += 14
    if assist:
        out["assist_reply"] = client.act("aim %.2f %.2f %.2f" % tuple(point))
    client.input(None, ["ZR"], draw_frames)
    out["frames"] += draw_frames
    st = _st(client)
    stop = watch.stop(st)
    if stop:
        return _out(out, "not fired", stop + (" (a help box?)" if stop == "event" else "") + " while drawing", st)
    why = aim_view(client, point, out)
    if not why:
        if tilt_frames:
            client.input([0 if tilt_frames > 0 else 180, 1.0], ["ZR"], abs(int(tilt_frames)))
            out["frames"] += abs(int(tilt_frames))
        client.input(None, ["ZR"], hold_frames)
        out["frames"] += hold_frames
        st, err = _aim_error(client, point)
        if not tilt_frames and (err is None or math.hypot(*err) > AIM_TOL):   # the line through the target, once more
            why = "the aim drifted off before the let-go (%s)" % (
                "off the screen" if err is None else "%.3f %.3f from the reticle" % err)
    st = _st(client)
    stop = watch.stop(st)
    if stop:
        return _out(out, "not fired", stop + (" (a help box?)" if stop == "event" else "") + " while aiming", st)
    if why:
        client.input(None, ["ZR", "B"], 4)   # put it away: no shot at a target the view is not on
        client.step(4)
        out["frames"] += 8
        st = _st(client)
        return _out(out, "not aimed", why, st)
    client.input(None, [], RELEASE_FRAMES)
    out["frames"] += RELEASE_FRAMES
    used = out["frames"]

    seen, judged, why, ended = 0, None, None, None
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
        ended = watch.stop(st)
        if ended and ended != "event":
            break
        ended = None
    out["frames"] = used + seen
    ammo_after = ammo_of(st, ammo_item)
    out["ammo_after"] = ammo_after
    spent = (ammo_before is not None and ammo_after is not None and ammo_after < ammo_before)
    if judged:
        out["seen"] = why
        return _out(out, judged, "the target: " + why, st)
    if ended:
        return _out(out, "fired", ended + " after the shot (not judged)", st)
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
                if flat > 0.3 and moved_once and to_y is None:
                    return _out(out, "stopped", "Link stepped off the %s onto the ledge (%.2f m across at height %.2f)"
                                % ("top" if up else "bottom", flat, pos[1]), st)
                if flat > 0.3 and not moved_once:
                    return _out(out, "not climbing", "Link moved %.2f m across and %.2f m up or down: he is walking, "
                                "not on a ladder or vines (face the wall with `face`)" % (flat, dy), st)
                still += 1
            elif (dy > 0) != up:
                return _out(out, "not climbing", "the height went the wrong way (%.2f m): Link is not on the wall" % dy,
                            st)
            else:
                still, moved_once = 0, True
        if to_y is None and moved_once and still >= 2:   # not 4 looks of stick at the top: Link would walk off it
            return _out(out, "stopped", "the height stays at %.2f: the %s" % (pos[1], "top" if up else "bottom"), st)
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
    faces it and leans on it until A offers something (the game's "Grab"), holds A `GRAB_FRAMES` to grab (a short tap
    would climb onto it), then pushes with the stick at full, a look every `chunk` frames. `grab` says what A offered
    and whether it was held.

    Results: `pushed` (the block moved the distance), `stuck` (it did not move for `stuck_looks` looks while Link stayed
    on it: it is blocked, at its end, or not a block that moves), `slipped` (Link moved but the block did not: he slid
    off its side), `stopped` (the bridge refused the move: no camera, an event; no frame ran), `not reached` (the walk to the block's side stopped), or a stop (gone, no world list, scene changed,
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
    # The grab: lean on the block until A offers something, then hold A (a tap climbs onto the block). The stick
    # alone then pushes; without a prompt nothing is pressed and the stick leans on it as before.
    leaned = 0
    while ph.no_prompt(st.get("prompt")) and leaned < GRAB_LEAN_FRAMES:
        why = _refused(client.move(yaw, 1.0, 4, check=False))
        if why:
            return _out(out, "stopped", "move refused: " + why, st)
        leaned += 4
        out["frames"] += 4
        st = _st(client)
        stop = watch.stop(st)
        if stop:
            return _out(out, stop.split(" (")[0], stop, st)
    out["grab"] = {"prompt": st.get("prompt"), "held": False}
    if not ph.no_prompt(st.get("prompt")):
        client.input(None, ["A"], GRAB_FRAMES)
        out["frames"] += GRAB_FRAMES
        st = _st(client)
        out["grab"].update(held=True, after=st.get("prompt"))
        now = _pos(st)
        if now and link and now[1] > link[1] + 0.5:
            return _out(out, "stuck", "Link went up %.2f m: he climbed onto the block instead of grabbing it"
                        % (now[1] - link[1]), st)
        stop = watch.stop(st)
        if stop:
            return _out(out, stop.split(" (")[0], stop + " while grabbing", st)
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
        why = _refused(client.move(yaw, 1.0, n, check=False))
        if why:
            return _out(out, "stopped", "move refused: " + why, st)
        out["frames"] += n


# --- jump -------------------------------------------------------------------------------------------------------------

def jump(client, x, z, land_y=None, tolerance=0.6, max_frames=240, chunk=4, stuck_looks=STUCK_LOOKS):
    """Run straight at (x, z) at full stick, so Link jumps off the edge by himself (the game jumps a run off a ledge;
    a walk off it only drops), until he lands near the point. No detours: a side step at a ledge is a fall.

    `land_y`: the height of the far side. Results: `landed` (within `tolerance` of the point and on the ground, and
    not more than FALL_M below `land_y`), `fell` (on the ground more than FALL_M below `land_y`), `respawned` (Link was put back: a pit or a void; the life may be lower), `stuck` (no nearer for
    `stuck_looks` looks: a wall, or the edge does not jump), `stopped` (the bridge refused the move: no camera, an
    event; no frame ran), or a stop (scene changed, event, timed out). On the ground means two looks in a row with the
    height steady (ON_GROUND_M), so the top of the arc is not taken for a landing; within `tolerance` of the point and
    down (one steady look, or within AT_LAND_M of `land_y`) the stick lets go, so Link stops there and the landing is called two looks (2 x `chunk` frames) after he comes down. The trace
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
        if ground >= 2 and below:
            return _out(out, "fell", "on the ground at %.2f, %.2f below the landing" % (pos[1], land_y - pos[1]), st)
        if dist <= tolerance and ground >= 2:
            return _out(out, "landed", "at the point, height %.2f%s" % (pos[1], "" if air else " (no jump was seen)"),
                        st)
        if dist < best - JUMP_STEP_M or dist <= tolerance:   # at the point: waiting to land is not stuck
            best, still = min(best, dist), 0
        else:
            still += 1
        if still >= stuck_looks:
            return _out(out, "stuck", "no nearer than %.2f m for %d frames (a wall, or the edge is not a jump)" %
                        (best, still * chunk), st)
        if out["frames"] >= max_frames:
            return _out(out, "timed out", "%.2f m from the point" % dist, st)
        last, prev_y = pos, pos[1]
        n = min(chunk, max_frames - out["frames"])
        # At the point and down (one steady look, or at the height of `land_y`): let go of the stick, or Link runs on
        # past it before the second steady look. In the air the stick stays full: a point just past a gap's far edge
        # is within tolerance while he is still over the gap, and the game may not keep his speed with the stick at 0.
        down = ground >= 1 or (land_y is not None and abs(pos[1] - land_y) <= AT_LAND_M)
        hold = 0.0 if dist <= tolerance and down else 1.0
        why = _refused(client.move(pc.yaw_to(pos, x, z) % 360, hold, n, check=False))
        if why:
            return _out(out, "stopped", "move refused: " + why, st)
        out["frames"] += n
