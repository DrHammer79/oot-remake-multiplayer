# The files of coop_core (cmake/coop_core.cmake): read by it, by lichii/hook/core.cmake (the emulator compiles the
# same list inside its own core, with its own flags) and by tools/check_core.py. Only set() lists here.

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
