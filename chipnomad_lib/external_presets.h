#pragma once
#include <string>
#include <vector>

#include "project.h"

// Data-only import. Never executes source tracker/player code or changes a song.
struct ExternalPreset {
  Instrument instrument{};
  Table table{};
  std::string name;
  int sourceIndex = 0;
};
bool externalPresetExtension(InstrumentType target, const std::string& path);
// On failure output is unchanged. A successful partial bank reports exclusions
// in warning; sourceIndex remains stable even when other voices are excluded.
bool importExternalPresets(InstrumentType target, const std::string& path,
                           const std::vector<uint8_t>& data, std::vector<ExternalPreset>& output,
                           std::string& warning);

// One Furnace instrument (a .fui container, new or legacy) for a SID, Sega
// PSG or Game Boy voice, converted as far as the engine can play it: macros it
// has no lane for are dropped, and `dropped` lists them. On failure `dropped`
// holds the reason. `nes` converts a NES instrument to a Game Boy one: 1 for a
// pulse or noise channel, 2 for the triangle channel (played as a 50% pulse).
bool importFurnaceInstrument(InstrumentType target, const std::vector<uint8_t>& data,
                             ExternalPreset& output, std::string& dropped, int nes = 0);
