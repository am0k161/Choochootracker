#include "corelib_power.h"
#include <SDL2/SDL.h>
#include <stdio.h>

static int batteryPlusPercent(void) {
#if defined(__linux__) && !defined(ANDROID_BUILD)
  FILE* file = fopen("/tmp/battery.percent", "r");
  if (!file) return -1;
  int percent = -1;
  const int read = fscanf(file, "%d", &percent);
  fclose(file);
  if (read == 1 && percent >= 0 && percent <= 100) return percent;
#endif
  return -1;
}

PowerInfo powerGetInfo(void) {
  int seconds = -1, percent = -1;
  const SDL_PowerState state = SDL_GetPowerInfo(&seconds, &percent);
  const PowerState mapped = state == SDL_POWERSTATE_NO_BATTERY ? PowerState::noBattery :
    state == SDL_POWERSTATE_ON_BATTERY ? PowerState::onBattery :
    state == SDL_POWERSTATE_CHARGING ? PowerState::charging :
    state == SDL_POWERSTATE_CHARGED ? PowerState::charged : PowerState::unknown;
  const int calibratedPercent = batteryPlusPercent();
  if (calibratedPercent >= 0) {
    percent = calibratedPercent;
    seconds = -1; // BatteryPlus and the PMIC do not share the same discharge curve.
  }
  return {mapped, percent, seconds};
}

uint64_t powerNowSeconds(void) { return SDL_GetTicks() / 1000u; }
