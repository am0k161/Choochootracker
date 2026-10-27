#ifndef __SCREEN_FUR_CHANNELS_H__
#define __SCREEN_FUR_CHANNELS_H__

#include "screens.h"
#include "import/import_fur.h"

// Picker shown before a Furnace import when more than 8 channels play notes.
// Call furChannelsPick, then screenSetup(&screenFurChannels, 0). picked
// receives the chosen channel numbers (at most 8, ascending); cancelled is
// called when the user backs out.
void furChannelsPick(const FurChannelInfo* channels, int count,
                     void (*picked)(const int* channels, int count), void (*cancelled)(void));

extern const AppScreen screenFurChannels;

#endif
