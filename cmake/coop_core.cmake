# coop_core (v1 step 3, docs/platform.md): the co-op's platform-free part as one static library, which
# the Hookshot/Longshot module, the emulator's core and the unit tests link. It is the coop/* libraries
# plus the hook files and UI models that use no OS, emulator or Hookshot code.
#
# A file goes here only when it is the same code in every build: no windows.h, no emulator (core/,
# common/, Qt) or Hookshot (hos_coop_*) header, and no #if on LICHII_COOP or COOP_HOOKSHOT (the library
# is built once, without them). tools/check_core.py checks that for the lists below and everything they
# include (CI: python-tools). A hook file listed here is NOT also listed in lichii/hook/core.cmake (the
# emulator's and the module's per-build list).
#
# Not here yet: the hook files with LICHII_COOP glue (chests, drops, dungeon, fairy, quests, story,
# look_share, ocarina_sound, link_sound, voice, puppet_config, ride, spawn, ...), and the menu models
# that call coop_i18n's Tr() (i18n finds the game folder with windows.h): see reports/v1-core-a.md.
get_filename_component(_coop_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

# Hook files (lichii/hook): game logic over GameMemory and the callers, no platform code.
set(COOP_CORE_HOOK_SOURCES
  coop_boss.cpp
  coop_boss_native.cpp
  coop_boss_scenes.cpp
  coop_callers.cpp
  coop_camera.cpp
  coop_cheat_items.cpp
  coop_colors.cpp
  coop_counters.cpp
  coop_dbno.cpp
  coop_difficulty.cpp
  coop_enemy_shots.cpp
  coop_lift_ride.cpp
  coop_live_events.cpp
  coop_live_places.cpp
  coop_lobby.cpp
  coop_local_game.cpp
  coop_local_net.cpp
  coop_logic_probe.cpp
  coop_mod_share.cpp
  coop_name_privacy.cpp
  coop_npc_anim.cpp
  coop_objects_sync.cpp
  coop_ocarina.cpp
  coop_pickups.cpp
  coop_platforms.cpp
  coop_pose.cpp
  coop_projectiles.cpp
  coop_puppet.cpp
  coop_puppet_sound.cpp
  coop_pvp.cpp
  coop_quest.cpp
  coop_rooms.cpp
  coop_scrubs.cpp
  coop_settings.cpp
  coop_shared_scenes.cpp
  coop_teleport.cpp
  coop_time_sync.cpp
  coop_world.cpp
)

# UI models (hookshot/src): rows, text and colours, no drawing and no Hookshot ABI.
set(COOP_CORE_UI_SOURCES
  coop_chatbox.cpp
  coop_look.cpp
  coop_look_tunic.cpp
  coop_mod_json.cpp
)

set(_coop_core_files)
foreach(name IN LISTS COOP_CORE_HOOK_SOURCES)
  list(APPEND _coop_core_files "${_coop_root}/lichii/hook/${name}")
endforeach()
foreach(name IN LISTS COOP_CORE_UI_SOURCES)
  list(APPEND _coop_core_files "${_coop_root}/hookshot/src/${name}")
endforeach()

add_library(coop_core STATIC ${_coop_core_files})
# The headers live beside the sources. The hook folder is public (the emulator, the module and the tests
# include the hook's headers); the UI models' folder only the module and the tests add, as before.
target_include_directories(coop_core PUBLIC "${_coop_root}/lichii/hook" PRIVATE "${_coop_root}/hookshot/src")
target_link_libraries(coop_core PUBLIC coop coop_net coop_host coop_presence coop_overlay coop_voice
                      coop_protocol coop_platform PRIVATE coop_warnings)
# As the module built these files before: hidden symbols (nothing in the core is exported from the
# module) and, with MSVC, the emulator's warning exceptions and big object files.
set_target_properties(coop_core PROPERTIES CXX_VISIBILITY_PRESET hidden)
if(MSVC)
  target_compile_options(coop_core PRIVATE /wd4456 /wd4457 /wd4458 /wd4459 /bigobj)
endif()
