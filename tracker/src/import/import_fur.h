#ifndef __IMPORT_FUR_H__
#define __IMPORT_FUR_H__

#include "chipnomad_lib.h"

#ifdef __cplusplus
extern "C" {
#endif

// Imports a Furnace module (.fur) as a new project: the first subsong, at most
// 8 channels. Tested with the demos of Furnace 0.5 to 0.6.8.3 (format 232); the
// newer INF2/SNG2 layout (format 240 and up) is read but untested. Without a choice, the
// channels that have notes are kept (those playing samples first).
//
// Converted: notes, note off, volumes (rescaled to 00-7F), the effects 00xy
// (ARP), 04xy (PVB), 08xy (PAN), 0Fxx (GGR, speeds up to 1F), ECxx (OFF) and
// EDxx (DEL), the song order (one chain per order and channel, cut into
// 16-row phrases) and the speed pattern (groove 0). Instruments of the YM2612,
// YM2151, OPL2/OPL3 (2 and 4 operators), SID and the Furnace samples become
// the matching tracker instruments; other chips keep their notes with a
// default instrument (AY-3-8910 channels get an AY instrument, the SN76489 a
// Sega PSG one). Everything else (other effects, macros, wavetables, other
// chips' patches) is dropped.
//
// Furnace samples are written as mono 16-bit WAV files to
// <sampleDir>/<file name>/NNN.wav and loaded into Sample instruments. When
// sampleDir is NULL or empty, samples are skipped with a warning.
//
// The destination must be initialized with projectInit/projectInitAY first.
// Success releases its previous instrument data; failure leaves the
// destination unchanged and owned by the caller.
int projectLoadFur(Project* project, const char* path, const char* sampleDir);

// Same with an explicit channel list (zero based Furnace channel numbers, at
// most 8) and subsong. channelCount 0 means automatic.
int projectLoadFurChannels(Project* project, const char* path, const char* sampleDir,
                           const int* channels, int channelCount, int subsong);

// One channel of a module that plays notes in the chosen subsong.
struct FurChannelInfo {
  int channel;       // zero based Furnace channel number
  char name[32];     // chip name and channel on it, e.g. "YM2612 3"
  int notes;         // number of notes in the subsong
  int picked;        // 1 when the automatic choice keeps it
};

// Lists the channels that play notes in a subsong, for a picker shown before
// the import when more than 8 of them do. Fills at most maxChannels entries and
// stores their number in *count. Returns 0 on success, otherwise sets
// projectFileError.
int furListChannels(const char* path, int subsong, FurChannelInfo* channels, int maxChannels, int* count);

// What was lost or approximated by the last import, one line per item
// separated by newlines ("" when nothing). Valid until the next import.
const char* projectLoadFurWarnings(void);

#ifdef __cplusplus
}
#endif

#endif
