/* SPDX-License-Identifier: GPL-3.0-or-later
   coop_platform.h must stay plain C (an adapter may be C): this file only compiles it as C and reports the layout
   the C compiler sees, which platform_test.cpp compares with the C++ one. */
#include <coop_platform.h>

size_t coop_platform_c_size(void) { return sizeof(CoopPlatform); }
size_t coop_platform_c_offset_log(void) { return offsetof(CoopPlatform, log); }
size_t coop_platform_c_addr_size(void) { return sizeof(CoopAddr); }
size_t coop_platform_c_call_size(void) { return sizeof(CoopCall); }
