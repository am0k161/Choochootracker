#include "corelib_power.h"

PowerInfo mockPowerInfo = {PowerState::unknown, -1, -1};
uint64_t mockPowerNowSeconds;

PowerInfo powerGetInfo(void) { return mockPowerInfo; }
uint64_t powerNowSeconds(void) { return mockPowerNowSeconds; }
