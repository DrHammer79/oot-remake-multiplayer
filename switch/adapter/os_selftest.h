// The OS slots' self-test (os_selftest.cpp), called from the module's frame hook.
#pragma once

#include <stdint.h>

extern "C" {
void coop_os_selftest_frame(uint64_t frame);  // every frame, on the game thread
void coop_os_selftest_thread(void* arg);      // the test's own thread (started by the above)
}
