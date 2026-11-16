# coop_core (v1 step 3, docs/platform.md): the co-op's platform-free part as one static library, which
# the Hookshot/Longshot module and the unit tests link. It is the coop/* libraries plus the hook files and
# UI models that use no OS, emulator or Hookshot code (the lists: cmake/coop_core_sources.cmake).
#
# A file goes there only when it is the same code in every build: no windows.h, no emulator (core/,
# common/, Qt) or Hookshot (hos_coop_*) header, and no #if on LICHII_COOP or COOP_HOOKSHOT (the library
# is built once, without them). tools/check_core.py checks that for the lists and everything they
# include (CI: python-tools). A hook file listed there is NOT also listed in lichii/hook/core.cmake.
#
# The emulator compiles the same list inside its own core (lichii/hook/core.cmake), not this library:
# Fritchii's directory flags (-fwrapv, -fno-rtti; MSVC /GT, /GR-, /volatile:iso, ...) stay on the files
# that way. tools/check_core_flags.py compares each core file's flags with its build's own files (CI).
#
# Not here yet: the hook files with LICHII_COOP glue (chests, drops, dungeon, fairy, quests, story,
# look_share, ocarina_sound, link_sound, voice, puppet_config, ride, spawn, ...), and the menu models
# that call coop_i18n's Tr() (i18n finds the game folder with windows.h): see docs/platform.md.
get_filename_component(_coop_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

include("${CMAKE_CURRENT_LIST_DIR}/coop_core_sources.cmake")  # COOP_CORE_HOOK_SOURCES, COOP_CORE_UI_SOURCES

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
                      coop_protocol coop_platform coop_ui PRIVATE coop_warnings)
# As the module built these files before: hidden symbols (nothing in the core is exported from the
# module) and, with MSVC, the emulator's warning exceptions and big object files.
set_target_properties(coop_core PROPERTIES CXX_VISIBILITY_PRESET hidden)
if(MSVC)
  target_compile_options(coop_core PRIVATE /wd4456 /wd4457 /wd4458 /wd4459 /bigobj)
endif()
