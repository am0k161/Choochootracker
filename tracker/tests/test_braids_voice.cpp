#include "doctest.h"
#include "../../chipnomad_lib/synth/braids_voice.h"

#include <cmath>
#include <vector>

static int positiveCrossings(const std::vector<float>& samples, size_t start) {
  int crossings = 0;
  for (size_t i = start + 1; i < samples.size(); ++i)
    if (samples[i - 1] <= 0.0f && samples[i] > 0.0f) ++crossings;
  return crossings;
}

TEST_CASE("BraidsVoice renders every accessible model") {
  float buffer[4096];

  for (float rate : {32000.0f, 44100.0f, 48000.0f}) {
    for (int model = 0;
         model <= braids::MACRO_OSC_SHAPE_LAST_ACCESSIBLE_FROM_META;
         ++model) {
      BraidsVoice voice;
      voice.init(rate);
      REQUIRE(voice.setModel(model));
      voice.setPitch(60 << 7);
      voice.setParameters(16384, 16384);
      voice.strike();
      voice.render(buffer, 4096);

      double energy = 0.0;
      bool finite = true;
      float minimum = 1.0f;
      float maximum = -1.0f;
      for (float sample : buffer) {
        finite = finite && std::isfinite(sample);
        if (sample < minimum) minimum = sample;
        if (sample > maximum) maximum = sample;
        energy += std::fabs(sample);
      }
      CHECK(finite);
      CHECK(minimum >= -1.0f);
      CHECK(maximum < 1.0f);
      CHECK_MESSAGE(energy > 0.0, "silent Braids model: ", model, " at ", rate);
    }
  }
}

TEST_CASE("BraidsVoice rejects an unknown model") {
  BraidsVoice voice;
  voice.init();
  CHECK_FALSE(voice.setModel(255));
}

TEST_CASE("BraidsVoice keeps its pitch at every output sample rate") {
  int referenceCrossings = 0;
  for (float rate : {32000.0f, 44100.0f, 48000.0f, 96000.0f}) {
    BraidsVoice voice;
    std::vector<float> output((size_t)rate);
    voice.init(rate);
    voice.noteOn();
    voice.render(output.data(), output.size());
    const int crossings = positiveCrossings(output, (size_t)(rate / 10.0f));
    if (!referenceCrossings) referenceCrossings = crossings;
    CHECK(crossings == doctest::Approx(referenceCrossings).epsilon(0.01));
  }
}

TEST_CASE("BraidsVoice audio-rate envelope reaches silence after release") {
  BraidsVoice voice;
  float buffer[256];
  voice.init();
  voice.setEnvelope(true, 0.0f, 0.0f, 1.0f, 0.001f);
  voice.noteOn();
  voice.render(buffer, 128);
  voice.noteOff();
  voice.render(buffer, 256);

  for (size_t i = 128; i < 256; ++i) CHECK(buffer[i] == doctest::Approx(0.0f));
  CHECK_FALSE(voice.active());
}

TEST_CASE("BraidsVoice percussion models obey the ADSR release") {
  const uint8_t models[] = {
    braids::MACRO_OSC_SHAPE_STRUCK_DRUM,
    braids::MACRO_OSC_SHAPE_KICK,
    braids::MACRO_OSC_SHAPE_CYMBAL,
    braids::MACRO_OSC_SHAPE_SNARE,
  };
  float buffer[512];
  for (uint8_t model : models) {
    BraidsVoice voice;
    voice.init();
    REQUIRE(voice.setModel(model));
    voice.setEnvelope(true, 0.0f, 0.0f, 1.0f, 0.001f);
    voice.noteOn();
    voice.render(buffer, 128);
    voice.noteOff();
    voice.render(buffer, 512);
    CHECK_FALSE(voice.active());
    for (size_t i = 128; i < 512; ++i) {
      CHECK(buffer[i] == doctest::Approx(0.0f));
    }
  }
}

TEST_CASE("BraidsVoice filter modes and slopes remain stable") {
  for (int mode = 0; mode < 3; ++mode) {
    for (int slope24dB = 0; slope24dB < 2; ++slope24dB) {
      BraidsVoice voice;
      float buffer[4096];
      voice.init();
      voice.setFilter(true, static_cast<BraidsFilterMode>(mode), slope24dB,
                      12000.0f, 1.0f);
      voice.noteOn();
      voice.render(buffer, 4096);
      for (float sample : buffer) CHECK(std::isfinite(sample));
    }
  }
}
