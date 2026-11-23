"""Where the test games' windows go: one monitor, chosen by its device name, and no focus stolen.

Every test run that starts games (tools/autotest/run.ps1 for the emulator, hookshot.py run for Longshot, which is also
what the live play-test kit and live.py start use) puts the game windows through this module, so a test game never
opens over what someone is working on.

The monitor is a device name, for example \\\\.\\DISPLAY2 (Settings > System > Display shows the number; "DISPLAY2" alone
works too). Pick it, strongest first:
  1. --display (hookshot.py run, live.py start, placement.py) or -Display (run.ps1, live_start.ps1);
  2. the environment variable OOT_TEST_DISPLAY;
  3. the default, \\\\.\\DISPLAY2.
The name is resolved to the monitor's work area (the screen minus the taskbar) when the games start, never as a
fixed 0,0, so a display added later, or the same display after the layout moved, only needs the setting changed. A name
that no monitor has is an error that lists the monitors found (the run does not fall back to another screen: that is
where the windows would pop up over someone's work).

Windows are placed side by side from the work area's top left, 960x600 each (smaller when the work area is too small,
so every window is fully on that monitor). Placing never activates a window (SWP_NOACTIVATE) and never restores a
minimized one with a call that activates it.

Command line (needs Windows for anything but `rects`):
  python placement.py list
  python placement.py rects [--display NAME] [--count N]
  python placement.py place --pid PID [--pid PID ...] [--display NAME] [--topmost] [--wait SECONDS] [--first-slot N --slots N]
  python placement.py launch --cwd DIR -- EXE [ARGS...]     (starts EXE without activating its window; prints its pid)
"""
import argparse
import json
import os
import subprocess
import sys
import time

ENV = "OOT_TEST_DISPLAY"
DEFAULT_DISPLAY = "\\\\.\\DISPLAY2"
WIN_W, WIN_H = 960, 600

SWP_NOACTIVATE = 0x0010
SWP_NOOWNERZORDER = 0x0200
SWP_SHOWWINDOW = 0x0040
HWND_TOPMOST = -1
HWND_NOTOPMOST = -2
SW_SHOWNOACTIVATE = 4


class PlacementError(Exception):
    pass


def popen_flags():
    """Popen keywords for a game: its own process group, and (Windows) its window shown without being activated.
    STARTF_USESHOWWINDOW with SW_SHOWNOACTIVATE is what a game's first ShowWindow(SW_SHOWNORMAL / SHOWDEFAULT) uses, so
    a test game opening does not take the focus from the window being worked in. A game that activates itself
    (SetForegroundWindow, a raise) is not stopped by this: the lane check shows whether Longshot does."""
    if os.name != "nt":
        return {"start_new_session": True}
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    si.wShowWindow = SW_SHOWNOACTIVATE
    return {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP, "startupinfo": si}


def display_name(arg=None, env=None):
    """The chosen device name: arg, else $OOT_TEST_DISPLAY, else the default."""
    env = os.environ if env is None else env
    for v in (arg, env.get(ENV), DEFAULT_DISPLAY):
        if v and v.strip():
            return v.strip()


def normalize(name):
    """'DISPLAY2', 'display2' and '\\\\.\\DISPLAY2' are the same monitor: the comparison key."""
    n = name.strip().upper()
    if n.startswith("\\\\.\\"):
        n = n[4:]
    return n


def find_monitor(monitors, name):
    """The monitor dict (name, work=(l, t, r, b), primary) called name, or a PlacementError that lists those found."""
    want = normalize(name)
    for m in monitors:
        if normalize(m["name"]) == want:
            return m
    found = ", ".join("%s (%dx%d at %d,%d%s)" % (m["name"], m["work"][2] - m["work"][0], m["work"][3] - m["work"][1],
                                                  m["work"][0], m["work"][1], ", primary" if m.get("primary") else "")
                      for m in monitors) or "none"
    raise PlacementError("no monitor called %s (monitors found: %s). Set the monitor with --display / -Display or %s."
                         % (name, found, ENV))


def layout(work, count, w=WIN_W, h=WIN_H):
    """count window rects (x, y, w, h) side by side from the work area's top left, all inside work (l, t, r, b).
    A window is w x h; when the area is narrower than count * w, or shorter than h, the windows shrink to fit."""
    left, top, right, bottom = work
    aw, ah = right - left, bottom - top
    if aw <= 0 or ah <= 0 or count <= 0:
        raise PlacementError("the work area %s cannot hold %d windows" % (work, count))
    w = min(w, aw // count)
    h = min(h, ah)
    return [(left + i * w, top, w, h) for i in range(count)]


# ---- Windows (ctypes; imported only where used so the placement math tests run anywhere) ----

def _user32():
    import ctypes
    from ctypes import wintypes
    u = ctypes.WinDLL("user32", use_last_error=True)
    u.GetMonitorInfoW.argtypes = [wintypes.HANDLE, ctypes.c_void_p]
    u.SetWindowPos.argtypes = [wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                               wintypes.UINT]
    u.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
    u.IsIconic.argtypes = [wintypes.HWND]
    u.IsWindowVisible.argtypes = [wintypes.HWND]
    u.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
    u.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    u.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    return u


def monitors():
    """The monitors now: [{name, work=(l, t, r, b), bounds=(l, t, r, b), primary}], in real pixels."""
    import ctypes
    from ctypes import wintypes
    u = _user32()
    try:
        u.SetProcessDPIAware()   # real pixels, not a scaled desktop's units (run.ps1 does the same)
    except OSError:
        pass

    class MONITORINFOEXW(ctypes.Structure):
        _fields_ = [("cbSize", wintypes.DWORD), ("rcMonitor", wintypes.RECT), ("rcWork", wintypes.RECT),
                    ("dwFlags", wintypes.DWORD), ("szDevice", ctypes.c_wchar * 32)]

    out = []
    PROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HANDLE, wintypes.HDC, ctypes.POINTER(wintypes.RECT),
                              wintypes.LPARAM)

    def cb(hmon, hdc, rect, lparam):
        mi = MONITORINFOEXW()
        mi.cbSize = ctypes.sizeof(MONITORINFOEXW)
        if u.GetMonitorInfoW(hmon, ctypes.byref(mi)):
            r, w = mi.rcMonitor, mi.rcWork
            out.append({"name": mi.szDevice, "bounds": (r.left, r.top, r.right, r.bottom),
                        "work": (w.left, w.top, w.right, w.bottom), "primary": bool(mi.dwFlags & 1)})
        return True

    u.EnumDisplayMonitors.argtypes = [wintypes.HDC, ctypes.c_void_p, PROC, wintypes.LPARAM]
    u.EnumDisplayMonitors(None, None, PROC(cb), 0)
    return sorted(out, key=lambda m: m["name"])


def game_window(pid):
    """The game's own window of process pid: the largest visible top-level window that is not the co-op cheats window
    (with LICHII_COOP_CHEATS=1 that window can be the one Windows reports first), or 0."""
    import ctypes
    from ctypes import wintypes
    u = _user32()
    best = [0, 0]
    PROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def cb(h, lparam):
        p = wintypes.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value != pid or not u.IsWindowVisible(h):
            return True
        buf = ctypes.create_unicode_buffer(256)
        u.GetWindowTextW(h, buf, 256)
        if buf.value.startswith("Co-op item cheats"):
            return True
        r = wintypes.RECT()
        u.GetClientRect(h, ctypes.byref(r))
        area = (r.right - r.left) * (r.bottom - r.top)
        if area > best[1]:
            best[0], best[1] = h, area
        return True

    u.EnumWindows.argtypes = [PROC, wintypes.LPARAM]
    u.EnumWindows(PROC(cb), 0)
    return best[0]


def place_window(hwnd, rect, topmost=False):
    """Moves and sizes the window to rect (x, y, w, h) without activating it. Topmost only when asked (a screen copy
    needs the game above other windows; it stays on the chosen monitor, so it does not cover anything elsewhere)."""
    u = _user32()
    if u.IsIconic(hwnd):
        u.ShowWindow(hwnd, SW_SHOWNOACTIVATE)   # (never SW_RESTORE: it activates)
    x, y, w, h = rect
    if not u.SetWindowPos(hwnd, HWND_TOPMOST if topmost else HWND_NOTOPMOST, x, y, w, h,
                          SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW):
        raise PlacementError("SetWindowPos failed for window 0x%x" % hwnd)


# ---- A copy of a game window's client area (live.py shot --window) ----
# Longshot's control-bridge shot is the game's own picture only: Longshot's Insert menu, and with it every co-op page
# (Multiplayer, Customize, Mods: tabs of that menu, hookshot patch 0012), is drawn over the window after that copy is
# taken. A copy of the window is what the player sees.

def png_bytes(w, h, rgb):
    """A PNG (8-bit RGB) of w x h pixels from rgb: w*h*3 bytes, rows top to bottom."""
    import struct
    import zlib
    if w <= 0 or h <= 0 or len(rgb) != w * h * 3:
        raise ValueError("png_bytes: %d bytes for %dx%d" % (len(rgb), w, h))
    stride = w * 3
    raw = b"".join(b"\x00" + bytes(rgb[y * stride:(y + 1) * stride]) for y in range(h))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def bgra_to_rgb(bgra):
    """GDI's 32-bit BGRA pixels as RGB bytes."""
    out = bytearray(len(bgra) // 4 * 3)
    out[0::3], out[1::3], out[2::3] = bgra[2::4], bgra[1::4], bgra[0::4]
    return bytes(out)


def is_dark(rgb, w, h):
    """True when every one of 8x8 sample points is near black (PrintWindow's answer for a window it cannot copy;
    run.ps1's Dark does the same test)."""
    for y in range(h // 8, h, max(1, h // 8)):
        for x in range(w // 8, w, max(1, w // 8)):
            i = (y * w + x) * 3
            if rgb[i] + rgb[i + 1] + rgb[i + 2] > 30:
                return False
    return True


def sample_points(rect, n=4):
    """n x n points spread inside rect (l, t, r, b), screen pixels: where a screen copy is checked."""
    l, t, r, b = rect
    return [(l + (r - l) * (2 * i + 1) // (2 * n), t + (b - t) * (2 * j + 1) // (2 * n))
            for j in range(n) for i in range(n)]


def screen_copy_problem(rect, monitor_rects, owner_at):
    """Why a screen copy of rect would not be the window ('' when it would be): a part off every monitor, or another
    window over it at a sample point. owner_at(x, y): the top-level window at that point (compared by the caller)."""
    l, t, r, b = rect
    if r <= l or b <= t:
        return "the window has no client area (minimized?)"
    if not any(ml <= l and mt <= t and r <= mr and b <= mb for ml, mt, mr, mb in monitor_rects):
        return "the window is not wholly on one monitor"
    for x, y in sample_points(rect):
        if not owner_at(x, y):
            return "another window covers it at %d,%d" % (x, y)
    return ""


def window_shot(hwnd, path):
    """Copies the client area of window hwnd into path (PNG). PrintWindow(PW_CLIENTONLY|PW_RENDERFULLCONTENT) first;
    when that gives black (some Vulkan windows) a copy of the screen there, but only when the window is wholly on a
    monitor and nothing covers it. Returns 'print' or 'screen'; raises PlacementError when no true copy can be made
    (nothing is written then)."""
    import ctypes
    from ctypes import wintypes
    u = _user32()
    g = ctypes.WinDLL("gdi32", use_last_error=True)
    try:
        u.SetProcessDPIAware()
    except OSError:
        pass
    u.ClientToScreen.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.POINT)]
    u.GetDC.argtypes = [wintypes.HWND]
    u.GetDC.restype = wintypes.HDC
    u.ReleaseDC.argtypes = [wintypes.HWND, wintypes.HDC]
    u.PrintWindow.argtypes = [wintypes.HWND, wintypes.HDC, wintypes.UINT]
    u.WindowFromPoint.argtypes = [wintypes.POINT]
    u.WindowFromPoint.restype = wintypes.HWND
    u.GetAncestor.argtypes = [wintypes.HWND, wintypes.UINT]
    u.GetAncestor.restype = wintypes.HWND
    g.CreateCompatibleDC.argtypes = [wintypes.HDC]
    g.CreateCompatibleDC.restype = wintypes.HDC
    g.CreateCompatibleBitmap.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int]
    g.CreateCompatibleBitmap.restype = wintypes.HBITMAP
    g.SelectObject.argtypes = [wintypes.HDC, wintypes.HGDIOBJ]
    g.SelectObject.restype = wintypes.HGDIOBJ
    g.BitBlt.argtypes = [wintypes.HDC, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.HDC,
                         ctypes.c_int, ctypes.c_int, wintypes.DWORD]
    g.GetDIBits.argtypes = [wintypes.HDC, wintypes.HBITMAP, wintypes.UINT, wintypes.UINT, ctypes.c_void_p,
                            ctypes.c_void_p, wintypes.UINT]
    g.DeleteObject.argtypes = [wintypes.HGDIOBJ]
    g.DeleteDC.argtypes = [wintypes.HDC]

    if not hwnd or u.IsIconic(hwnd):
        raise PlacementError("no window to copy (none found, or minimized)")
    r = wintypes.RECT()
    u.GetClientRect(hwnd, ctypes.byref(r))
    o = wintypes.POINT(0, 0)
    u.ClientToScreen(hwnd, ctypes.byref(o))
    w, h = r.right - r.left, r.bottom - r.top
    if w <= 0 or h <= 0:
        raise PlacementError("the window has no client area")

    class BIH(ctypes.Structure):
        _fields_ = [("biSize", wintypes.DWORD), ("biWidth", ctypes.c_long), ("biHeight", ctypes.c_long),
                    ("biPlanes", wintypes.WORD), ("biBitCount", wintypes.WORD), ("biCompression", wintypes.DWORD),
                    ("biSizeImage", wintypes.DWORD), ("biXPelsPerMeter", ctypes.c_long),
                    ("biYPelsPerMeter", ctypes.c_long), ("biClrUsed", wintypes.DWORD),
                    ("biClrImportant", wintypes.DWORD)]

    screen = u.GetDC(None)
    mem = g.CreateCompatibleDC(screen)
    bmp = g.CreateCompatibleBitmap(screen, w, h)
    old = g.SelectObject(mem, bmp)

    def read():
        bih = BIH()
        bih.biSize, bih.biWidth, bih.biHeight, bih.biPlanes, bih.biBitCount = ctypes.sizeof(BIH), w, -h, 1, 32
        buf = ctypes.create_string_buffer(w * h * 4)
        if g.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bih), 0) != h:
            raise PlacementError("GetDIBits failed")
        return bgra_to_rgb(buf.raw)

    try:
        how = "print"
        rgb = read() if u.PrintWindow(hwnd, mem, 3) else None   # PW_CLIENTONLY | PW_RENDERFULLCONTENT
        if rgb is None or is_dark(rgb, w, h):
            rect = (o.x, o.y, o.x + w, o.y + h)
            why = screen_copy_problem(rect, [m["bounds"] for m in monitors()],
                                      lambda x, y: u.GetAncestor(u.WindowFromPoint(wintypes.POINT(x, y)), 2) == hwnd)
            if why:   # (GA_ROOT = 2) a screen copy would be other pixels: no picture rather than a wrong one
                raise PlacementError("window copy not usable: PrintWindow gave black and %s" % why)
            if not g.BitBlt(mem, 0, 0, w, h, screen, o.x, o.y, 0x00CC0020):   # SRCCOPY
                raise PlacementError("screen copy failed")
            rgb, how = read(), "screen"
    finally:
        g.SelectObject(mem, old)
        g.DeleteObject(bmp)
        g.DeleteDC(mem)
        u.ReleaseDC(None, screen)
    part = path + ".part"
    with open(part, "wb") as f:
        f.write(png_bytes(w, h, rgb))
    os.replace(part, path)
    return how


def check_display(arg=None):
    """The chosen monitor (dict), or a PlacementError; call it before starting games so a wrong name fails first."""
    return find_monitor(monitors(), display_name(arg))


def place_pids(pids, display=None, topmost=False, wait=0.0, step=0.25, slots=None, count=None):
    """Places the game windows of pids side by side on the chosen monitor: pid i goes in slot slots[i] (default i) of
    count slots (default: as many as the biggest slot needs, at least len(pids)), so a game placed alone keeps the
    place it will have beside the other. Waits up to wait seconds for a pid that has no window yet.
    Returns [(pid, rect)] for those placed; a pid whose window never came is left out."""
    mon = check_display(display)
    slots = list(range(len(pids))) if slots is None else list(slots)
    rects = layout(mon["work"], max([count or 0, len(pids)] + [s + 1 for s in slots]))
    placed = []
    deadline = time.time() + wait
    for pid, slot in zip(pids, slots):
        h = game_window(pid)
        while not h and time.time() < deadline:
            time.sleep(step)
            h = game_window(pid)
        if h:
            place_window(h, rects[slot], topmost)
            placed.append((pid, rects[slot]))
    return placed


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    r = sub.add_parser("rects")
    r.add_argument("--display")
    r.add_argument("--count", type=int, default=2)
    pl = sub.add_parser("place")
    pl.add_argument("--pid", type=int, action="append", required=True)
    pl.add_argument("--display")
    pl.add_argument("--topmost", action="store_true")
    pl.add_argument("--wait", type=float, default=0.0)
    pl.add_argument("--first-slot", type=int, default=0, help="the first pid's place (0 = left); the next pids follow")
    pl.add_argument("--slots", type=int, help="how many places the monitor is shared in (default: as many as needed)")
    la = sub.add_parser("launch")
    la.add_argument("--cwd", required=True)
    la.add_argument("cmd_line", nargs=argparse.REMAINDER, help="-- EXE [ARGS...]")
    a = p.parse_args(argv)
    try:
        if a.cmd == "list":
            print(json.dumps(monitors(), indent=1))
        elif a.cmd == "rects":
            mon = check_display(a.display)
            print(json.dumps({"display": mon["name"], "rects": layout(mon["work"], a.count)}))
        elif a.cmd == "launch":
            cmd = [c for i, c in enumerate(a.cmd_line) if not (i == 0 and c == "--")]
            if not cmd:
                raise PlacementError("launch: no program given")
            print(subprocess.Popen(cmd, cwd=a.cwd, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL, **popen_flags()).pid)
        else:
            slots = [a.first_slot + i for i in range(len(a.pid))]
            print(json.dumps({"placed": place_pids(a.pid, a.display, a.topmost, a.wait, slots=slots, count=a.slots)}))
    except PlacementError as e:
        print("placement: %s" % e, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
