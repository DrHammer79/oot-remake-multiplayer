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
