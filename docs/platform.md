# The platform interface (v1 'everywhere', step 1)

For v1 the co-op core (protocol, net, host sync, the game logic) runs on more than one platform: the Longshot/Hookshot
module today, an ExeFS code mod on emulators and consoles later. The core reaches everything outside itself through
one plain C table, `CoopPlatform`, that the platform's adapter fills.

- `coop/platform/include/coop_platform.h`: the table. Versioned like `HosCoopApi` (`hookshot/src/hos_coop_api.h`):
  `version` and `size` first, the table only grows at its end, and the core reads a later slot only when
  `COOP_PLATFORM_HAS(p, slot)` says the adapter's table reaches it. Game slots (memory, calls, hooks, input, UI, voice)
  are optional; the OS slots (sockets, threads, locks, time, memory, files, log) are required.
  `CoopCall` has `HosCoopCall`'s layout.
- Built to be lifted: only plain structs and C function pointers cross the boundary (no virtuals, `std::function` or
  C++ types). `COOP_PLATFORM_SLOTS` lists every slot, so a build step can write a recompiler's entries list from it;
  the functions the core hands to the adapter (`thread_start`'s) need entries too.
- `coop/platform/include/coop/os.hpp` (`coop::os`): the core's threads, mutexes, time, aligned memory, files (relative to
  the mod's folder, written whole through a temporary file, no `..` or absolute paths), non-blocking UDP sockets and
  log, all through the installed table. `coop::os::StdPlatform()` is the PC backend (std::thread, std::mutex,
  steady_clock, std::filesystem, Winsock/BSD sockets); `coop::os::Install()` takes an adapter's table and refuses one of
  another version, too short, or missing a required slot. No exceptions cross it.
- Library `coop_platform` (`coop/CMakeLists.txt`); tests in `tests/platform/` (the C and C++ layouts agree, the
  install checks, threads, files, UDP over loopback).
- The Longshot/Hookshot module is the first adapter (step 5, `hookshot/src/coop_hs_platform.{h,cpp}`): at
  `hos_coop_start` it fills `CoopPlatform` from Hookshot's `HosCoopApi` and installs it for `coop::os`. Game slots
  forward to Hookshot's table (`read`, `write`, `cas32`, `call`, `scratch`, `pad_buttons` for player 0 only, `key_down`,
  `log`); a slot an older Hookshot lacks stays NULL. The OS slots are the std backend's, files under the game's folder
  (`HosCoopApi::game_dir`, which the table itself does not carry). Not filled: `hook_wrap`/`hook_site` (the hooks are
  Hookshot's patches), `pad_sticks` (sticks come through `hos_coop_pad`), `draw_2d` (step 6) and voice (the module's own
  exports). The compat runtime (`hookshot/compat/coop_runtime.h`, the emulator API slice the hook files use) and the
  module's game calls now go through this table; tests in `tests/hookshot/hs_platform_test.cpp`.
- Still to come: step 4 (os-wrap) moves the core's `std::thread`/`std::mutex`/`chrono`/`fstream` onto `coop::os`. The
  `coop_core_*` entry points are declared for review of their shape and defined by core-lib-split.
- Changes from the design sketch: `thread_start` returns a handle and `thread_join` takes it; `thread_id`,
  `mutex_free`, `sleep_us` and `sock_local` were added; the memory slots are `mem_alloc`/`mem_free` (a CRT may define
  `free` as a macro); sockets are opened by kind (`COOP_SOCK_UDP4`/`6`) and `sock_recvfrom` answers
  `COOP_SOCK_WOULD_BLOCK` when nothing waits.

## coop_core (step 3)

`coop_core` (`cmake/coop_core.cmake`, its file lists in `cmake/coop_core_sources.cmake`) is the static library the
adapters link: the coop/* libraries (protocol, net, host, presence, overlay, voice, platform) plus the hook files and UI
models that are the same code in every build. The Longshot module and `coop_tests` link it; before, each compiled those
files itself. The emulator compiles the same hook-file list inside its own `core` (`lichii/hook/core.cmake`), as before:
a library outside Fritchii's `src/` would lose Fritchii's flags for core (`-fwrapv`, `-fno-rtti`; MSVC `/GT`, `/GR-`,
`/volatile:iso`, ...), and `-fwrapv` changes what signed overflow does.

- `tools/check_core_flags.py` proves the flags did not change (CI, from each build's compile database): in the emulator
  every core file compiles with the same flags as `coop_dungeon.cpp` (always in `core`); in the module (MSVC and
  Linux) every `coop_core` file compiles with the flags of `hookshot/src/coop_title.cpp` except the module-only
  `LICHII_COOP`/`COOP_HOOKSHOT` defines and the compat/, server/, fmt and Monocypher include folders, which
  `tools/check_core.py` proves no core file uses. Order is compared within each option family (`-fwrapv`/`-fno-wrapv`,
  `/O1`/`/O2`), where a later flag wins; across families CMake may order a de-duplicated flag (MSVC `/utf-8`) differently.

- A file is in it only when neither it nor anything it includes uses an OS header (`windows.h`, POSIX system headers),
  an emulator one (`core/`, `common/`, Qt) or a Hookshot/Longshot one (`hos_coop_*`, Vulkan, SDL, fmt), and it has no
  `#if` on `LICHII_COOP`/`COOP_HOOKSHOT` (the library is built without them). `tools/check_core.py` checks that, and
  that no file is in both `coop_core` and `lichii/hook/core.cmake` (CI: python-tools). A new platform-free hook file
  goes in `COOP_CORE_HOOK_SOURCES`; one with emulator glue goes in `lichii/hook/core.cmake` as before.
- Still outside it: the hook files whose emulator glue sits behind `#ifdef LICHII_COOP` in the same file (chests,
  drops, dungeon, fairy, quests, story, look_share, ocarina_sound, link_sound, voice, puppet_config, ride, spawn and
  the `*_game.cpp`/memory files), and the menu models that call `coop_i18n`'s `Tr()` (i18n finds the game folder with
  `windows.h`). The core's files call into the first group, so the core is not yet link-complete on its own (the module
  and the tests compile that group themselves). Moving that glue into `*_game.cpp` files and i18n's folder lookup onto
  `coop::os` (step 4) lets them join.
- Lifting (ExeFS payload): the core still takes callbacks as C++ types where the adapter will sit:
  `coop::host::GameMemory` and `CoopPuppet::Guest` (virtual), `CoopCallers::Caller` and `CoopLocalGame`'s
  pads/take/read/write/log hooks, `CoopLocalNet`'s Send/Log, `CoopWorld`'s ReadWord (`std::function`). Inside the
  core they are fine; at the payload boundary they become `CoopPlatform` slots (step 5).

## The payload build (step 8, COOP_PLATFORM_STATIC)
A payload that a recompiler lifts (the ExeFS module, subsdk1) may make no call through a function pointer, and
`CoopPlatform` is a table of them. So the payload builds `coop/platform/src/os.cpp` with `COOP_PLATFORM_STATIC`: every
coop::os call of a required slot is then a direct call of the adapter's `coop_plat_<slot>` function
(`coop_platform_static.h`, each checked at compile time to have its slot's type). The adapter fills its table's
required slots with the same functions; the table still gives the optional game slots, `kind` and `main_base`. Before
any `Install`, `Platform()` is a table of those functions with no game slots; the std backend (`os_std.cpp`) is not in
a payload. PC and the Longshot/Hookshot module do not define the flag and keep the table. Test:
`coop_platform_static_tests` (tests/platform/static_os_test.cpp) builds os.cpp with the flag over fake adapter functions.
