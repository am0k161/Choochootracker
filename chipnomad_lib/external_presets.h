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

// One Furnace instrument (a .fui container, new or legacy) for a SID or Sega
// PSG voice, converted as far as the engine can play it: macros it has no lane
// for are dropped, and `dropped` lists them. On failure `dropped` holds the
// reason.
bool importFurnaceInstrument(InstrumentType target, const std::vector<uint8_t>& data,
                             ExternalPreset& output, std::string& dropped);
