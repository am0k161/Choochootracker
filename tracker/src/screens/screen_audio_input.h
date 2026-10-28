#ifndef __SCREEN_AUDIO_INPUT_H__
#define __SCREEN_AUDIO_INPUT_H__

#include "screens.h"

constexpr int AUDIO_INPUT_FROM_INSTRUMENT = 1;
extern const AppScreen screenAudioInput;
int audioInputSetEnabled(int enabled);

#endif
