#!/usr/bin/env python3
"""Build the ExeFS prototype (switch/proto) into an NSO code module with LLVM.

    build.py [--llvm <LLVM bin folder>] [--out <folder>]

Needs clang and ld.lld with the AArch64 target (any stock LLVM build). Writes
<out>/module.elf, <out>/subsdk1 and <out>/subsdk1.entries (default out:
build/switch-proto), after lift_check.py has passed (the module stays liftable by a
recomp). Install by copying subsdk1 into the emulator's
<user>/load/0400486024DEC000/<mod>/exefs/.
"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
SRC = REPO / "switch" / "proto"
ADAPTER = REPO / "switch" / "adapter"
# v1 step 8: the adapter's sources (switch/adapter) and the headers they share with the core.
ADAPTER_SOURCES = ("platform_os.cpp", "arena.cpp", "os_selftest.cpp", "platform_net.cpp")
INCLUDES = ["-I", str(REPO / "coop" / "platform" / "include")]
# Every name the module may import (one per line, '#' comments): checked against the game's dynsym, so a declaration
# that mangles differently (a wrong parameter type) fails the build instead of failing to link in the game.
IMPORTS = HERE / "imports.txt"
# v1 step 8c, --net: ENet (its protocol files at the pinned commit, built against switch/adapter/libc) on the platform's
# sockets (coop/net/enet_coop) and the network self-test. Off by default, so other runs' modules stay as they were.
ENET_FILES = ("compress.c", "host.c", "list.c", "packet.c", "peer.c", "protocol.c")  # callbacks.c: enet_coop's
LIBC = ADAPTER / "libc"
NET_DEFINES = ["-DCOOP_PLATFORM_STATIC"]
C_FLAGS = [
    "--target=aarch64-none-elf", "-march=armv8-a", "-O2", "-std=c11",
    "-fPIC", "-ffreestanding", "-fno-stack-protector",
    "-funwind-tables", "-fno-jump-tables", "-fvisibility=hidden", "-ffixed-x18", "-mno-outline-atomics",
    "-nostdlib",
]
sys.path.insert(0, str(HERE))
import elf2nso  # noqa: E402
import lift_check  # noqa: E402

CFLAGS = [
    "--target=aarch64-none-elf", "-march=armv8-a", "-O2", "-std=c++20",
    "-fPIC", "-ffreestanding", "-fno-exceptions", "-fno-rtti", "-fno-stack-protector",
    "-funwind-tables", "-fno-jump-tables", "-fvisibility=hidden", "-ffixed-x18", "-mno-outline-atomics",
    "-nostdlib", "-Wall", "-Wextra", "-Werror",
]
LDFLAGS = [
    "-shared", "-z", "now", "--hash-style=sysv", "--build-id=sha1", "-z", "noexecstack",
    "-e", "__module_start", "--eh-frame-hdr", "--apply-dynamic-relocs",
]


def find_tool(name: str, llvm: str | None) -> str:
    exe = name + (".exe" if os.name == "nt" else "")
    candidates = [Path(llvm) / exe] if llvm else []
    if os.name == "nt":
        candidates.append(Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "LLVM" / "bin" / exe)
    for c in candidates:
        if c.is_file():
            return str(c)
    found = shutil.which(name)
    if not found:
        sys.exit(f"build: {name} not found (pass --llvm <LLVM bin folder>)")
    return found


def run(cmd: list[str]) -> None:
    r = subprocess.run(cmd)
    if r.returncode != 0:
        sys.exit(f"build: failed: {' '.join(cmd)}")


def allowed_imports(path: Path = IMPORTS) -> set[str]:
    names = set()
    for line in path.read_text(encoding="ascii").splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            names.add(line.split()[0])
    return names


def import_problems(elf: "lift_check.Elf", allowed: set[str]) -> list[str]:
    """Every undefined dynamic symbol must be in the allow-list."""
    return [f"imports {s.name}, which is not in {IMPORTS.name} (checked against the game's dynsym?)"
            for s in elf.symbols(lift_check.SHT_DYNSYM) if not s.defined and s.name and s.name not in allowed]


def enet_source(out: Path, given: str | None) -> Path:
    """ENet's source at the commit cmake/enet.cmake pins: --enet DIR, else a clone in the build folder."""
    cmake = (REPO / "cmake" / "enet.cmake").read_text(encoding="utf-8")
    url = re.search(r"GIT_REPOSITORY\s+(\S+)", cmake).group(1)
    tag = re.search(r"GIT_TAG\s+([0-9a-f]{40})", cmake).group(1)
    src = Path(given) if given else out / "enet-src"
    if not given and not (src / ".git").exists():
        run(["git", "clone", "--quiet", url, str(src)])
    head = subprocess.run(["git", "-C", str(src), "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
    if head != tag:
        if given:
            sys.exit(f"build: {src} is at {head or '?'}, cmake/enet.cmake pins {tag}")
        run(["git", "-C", str(src), "-c", "advice.detachedHead=false", "checkout", "--quiet", tag])
    return src


def net_sources(enet: Path) -> list[tuple[Path, list[str]]]:
    """(source, flags) for a --net build: ENet without warnings (not ours), our files strict."""
    inc = ["-isystem", str(LIBC), "-isystem", str(enet / "include")]
    ours_cpp = [*CFLAGS, *INCLUDES, *NET_DEFINES, *inc]
    return ([(enet / name, [*C_FLAGS, *inc, "-w"]) for name in ENET_FILES] +
            [(LIBC / "mem.c", [*C_FLAGS, *inc, "-Wall", "-Wextra", "-Werror"]),
             (REPO / "coop" / "net" / "enet_coop" / "enet_coop.cpp", ours_cpp),
             (ADAPTER / "net_selftest.cpp", ours_cpp)])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--llvm")
    ap.add_argument("--out", default=str(REPO / "build" / "switch-proto"))
    ap.add_argument("--net", action="store_true", help="add ENet and the network self-test (v1 step 8c run)")
    ap.add_argument("--enet", help="ENet's source at the pinned commit (default: cloned into --out)")
    a = ap.parse_args()
    clang = find_tool("clang", a.llvm)
    lld = find_tool("ld.lld", a.llvm)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    objs = []
    sources = [SRC / "start.s", SRC / "main.cpp", SRC / "codemem.cpp"] + [ADAPTER / name for name in ADAPTER_SOURCES]
    builds = [(src, [*CFLAGS, *INCLUDES] if src.suffix == ".cpp" else ["--target=aarch64-none-elf", "-march=armv8-a"])
              for src in sources]
    if a.net:
        builds = [(src, [*flags, "-DCOOP_NET_SELFTEST"] if src.name == "main.cpp" else flags) for src, flags in builds]
        builds += net_sources(enet_source(out, a.enet))
    for src, flags in builds:
        obj = out / (src.parent.name + "_" + src.name + ".o")
        run([clang, *flags, "-c", str(src), "-o", str(obj)])
        objs.append(str(obj))
    elf = out / "module.elf"
    run([lld, *LDFLAGS, "-T", str(SRC / "module.ld"), "--version-script", str(SRC / "exports.map"),
         *objs, "-o", str(elf)])
    try:
        parsed = lift_check.Elf(elf.read_bytes())
        reports: list[str] = []
        problems, entries = lift_check.check(parsed, ("enet_",) if a.net else (), reports)
        for line in reports:  # step 8 decision 2: ENet's own callbacks, listed for the lift-clean step (f2)
            print(f"build: lift report: {line}")
        problems += import_problems(parsed, allowed_imports())
    except lift_check.LiftError as e:
        problems, entries = [str(e)], []
    if problems:
        sys.exit("build: the module would not lift or would not link in the game:\n  " + "\n  ".join(problems))
    lift_check.write_entries(str(out / "subsdk1.entries"), entries)
    nso = elf2nso.build_nso(elf.read_bytes())
    (out / "subsdk1").write_bytes(nso)
    print(f"build: {out / 'subsdk1'} ({len(nso)} bytes), {len(entries)} entries")
    return 0


if __name__ == "__main__":
    sys.exit(main())
