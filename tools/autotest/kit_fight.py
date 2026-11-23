"""`fight`: lock on to one enemy and slash it until it is dead, Link is low, or the frames are used. Standard library
only; the caller adds the `look`. Built on the helpers in playtest_helpers (the enemy is looked up by id before every
round, and a scene change, a load or a stale list stops the fight before anything is judged).

    fight(client, ident, name)  ->  reply (result, enemy life before/after, Link's life, hits, hurt, rounds, hint)

The buttons are guesses made without a game: lock-on `ZL`, sword `Y`, shield `R`. A fight whose enemy life never moves
says so in `hint` (the buttons may be wrong); the first lane run settles them.
"""
import math

import playtest_client as pc
import playtest_helpers as ph

LOCK = "ZL"
SWORD = "Y"
SHIELD = "R"
REACH_M = 2.2        # the sword reaches about this far; farther than this Link walks up first
APPROACH_FRAMES = 12
SLASH_FRAMES = 6
SHIELD_FRAMES = 8
BACK_OFF_FRAMES = 14
MIN_LIFE = 4         # quarter hearts: at or below this Link backs off and the fight ends ("low")
MAX_FRAMES = 1200


def _xz(pos):
    return pos[0], pos[2]


def _dist(link, actor):
    p, q = _xz(link), actor.get("pos")
    if not isinstance(q, (list, tuple)) or len(q) < 3:
        return actor.get("dist")
    return math.hypot(p[0] - q[0], p[1] - q[2])


def _away_yaw(link, actor):
    """The world yaw (0 = +z, 90 = +x) that takes Link straight away from the actor."""
    q = actor["pos"]
    return math.degrees(math.atan2(link[0] - q[0], link[2] - q[2])) % 360


def _press(client, buttons, frames):
    client.input(None, list(buttons), frames)
    client.step(frames)
    return 2 * frames


def fight(client, ident, name=None, max_frames=MAX_FRAMES, lock=LOCK, sword=SWORD, shield=SHIELD, min_life=MIN_LIFE,
          reach_m=REACH_M):
    """Returns result: `dead` (its life was seen at 0), `gone` (it left the list: perhaps dead; the look says),
    `low` (Link's life fell to `min_life` or less: he backed off), `stopped` (the scene changed, a load began, an event
    or a stale list: see `stopped`), `alive` (the frame limit; `hint` when its life never moved). Also `life_before`,
    `life_after` (the enemy's), `link_before`, `link_after`, `hits` (rounds in which its life went down), `hurt`
    (rounds in which Link's did), `rounds`, `frames`."""
    if not 1 <= int(max_frames) <= MAX_FRAMES:
        raise ValueError("max_frames is 1 to %d" % MAX_FRAMES)
    a, w = ph._look(client, ident)
    why = ph._no_list(w)
    if why or ph._gone(a, name):
        return {"result": "stopped", "stopped": why or "gone", "rounds": 0, "frames": 0, "hits": 0, "hurt": 0}
    name = name or a.get("name")
    st = pc.game_of(client.state())
    scene = st.get("scene")
    before = a.get("life")
    out = {"result": "alive", "life_before": before, "life_after": before, "link_before": st.get("life"),
           "link_after": st.get("life"), "hits": 0, "hurt": 0, "rounds": 0, "frames": 0}
    if isinstance(before, (int, float)) and before <= 0:
        return dict(out, result="dead", note="its life was already 0")
    last_enemy, last_link, shielded = before, st.get("life"), False
    while out["frames"] < int(max_frames):
        stop = ph._interrupted(st, scene, w)
        if stop:
            return dict(out, result="stopped", stopped=stop)
        if ph._gone(a, name):
            return dict(out, result="gone")
        life = a.get("life")
        out["life_after"] = life
        if isinstance(life, (int, float)) and life <= 0:
            return dict(out, result="dead")
        mine = st.get("life")
        out["link_after"] = mine
        if isinstance(mine, (int, float)) and mine <= min_life:
            link = st.get("pos")
            if link and isinstance(a.get("pos"), (list, tuple)):
                client.move(_away_yaw(link, a), 1.0, BACK_OFF_FRAMES, check=False)
                out["frames"] += BACK_OFF_FRAMES
            return dict(out, result="low", note="Link's life is %s (a quarter heart is 1): he backed off" % mine)
        link, dist = st.get("pos"), None
        if link and isinstance(a.get("pos"), (list, tuple)):
            dist = _dist(link, a)
        hurt_now = isinstance(mine, (int, float)) and isinstance(last_link, (int, float)) and mine < last_link
        if dist is not None and dist > reach_m:
            q = a["pos"]
            r = pc.walk_to(client, q[0], q[2], tolerance=reach_m * 0.6, max_frames=APPROACH_FRAMES, chunk=APPROACH_FRAMES)
            out["frames"] += max(1, r.get("frames", 0))
            shielded = False
        elif hurt_now and not shielded:
            out["frames"] += _press(client, (lock, shield), SHIELD_FRAMES)    # block the next blow, then slash again
            shielded = True
        else:
            out["frames"] += _press(client, (lock, sword), SLASH_FRAMES)
            shielded = False
        out["rounds"] += 1
        a, w = ph._look(client, ident)
        st = pc.game_of(client.state())
        if isinstance(a, dict) and isinstance(a.get("life"), (int, float)) and isinstance(last_enemy, (int, float)) \
                and a["life"] < last_enemy:
            out["hits"] += 1
        if hurt_now:
            out["hurt"] += 1
        last_enemy = a.get("life") if isinstance(a, dict) else last_enemy
        last_link = mine
    stop = ph._interrupted(st, scene, w)
    if stop:
        return dict(out, result="stopped", stopped=stop)
    if ph._gone(a, name):
        return dict(out, result="gone")
    out["life_after"] = a.get("life")
    out["link_after"] = st.get("life")
    if isinstance(out["life_after"], (int, float)) and out["life_after"] <= 0:
        out["result"] = "dead"
    elif out["rounds"] and out["life_after"] == before:
        out["hint"] = ("its life never moved in %d rounds: the lock-on %s or sword %s button may be wrong (they are "
                       "guesses), the sword may not be out, or it cannot be hurt that way" % (out["rounds"], lock, sword))
    return out
