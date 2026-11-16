#!/usr/bin/env python3
"""Verdict for one emulator run of the ExeFS module (run_fritchii.ps1 calls it).

    run_check.py --log eden_log.txt [--preset NAME] [--expect NAME=REGEX]... [--forbid NAME=REGEX]...
                 [--listen listen.txt --datagrams N] [--alive 0|1]

Prints one `PASS <check>` / `FAIL <check>` line per check, then `RESULT PASS` or `RESULT FAIL`; exit 0 / 1.
Only the module's own lines (`coop-proto: ...`, and the adapter's `coop: net: ...` from coop_plat_log) are matched, so
text elsewhere in the emulator log cannot pass a check.
--alive 1 says the game was still running at the end (no crash); 0 fails the run.
"""
from __future__ import annotations

import argparse
import re
import sys

MODULE_TAG = "coop-proto: "
# The adapter's log (coop_plat_log, switch/adapter/platform_os.cpp) tags its lines `coop: `, as the emulator's own co-op
# hook does (`coop: boss: ...`): only these topics of it are the module's.
ADAPTER_TAG = "coop: "
ADAPTER_TOPICS = ("net: ", "enet: ")
HELLO = ("1 loaded (hello)", r"hello from subsdk1")
HOOK_IN = ("2 frame hook in", r"hook \d+: frame hook in at main\+0x6e70f8")
NULL_GUARDS = 29  # switch/proto/null_guards.inc kNullGuardCount

# name -> (expects, forbids, datagrams)
PRESETS: dict[str, tuple[list[tuple[str, str]], list[tuple[str, str]], int]] = {
    # The prototype (reports v1-proto-a): hello, frame hook, one game call, one UDP hello.
    "proto": ([
        HELLO, HOOK_IN,
        ("2 frame hook runs", r"^frame 600$"),
        ("3 game call returned", r"game call: set_time returned"),
        ("4 net sent", r"net: connect 0, sent 29 bytes"),
    ], [], 3),
    # Null guards Run 2 (v1 step 8 (g)): every guard in, none left out, the game plays on.
    "nullguards": ([
        HELLO, HOOK_IN,
        # Pinned, so "0 of 0" cannot pass; run_check_test.py keeps it equal to null_guards.inc's kNullGuardCount.
        ("2 null guards all applied", rf"null guards: {NULL_GUARDS} of {NULL_GUARDS} applied"),
        ("3 game plays (frame 6000)", r"^frame 6000$"),
    ], [
        ("2 no guard left out", r"null guard at main\+0x[0-9a-f]+: .*left out"),
        ("2 island page usable", r"null guards: the island page could not"),
        ("2 code pages back to RX", r"restore RX failed|RX back refused"),
    ], 0),
    # The code-memory path (v1 step 8 (a), switch/proto/codemem.h): every patch written through a RW alias,
    # main's own pages stay RX, no 0x73 fallback.
    "codemem": ([
        HELLO, HOOK_IN,
        # Two aliases: main's text (not one page) and the island page (one page).
        ("2 alias of main's text", r"^codemem: alias at 0x[0-9a-f]+ for 0x[0-9a-f]+ \+0x(?!1000 )[0-9a-f]+ \(RW"),
        ("2 alias of the island page", r"^codemem: alias at 0x[0-9a-f]+ for 0x[0-9a-f]+ \+0x1000 \(RW"),
        ("3 frame site page RX after patching", r"^codemem: frame site page perm 0x5 \(RX\)"),
        ("4 null guards all applied", rf"^null guards: {NULL_GUARDS} of {NULL_GUARDS} applied"),
        ("5 game plays (frame 3000)", r"^frame 3000$"),
    ], [
        ("2 no refused step", r"^codemem: .* refused, result"),
        ("2 main's text found", r"^codemem: main's text not found"),
        ("3 no write through svc 0x73", r"writes through svc 0x73"),
        ("3 no 0xd401", r"0xd401"),
        ("4 no guard left out", r"null guard at main\+0x[0-9a-f]+: .*left out"),
        ("4 island page written", r"null guards: the island page could not"),
        ("4 every hook written", r"^hook: main\+0x[0-9a-f]+ could not be written"),
    ], 0),
    # The network slots + ENet in the payload (v1 step 8 (c), build.py --net, switch/adapter/net_selftest.cpp): a UDP
    # echo through the slots (listen.py --echo on 27520), then an ENet connect + disconnect to coop_server on 27521.
    # The prototype's own 3 hello datagrams still reach the listener (frame 900).
    "net": ([
        HELLO, HOOK_IN,
        ("3 udp bound through the slots", r"^net: udp bound to port [1-9]\d* through the platform slots$"),
        ("4 udp sent to the echo", r"^net: udp sent 26 bytes to 127\.0\.0\.1:27520$"),
        ("5 udp echo read back, same text", r"^net: udp echo 26 bytes back from 127\.0\.0\.1:27520, same text$"),
        ("6 enet connected to coop_server", r"^net: enet connected to the server \(mtu \d+\)$"),
        ("7 enet disconnected", r"^net: enet disconnected$"),
        ("8 self-test finished", r"^net: self-test done$"),
    ], [
        ("3-8 no net failure", r"^net: FAIL"),
        ("9 ENet never out of memory", r"^enet: out of memory"),
    ], 3),
}


def module_lines(log_text: str) -> list[str]:
    out = []
    for line in log_text.splitlines():
        i = line.find(MODULE_TAG)
        if i >= 0:
            out.append(line[i + len(MODULE_TAG):].rstrip())
            continue
        i = line.find(ADAPTER_TAG)
        if i >= 0 and line.startswith(ADAPTER_TOPICS, i + len(ADAPTER_TAG)):
            out.append(line[i + len(ADAPTER_TAG):].rstrip())
    return out


def pair(text: str) -> tuple[str, str]:
    name, sep, regex = text.partition("=")
    if not sep or not name or not regex:
        raise argparse.ArgumentTypeError(f"want NAME=REGEX, got {text!r}")
    re.compile(regex)
    return name, regex


def verdict(lines: list[str], expects, forbids, datagrams: int | None = None, listen_text: str = "",
            alive: bool | None = None) -> tuple[bool, list[str]]:
    report = []
    ok_all = True

    def say(ok: bool, name: str) -> None:
        nonlocal ok_all
        ok_all = ok_all and ok
        report.append(f"{'PASS' if ok else 'FAIL'} {name}")

    for name, regex in expects:
        say(any(re.search(regex, line) for line in lines), name)
    for name, regex in forbids:
        hit = next((line for line in lines if re.search(regex, line)), None)
        say(hit is None, name if hit is None else f"{name}: {hit}")
    if datagrams:
        got = len(re.findall(r"coop-proto hello from subsdk1", listen_text))
        say(got == datagrams, f"listener datagrams ({got} of {datagrams})")
    if alive is not None:
        say(alive, "game still running at the end (no crash)")
    if not expects and not forbids and not datagrams and alive is None:
        say(False, "no checks given")
    report.append("RESULT PASS" if ok_all else "RESULT FAIL")
    return ok_all, report


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument("--log", required=True)
    ap.add_argument("--preset", choices=sorted(PRESETS))
    ap.add_argument("--expect", type=pair, action="append", default=[])
    ap.add_argument("--forbid", type=pair, action="append", default=[])
    ap.add_argument("--listen")
    ap.add_argument("--datagrams", type=int)
    ap.add_argument("--alive", type=int, choices=(0, 1))
    a = ap.parse_args(argv)
    expects, forbids, datagrams = ([], [], 0)
    if a.preset:
        expects, forbids, datagrams = PRESETS[a.preset]
        expects, forbids = list(expects), list(forbids)
    expects += a.expect
    forbids += a.forbid
    if a.datagrams is not None:
        datagrams = a.datagrams
    try:
        log_text = open(a.log, encoding="utf-8", errors="replace").read()
    except OSError as e:
        log_text = ""
        print(f"run_check: no log ({e})")
    listen_text = ""
    if datagrams:
        try:
            listen_text = open(a.listen or "", encoding="utf-8", errors="replace").read()
        except OSError:
            pass
    alive = None if a.alive is None else bool(a.alive)
    lines = module_lines(log_text)
    for line in lines[:40]:
        print(f"  {line}")
    ok, report = verdict(lines, expects, forbids, datagrams, listen_text, alive)
    print("\n".join(report))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
