#include "doctest.h"
#include "chipnomad_lib.h"
#include "pitch_table_utils.h"
#include "synth/audio_input_voice.h"
#include "audio_input_manager.h"
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

TEST_CASE("Audio in gates stereo with ADSR and filters both channels") {
  Instrument instrument{};
  getInstrumentFunctions(InstrumentType::AudioIn).init(&instrument);
  auto& p = instrument.chip.audioIn;
  p.filterEnabled = 0;
  p.release = 8;
  AudioInputVoice voice;
  voice.init(48000);
  voice.configure(p, 1);
  float input[512], output[512];
  for (int i = 0; i < 256; ++i) { input[i * 2] = .25f; input[i * 2 + 1] = -.5f; }
  voice.setInput(input);
  voice.render(output, 256);
  for (float sample : output) CHECK(sample == 0);
  voice.noteOn(); voice.render(output, 256);
  for (int i = 0; i < 512; ++i) CHECK(output[i] == input[i]);
  voice.noteOff(); voice.render(output, 256);
  CHECK(output[0] > 0);
  CHECK(output[511] == 0);
  CHECK_FALSE(voice.active());
  p.filterEnabled = 1; p.filterCharacter = 1; p.filterCutoffHz = 100;
  voice.configure(p, 1); voice.noteOn(); voice.render(output, 256);
  CHECK(std::fabs(output[0]) < .01f);
  for (int i = 0; i < 256; ++i) CHECK(output[i * 2 + 1] == doctest::Approx(-2 * output[i * 2]));
  voice.kill(); voice.render(output, 256);
  for (float sample : output) CHECK(sample == 0);
  p.filterEnabled = 0; voice.configure(p, 1); voice.noteOn(); voice.setInput(nullptr);
  voice.render(output, 256);
  for (float sample : output) CHECK(sample == 0);
}

TEST_CASE("Audio in capture is stereo and independent from dry monitoring") {
  AudioInputManager manager;
  AudioInputManagerTestPeer::captureOpenForTest(manager, true);
  AudioInputManagerTestPeer::armSlotForTest(manager, 0, 0);
  manager.setMonitor(1);
  int16_t input[] = {8192, -16384, 4096, -8192};
  manager.onCapture(input, 2, 2);
  float live[8]; manager.readLiveInput(live, 4);
  CHECK(live[0] == .25f); CHECK(live[1] == -.5f);
  CHECK(live[2] == .125f); CHECK(live[3] == -.25f);
  for (int i = 4; i < 8; ++i) CHECK(live[i] == 0);
  int16_t monitor[4]{}; manager.mixMonitor(monitor, 2);
  CHECK(monitor[0] == 4096); CHECK(monitor[1] == -8192);
  int16_t mono[] = {8192, -4096}; manager.onCapture(mono, 2, 1);
  manager.readLiveInput(live, 2);
  CHECK(live[0] == .25f); CHECK(live[1] == .25f);
  CHECK(live[2] == -.125f); CHECK(live[3] == -.125f);
  std::vector<int16_t> backlog(4096 * 2);
  for (int i = 0; i < 4096; ++i) { backlog[i * 2] = i; backlog[i * 2 + 1] = -i; }
  manager.onCapture(backlog.data(), 4096, 2);
  float recent[128]; manager.readLiveInput(recent, 64);
  CHECK(recent[0] == doctest::Approx(3008 / 32768.0f));
  CHECK(recent[1] == doctest::Approx(-3008 / 32768.0f));
}

TEST_CASE("Audio in preset preserves post settings and external category") {
  auto saved = std::make_unique<Project>(), loaded = std::make_unique<Project>();
  fillFXNames(); projectInit(saved.get()); projectInit(loaded.get());
  auto& instrument = saved->instruments[0];
  getInstrumentFunctions(InstrumentType::AudioIn).init(&instrument);
  instrument.chip.audioIn.attack = 47; instrument.chip.audioIn.release = 82;
  instrument.chip.audioIn.filterCutoffHz = 1234;
  const char* path = "build/tests/audio_in.cni";
  REQUIRE(instrumentSave(saved.get(), path, 0) == 0);
  REQUIRE(instrumentLoad(loaded.get(), path, 0) == 0);
  std::remove(path);
  CHECK(loaded->instruments[0].type == InstrumentType::AudioIn);
  CHECK(loaded->instruments[0].chip.audioIn.attack == 47);
  CHECK(loaded->instruments[0].chip.audioIn.release == 82);
  CHECK(loaded->instruments[0].chip.audioIn.filterCutoffHz == 1234);
  CHECK(getInstrumentDefinition(InstrumentType::AudioIn)->category == getInstrumentDefinition(InstrumentType::Midi)->category);
  projectFree(saved.get()); projectFree(loaded.get());
}

TEST_CASE("Audio in engine follows notes, chunk offsets, inserts and offline silence") {
  auto dry = std::unique_ptr<ChipNomadState, decltype(&chipnomadDestroy)>(chipnomadCreate(), chipnomadDestroy);
  auto wet = std::unique_ptr<ChipNomadState, decltype(&chipnomadDestroy)>(chipnomadCreate(), chipnomadDestroy);
  for (auto* s : {dry.get(), wet.get()}) {
    auto& p = s->project;
    p.chipsCount = p.tracksCount = 1; p.tickRate = 50; p.linearPitch = 1;
    calculateLinearPitchTable12TET(&p);
    getInstrumentFunctions(InstrumentType::AudioIn).init(&p.instruments[0]);
    p.instruments[0].chip.audioIn.filterEnabled = 0;
    p.instruments[0].volume = 255; p.trackVolume[0] = 100;
    p.trackReverbSend[0] = p.trackDelaySend[0] = 0;
    p.reverbReturn = p.delayReturn = 0;
    if (s == wet.get()) insertSelect(&p.trackInserts[0][0], insertDistortion);
    chipnomadInitChips(s, 48000, nullptr);
    REQUIRE(chipnomadReserveRenderBuffers(s, 2048) == 0);
    chipnomadQueuePlaybackPreviewNote(s, 0, 45, 0);
  }
  float input[4096], a[4096]{}, b[4096]{};
  for (int i = 0; i < 2048; ++i) { input[i * 2] = .2f + .1f * std::sin(i * .03f); input[i * 2 + 1] = -input[i * 2] * .5f; }
  REQUIRE(chipnomadRender(dry.get(), a, 2048, input) == 2048);
  REQUIRE(chipnomadRender(wet.get(), b, 2048, input) == 2048);
  float scale = a[1000] / input[1000];
  CHECK(scale > 0);
  double difference = 0;
  for (int i = 0; i < 4096; ++i) {
    CHECK(a[i] == doctest::Approx(input[i] * scale).epsilon(.001));
    CHECK(std::isfinite(b[i])); difference += std::fabs(a[i] - b[i]);
  }
  CHECK(difference > .01);
  REQUIRE(chipnomadRender(dry.get(), a, 2048) == 2048);
  for (float sample : a) CHECK(sample == 0);
  chipnomadQueuePlaybackStopPreview(dry.get(), 0);
  for (int i = 0; i < 3; ++i) { std::fill(a, a + 4096, 0); chipnomadRender(dry.get(), a, 2048, input); }
  for (float sample : a) CHECK(sample == 0);
}
