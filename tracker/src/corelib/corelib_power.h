#ifndef __CORELIB_POWER_H__
#define __CORELIB_POWER_H__

#include <stdint.h>

enum class PowerState : uint8_t {
  unknown,
  noBattery,
  onBattery,
  charging,
  charged,
};

struct PowerInfo {
  PowerState state;
  int percent;
  int seconds;
};

PowerInfo powerGetInfo(void);
uint64_t powerNowSeconds(void);

#endif
