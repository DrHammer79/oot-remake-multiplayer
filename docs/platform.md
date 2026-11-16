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
- Step 4 (os-wrap) moves the core's `std::mutex`/`steady_clock`/`fstream`/`getenv`/`random_device` onto `coop::os`
  (see "os-wrap (step 4)" below). The `coop_core_*` entry points are declared for review of their shape and defined by
  later steps.
- Slots appended after the first table (step 4): `mutex_try_lock` (for `std::unique_lock` with `std::try_to_lock`),
  `env_get` (optional: consoles have no environment) and `random_fill` (keys, names). The version stays 1: the table
  grows at its end, `Install` checks a required slot only when the table's `size` reaches it, and `coop::os` does
  without one an older table lacks (`try_lock` waits; `GetEnv` answers nothing; `RandomFill` fills from the clock,
  thread and a call count and answers false). `coop::os::SteadyClock` is a `std::chrono` clock over `now_us`, so the
  core keeps its `time_point`/duration code; on the std backend it has `std::chrono::steady_clock`'s epoch.
- Condition variables (`cond_new`, `cond_wait_us`, `cond_notify_one`/`_all`, `cond_free`, appended with them) for
  `coop::host::Session`'s network wait: `coop::os::CondVar` waits through a `std::unique_lock<coop::os::Mutex>` with
  `wait_for(lock, duration, pred)` as `std::condition_variable` has it; without the slots it sleeps a millisecond at a
  time with the lock released.
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

## os-wrap (step 4)

The core (`coop_core`'s files and every header they reach) makes no OS call of its own: it goes through `coop::os`, so
an adapter that fills `CoopPlatform` (Longshot today, an ExeFS code mod later) is all it needs. On PC the std backend
does what the core did itself, so Longshot and the emulator behave as before.

| Area | Was | Now |
|---|---|---|
| locks | `std::mutex` (incl. `coop::host::Session`, `coop_local.h`) | `coop::os::Mutex`; same `std::lock_guard`/`std::unique_lock` code |
| waits | `std::condition_variable` (`Session::wait`) | `coop::os::CondVar` |
| time | `std::chrono::steady_clock` | `coop::os::SteadyClock` (same epoch on PC) |
| environment | `getenv` / `_dupenv_s` | `coop::os::GetEnv` |
| random | `std::random_device` | `coop::os::RandomFill` |
| files | `std::filesystem` + `fstream` (coop.ini, player-key.txt) | `coop::os::ReadFile`/`WriteFile`, relative to the co-op's folder |

- Files: `CoopHook::LoadSettings(kSettingsFile, ...)` and `LoadPlayerKey(kPlayerKeyFile, ...)` take names relative to
  the co-op's folder. On Longshot that is the game folder (the std backend's root, set by the module's platform table;
  the same folder as before); the emulator points the std backend's root at its config folder before it reads them.
  `coop::host::SharedFlags::load` became `load_shared_flags` in `coop/host/flags_file.hpp`, which the core does not
  include (the emulator's `LICHII_COOP_FLAGS` file and tests use it).
- `tools/check_core.py` refuses each converted family in the core: `std::mutex` and the other std mutexes,
  `std::condition_variable`, `std::thread`/`this_thread`, the std clocks, `getenv`/`_dupenv_s`/`setenv`,
  `std::random_device`, `std::filesystem`/`fstream`/`fopen`, and the headers `<thread>`, `<condition_variable>`,
  `<shared_mutex>`, `<future>`, `<filesystem>`, `<fstream>`. `<mutex>` stays allowed (`std::lock_guard`), and so do
  `<chrono>` durations and seeded game RNGs (`std::mt19937`).
- Not in this step: the coop/* libraries' own sources outside what the core reaches (`coop_net` is ENet over the OS
  sockets until the ExeFS adapter's socket backend, step 8; `coop/host`'s save transfer and map-drop log files,
  `coop_voice`), and the adapters' own files (`coop_hook.cpp`, `coop_hookshot.cpp`, ...), which are platform code.
