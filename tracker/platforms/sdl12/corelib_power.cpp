#include "corelib_power.h"
#include <SDL/SDL.h>

PowerInfo powerGetInfo(void) { return {PowerState::unknown, -1, -1}; }
uint64_t powerNowSeconds(void) { return SDL_GetTicks() / 1000u; }
