# ENet (MIT licence), fetched at a fixed commit and never copied into the tree.
# v1.3.18 = commit 2662c0de09e36f2a2030ccc2c528a3e4c9e8138a
include(FetchContent)

# ENet's own CMakeLists asks for a CMake older than 3.5, which CMake 4 refuses.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

FetchContent_Declare(enet
  GIT_REPOSITORY https://github.com/lsalzman/enet.git
  GIT_TAG        2662c0de09e36f2a2030ccc2c528a3e4c9e8138a # v1.3.18
)
FetchContent_MakeAvailable(enet)

unset(CMAKE_POLICY_VERSION_MINIMUM)

# ENet's CMakeLists sets its include path per directory, not on the target,
# and does not link the Windows socket libraries. Add both here.
target_include_directories(enet SYSTEM PUBLIC "${enet_SOURCE_DIR}/include")
if(WIN32)
  target_link_libraries(enet PUBLIC ws2_32 winmm)
endif()

# ENet's own warnings are not ours to fix; keep the build log for our code.
if(MSVC)
  target_compile_options(enet PRIVATE /w)
else()
  target_compile_options(enet PRIVATE -w)
endif()

# ENet finds header offsets by member access through a null pointer
# (protocol.c, "& ((ENetProtocolHeader *) 0) -> sentTime"), an old offsetof
# idiom that UBSan reports. Its fragment bitmask also does `1 << 31` on an int
# (protocol.c, "1 << (fragmentNumber % 32)") once a packet has 32 or more
# fragments, as every full 4096-entry flag message (45 KB) has. Turn off only
# those two checks, only for ENet; ASan and the rest of UBSan still cover it.
if(COOP_SANITIZE)
  target_compile_options(enet PRIVATE -fno-sanitize=null,shift-base)
endif()

# enet_coop (v1 step 8c): ENet's protocol files with coop/net/enet_coop in place of unix.c / win32.c, so ENet
# runs on CoopPlatform's sockets (the ExeFS payload). It defines the same enet_* symbols as enet, so nothing
# links both: the PC client keeps enet; enet_coop has its own test program.
add_library(enet_coop STATIC
  "${enet_SOURCE_DIR}/callbacks.c" "${enet_SOURCE_DIR}/compress.c" "${enet_SOURCE_DIR}/host.c"
  "${enet_SOURCE_DIR}/list.c" "${enet_SOURCE_DIR}/packet.c" "${enet_SOURCE_DIR}/peer.c"
  "${enet_SOURCE_DIR}/protocol.c" "${PROJECT_SOURCE_DIR}/coop/net/enet_coop/enet_coop.cpp")
target_include_directories(enet_coop SYSTEM PUBLIC "${enet_SOURCE_DIR}/include")
target_include_directories(enet_coop PUBLIC "${PROJECT_SOURCE_DIR}/coop/net/enet_coop")
target_link_libraries(enet_coop PUBLIC coop_platform PRIVATE coop_warnings)
if(WIN32)
  target_link_libraries(enet_coop PUBLIC ws2_32)  # ENET_HOST_TO_NET_* are htons/htonl there
endif()
if(MSVC)
  target_compile_options(enet_coop PRIVATE $<$<COMPILE_LANGUAGE:C>:/w>)
else()
  target_compile_options(enet_coop PRIVATE $<$<COMPILE_LANGUAGE:C>:-w>)
endif()
if(COOP_SANITIZE)
  target_compile_options(enet_coop PRIVATE $<$<COMPILE_LANGUAGE:C>:-fno-sanitize=null,shift-base>)
endif()
