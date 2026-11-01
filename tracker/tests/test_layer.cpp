#include "doctest.h"
#include "chipnomad_lib.h"
#include "pitch_table_utils.h"
#include "project_utils.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <vector>

TEST_CASE("Layer defaults and quadratic transpose are stable") {
  Instrument layer{};
  getInstrumentFunctions(InstrumentType::Layer).init(&layer);
  CHECK((int)InstrumentType::Layer == 31);
  CHECK(getInstrumentDefinition(InstrumentType::Layer)->category == InstrumentCategory::utility);
  const auto* definition = getInstrumentDefinition(InstrumentType::Layer);
  CHECK(definition->functions.modDestinationsCount == 7);
  CHECK(definition->fxCount == 6);
  const char* destinations[] = {"Volume", "L1 Cents", "L1 Vol", "L2 Cents", "L2 Vol", "L3 Cents", "L3 Vol"};
  for (int destination = 1; destination <= 7; ++destination)
    CHECK(std::strcmp(instrumentModDestinationName(InstrumentType::Layer, destination), destinations[destination - 1]) == 0);
  fillFXNames();
  const FX layerFX[] = {fxL1C, fxL1V, fxL2C, fxL2V, fxL3C, fxL3V};
  const char* layerFXNames[] = {"L1C", "L1V", "L2C", "L2V", "L3C", "L3V"};
  for (int i = 0; i < 6; ++i) {
    CHECK(instrumentFXAvailable(InstrumentType::Layer, layerFX[i]));
    CHECK(std::strcmp(fxNames[layerFX[i]].name, layerFXNames[i]) == 0);
  }
  for (const auto& entry : layer.chip.layer.entries) {
    CHECK(entry.instrument == EMPTY_VALUE_8);
    CHECK(entry.transpose == 0x80);
    CHECK(entry.volume == 0x80);
  }
  CHECK(instrumentLayerTransposeCents(0x00) == -2400);
  CHECK(instrumentLayerTransposeCents(0x80) == 0);
  CHECK(instrumentLayerTransposeCents(0xff) == 2400);
  CHECK(instrumentLayerTransposeCents(0x81) < instrumentLayerTransposeCents(0xc0));
  bool positiveNotes[25]{}, negativeNotes[25]{};
  for (int value = 0; value < 256; ++value) {
    int cents = instrumentLayerTransposeCents((uint8_t)value);
    if (std::abs(cents) >= 100) {
      CHECK(cents % 100 == 0);
      (cents > 0 ? positiveNotes : negativeNotes)[std::abs(cents) / 100] = true;
    }
  }
  for (int note = 1; note <= 24; ++note) {
    CHECK(positiveNotes[note]);
    CHECK(negativeNotes[note]);
  }
}

TEST_CASE("Layer project and preset preserve slot references") {
  auto saved = std::make_unique<Project>(), loaded = std::make_unique<Project>();
  projectInit(saved.get()); projectInit(loaded.get());
  saved->chipsCount = saved->tracksCount = 1;
  saved->tickRate = 50;
  saved->chipSetup.ay.clock = 1773400;
  calculateLinearPitchTable12TET(saved.get());
  getInstrumentFunctions(InstrumentType::Layer).init(&saved->instruments[4]);
  auto& entries = saved->instruments[4].chip.layer.entries;
  entries[0] = {2, 0x00, 0x20};
  entries[1] = {7, 0x80, 0x80};
  entries[2] = {127, 0xff, 0xff};
  const char* preset = "build/tests/layer.cni";
  REQUIRE(instrumentSave(saved.get(), preset, 4) == 0);
  REQUIRE(instrumentLoad(loaded.get(), preset, 9) == 0);
  CHECK(loaded->instruments[9].type == InstrumentType::Layer);
  for (int i = 0; i < INSTRUMENT_LAYER_COUNT; ++i) {
    CHECK(loaded->instruments[9].chip.layer.entries[i].instrument == entries[i].instrument);
    CHECK(loaded->instruments[9].chip.layer.entries[i].transpose == entries[i].transpose);
    CHECK(loaded->instruments[9].chip.layer.entries[i].volume == entries[i].volume);
  }

  const char* song = "build/tests/layer.cct";
  REQUIRE(projectSave(saved.get(), song) == 0);
  REQUIRE(projectLoad(loaded.get(), song) == 0);
  CHECK(projectFileVersion == 12);
  CHECK(loaded->instruments[4].type == InstrumentType::Layer);
  for (int i = 0; i < INSTRUMENT_LAYER_COUNT; ++i) {
    CHECK(loaded->instruments[4].chip.layer.entries[i].instrument == entries[i].instrument);
    CHECK(loaded->instruments[4].chip.layer.entries[i].transpose == entries[i].transpose);
    CHECK(loaded->instruments[4].chip.layer.entries[i].volume == entries[i].volume);
  }
  std::remove(preset);
  std::remove(song);
  projectFree(saved.get()); projectFree(loaded.get());
}

TEST_CASE("Layer loader normalizes invalid slot values and rejects invalid controls") {
  auto saved = std::make_unique<Project>(), loaded = std::make_unique<Project>();
  projectInit(saved.get()); projectInit(loaded.get());
  getInstrumentFunctions(InstrumentType::Layer).init(&saved->instruments[0]);
  const char* path = "build/tests/layer-invalid.cni";
  REQUIRE(instrumentSave(saved.get(), path, 0) == 0);
  std::ifstream input(path);
  std::stringstream bytes; bytes << input.rdbuf(); input.close();
  std::string text = bytes.str();
  size_t line = text.find("- Layer: 255,128,128");
  REQUIRE(line != std::string::npos);
  text.replace(line, std::strlen("- Layer: 255,128,128"), "- Layer: 200,128,128");
  std::ofstream(path, std::ios::trunc) << text;
  REQUIRE(instrumentLoad(loaded.get(), path, 1) == 0);
  CHECK(loaded->instruments[1].chip.layer.entries[0].instrument == EMPTY_VALUE_8);

  text.replace(text.find("- Layer: 200,128,128"), std::strlen("- Layer: 200,128,128"),
               "- Layer: 2,256,128");
  std::ofstream(path, std::ios::trunc) << text;
  CHECK(instrumentLoad(loaded.get(), path, 2) != 0);
  std::remove(path);
  projectFree(saved.get()); projectFree(loaded.get());
}

TEST_CASE("Layer references follow swaps, survive cleanup and expose child FX") {
  Project project{};
  projectInit(&project);
  getInstrumentFunctions(InstrumentType::Layer).init(&project.instruments[0]);
  getInstrumentFunctions(InstrumentType::DrumSynth).init(&project.instruments[2]);
  project.instruments[0].chip.layer.entries[0].instrument = 2;
  project.phrases[0].rows[0].instrument = 0;
  project.phrases[0].rows[0].note = 40;
  CHECK(instrumentFXAvailableInProject(&project, 0, fxDDC));
  CHECK_FALSE(instrumentFXAvailableInProject(&project, 0, fxBTM));
  instrumentSwap(&project, 2, 3);
  CHECK(project.instruments[0].chip.layer.entries[0].instrument == 3);
  cleanupUnusedInstruments(&project);
  CHECK(project.instruments[3].type == InstrumentType::DrumSynth);
  projectFree(&project);
}

TEST_CASE("Layer renders an Audio in child on an independent lane") {
  auto state = std::unique_ptr<ChipNomadState, decltype(&chipnomadDestroy)>(chipnomadCreate(), chipnomadDestroy);
  Project& project = state->project;
  project.chipsCount = project.tracksCount = 1;
  project.tickRate = 50;
  project.linearPitch = 1;
  project.chipSetup.ay.clock = 1773400;
  calculateLinearPitchTable12TET(&project);
  getInstrumentFunctions(InstrumentType::Layer).init(&project.instruments[0]);
  getInstrumentFunctions(InstrumentType::AudioIn).init(&project.instruments[1]);
  project.instruments[0].chip.layer.entries[0].instrument = 1;
  project.instruments[0].chip.layer.entries[0].volume = 0x80;
  project.instruments[0].volume = project.instruments[1].volume = 255;
  project.trackVolume[0] = 100;
  chipnomadInitChips(state.get(), 48000, nullptr);
  REQUIRE(chipnomadReserveRenderBuffers(state.get(), 1024) == 0);
  chipnomadQueuePlaybackPreviewNote(state.get(), 0, 45, 0);
  float input[2048], output[2048]{};
  for (int i = 0; i < 1024; ++i) { input[i * 2] = .25f; input[i * 2 + 1] = -.125f; }
  REQUIRE(chipnomadRender(state.get(), output, 1024, input) == 1024);
  CHECK(output[400] > .05f);
  CHECK(output[401] < -.02f);
  CHECK(state->voiceMonitors[0].active);
  CHECK(state->voiceMonitors[PROJECT_MAX_TRACKS].active);
  const PlaybackStatus* status = chipnomadGetPlaybackStatus(state.get());
  REQUIRE(status != nullptr);
  CHECK(status->layerTracks[0][0].instrument == 1);
}

TEST_CASE("Layer modulation controls each lane's cents and volume") {
  auto state = std::unique_ptr<ChipNomadState, decltype(&chipnomadDestroy)>(chipnomadCreate(), chipnomadDestroy);
  Project& project = state->project;
  project.chipsCount = project.tracksCount = 1;
  project.tickRate = 50;
  project.linearPitch = 1;
  project.chipSetup.ay.clock = 1773400;
  calculateLinearPitchTable12TET(&project);
  getInstrumentFunctions(InstrumentType::Layer).init(&project.instruments[0]);
  getInstrumentFunctions(InstrumentType::AudioIn).init(&project.instruments[1]);
  Instrument& layer = project.instruments[0];
  layer.chip.layer.entries[0].instrument = 1;
  layer.chip.layer.entries[0].volume = 255;
  layer.modulation[0] = {ModulationType::StickLinear, 2, 127, 0, 0, 0, 0, 0};
  layer.modulation[1] = {ModulationType::StickLinear, 3, 127, 1, 0, 0, 0, 0};
  chipnomadSetLiveStickEnabled(1);
  chipnomadSetLiveStickAxes(1.0f, -1.0f, 0.0f, 0.0f);
  chipnomadInitChips(state.get(), 48000, nullptr);
  REQUIRE(chipnomadReserveRenderBuffers(state.get(), 1024) == 0);
  chipnomadQueuePlaybackPreviewNote(state.get(), 0, 45, 0);
  float input[2048]{}, output[2048]{};
  CHECK(chipnomadRender(state.get(), output, 1024, input) == 1024);
  chipnomadSetLiveStickEnabled(0);
  chipnomadSetLiveStickAxes(0.0f, 0.0f, 0.0f, 0.0f);
  const auto& child = state->playbackState.layerTracks[0][0];
  CHECK(child.note.fineOffset == 2400);
  CHECK(child.note.volume1 == 0);
  CHECK(state->playbackState.tracks[0].liveInstrument == 0);
  CHECK(state->playbackState.tracks[0].liveControls[liveSource1] == 255);
  CHECK(state->playbackState.tracks[0].liveControls[liveSource2] == 0);
}

static std::vector<float> renderLayerMonk(uint8_t transpose) {
  auto state = std::unique_ptr<ChipNomadState, decltype(&chipnomadDestroy)>(chipnomadCreate(), chipnomadDestroy);
  Project& project = state->project;
  project.chipsCount = project.tracksCount = 1;
  project.tickRate = 50;
  project.linearPitch = 1;
  project.chipSetup.ay.clock = 1773400;
  calculateLinearPitchTable12TET(&project);
  getInstrumentFunctions(InstrumentType::Layer).init(&project.instruments[0]);
  getInstrumentFunctions(InstrumentType::Monk).init(&project.instruments[1]);
  project.instruments[0].chip.layer.entries[0] = {1, transpose, 255};
  project.instruments[0].volume = project.instruments[1].volume = 255;
  project.trackVolume[0] = 100;
  chipnomadInitChips(state.get(), 48000, nullptr);
  REQUIRE(chipnomadReserveRenderBuffers(state.get(), 4096) == 0);
  chipnomadQueuePlaybackPreviewNote(state.get(), 0, 45, 0);
  std::vector<float> output(8192);
  REQUIRE(chipnomadRender(state.get(), output.data(), 4096) == 4096);
  CHECK(state->playbackState.layerTracks[0][0].note.fineOffset == instrumentLayerTransposeCents(transpose));
  return output;
}

TEST_CASE("Layer repitches MonkSynth") {
  const auto center = renderLayerMonk(0x80);
  const auto high = renderLayerMonk(0xff);
  double centerEnergy = 0.0, highEnergy = 0.0, difference = 0.0;
  for (size_t i = 0; i < center.size(); ++i) {
    centerEnergy += center[i] * center[i];
    highEnergy += high[i] * high[i];
    difference += std::fabs(center[i] - high[i]);
  }
  CHECK(centerEnergy > 0.001);
  CHECK(highEnergy > 0.001);
  CHECK(difference > 1.0);
}

TEST_CASE("Layer repitches an active MonkSynth voice") {
  auto state = std::unique_ptr<ChipNomadState, decltype(&chipnomadDestroy)>(chipnomadCreate(), chipnomadDestroy);
  Project& project = state->project;
  project.chipsCount = project.tracksCount = 1; project.tickRate = 50; project.linearPitch = 1;
  project.chipSetup.ay.clock = 1773400; calculateLinearPitchTable12TET(&project);
  getInstrumentFunctions(InstrumentType::Layer).init(&project.instruments[0]);
  getInstrumentFunctions(InstrumentType::Monk).init(&project.instruments[1]);
  project.instruments[0].chip.layer.entries[0] = {1, 0x80, 255};
  project.instruments[0].volume = project.instruments[1].volume = 255; project.trackVolume[0] = 100;
  chipnomadInitChips(state.get(), 48000, nullptr); REQUIRE(chipnomadReserveRenderBuffers(state.get(), 4096) == 0);
  chipnomadQueuePlaybackPreviewNote(state.get(), 0, 45, 0);
  std::vector<float> center(8192), high(8192);
  REQUIRE(chipnomadRender(state.get(), center.data(), 4096) == 4096);
  project.instruments[0].chip.layer.entries[0].transpose = 0xff;
  REQUIRE(chipnomadQueueProjectRefresh(state.get()));
  REQUIRE(chipnomadRender(state.get(), high.data(), 4096) == 4096);
  CHECK(state->playbackState.layerTracks[0][0].note.fineOffset == 2400);
  double difference = 0.0;
  for (size_t i = 0; i < center.size(); ++i) difference += std::fabs(center[i] - high[i]);
  CHECK(difference > 1.0);
}

TEST_CASE("Layer lane FX override cents and volume") {
  auto state = std::unique_ptr<ChipNomadState, decltype(&chipnomadDestroy)>(chipnomadCreate(), chipnomadDestroy);
  Project& project = state->project;
  project.chipsCount = project.tracksCount = 1; project.tickRate = 50; project.linearPitch = 1;
  project.chipSetup.ay.clock = 1773400; calculateLinearPitchTable12TET(&project);
  for (auto& speed : project.grooves[0].speed) speed = 50;
  getInstrumentFunctions(InstrumentType::Layer).init(&project.instruments[0]);
  getInstrumentFunctions(InstrumentType::Monk).init(&project.instruments[1]);
  project.instruments[0].chip.layer.entries[0] = {1, 0x80, 255};
  project.song[0][0] = 0; project.chains[0].rows[0].phrase = 0;
  auto& row = project.phrases[0].rows[0];
  row.note = 45; row.instrument = 0; row.volume = PHRASE_VOLUME_MAX;
  row.fx[0] = {fxL1C, 0xff}; row.fx[1] = {fxL1V, 0x00};
  chipnomadInitChips(state.get(), 48000, nullptr); REQUIRE(chipnomadReserveRenderBuffers(state.get(), 1024) == 0);
  REQUIRE(chipnomadQueueProjectRefresh(state.get()));
  REQUIRE(chipnomadQueuePlaybackStartSong(state.get(), 0, 0, 1));
  float output[2048]{}; REQUIRE(chipnomadRender(state.get(), output, 1024) == 1024);
  const auto& child = state->playbackState.layerTracks[0][0];
  CHECK(child.note.fineOffset == 2400);
  CHECK(child.note.volume1 == 0);
}

TEST_CASE("Layer gives legacy AY children a virtual chip and honors zero lane volume") {
  auto state = std::unique_ptr<ChipNomadState, decltype(&chipnomadDestroy)>(chipnomadCreate(), chipnomadDestroy);
  Project& project = state->project;
  project.chipsCount = project.tracksCount = 1;
  project.tickRate = 50;
  project.linearPitch = 1;
  project.chipSetup.ay.clock = 1773400;
  calculateLinearPitchTable12TET(&project);
  getInstrumentFunctions(InstrumentType::Layer).init(&project.instruments[0]);
  getInstrumentFunctions(InstrumentType::AY1).init(&project.instruments[1]);
  project.instruments[0].chip.layer.entries[0].instrument = 1;
  project.instruments[0].chip.layer.entries[0].volume = 0x80;
  project.trackVolume[0] = 100;
  chipnomadInitChips(state.get(), 48000, nullptr);
  REQUIRE(state->layerAYChips[PROJECT_MAX_TRACKS] != nullptr);
  REQUIRE(chipnomadReserveRenderBuffers(state.get(), 2048) == 0);
  chipnomadQueuePlaybackPreviewNote(state.get(), 0, 48, 0);
  float audible[4096]{};
  REQUIRE(chipnomadRender(state.get(), audible, 2048) == 2048);
  const auto& child = state->playbackState.layerTracks[0][0];
  CHECK(child.note.instrument == 1);
  CHECK(child.note.pitchFinal != EMPTY_VALUE_8);
  CHECK(child.note.chip.ay.outVolume > 0);
  float energy = 0.0f;
  for (float sample : audible) energy += std::fabs(sample);
  CHECK(energy > 0.01f);

  state->project.instruments[0].chip.layer.entries[0].volume = 0;
  chipnomadInitChips(state.get(), 48000, nullptr);
  chipnomadQueuePlaybackPreviewNote(state.get(), 0, 48, 0);
  float silent[4096]{};
  REQUIRE(chipnomadRender(state.get(), silent, 2048) == 2048);
  float silentEnergy = 0.0f;
  for (float sample : silent) silentEnergy += std::fabs(sample);
  CHECK(silentEnergy == doctest::Approx(0.0f).epsilon(0.000001));
}
