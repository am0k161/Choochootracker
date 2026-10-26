#include "doctest.h"
#include "synth/multimode_filter.h"

#include <cmath>
#include <initializer_list>

namespace {
float toneRms(uint8_t character, uint8_t mode, bool slope24, float frequency,
              uint8_t color = 128) {
  MultimodeFilter filter;
  filter.init(48000.0f);
  filter.configure(true, character, mode, slope24, 1000.0f, 0.4f, color);
  double power = 0.0;
  for (int i = 0; i < 8192; ++i) {
    const float input = 0.35f * sinf(6.28318530718f * frequency * i / 48000.0f);
    const float output = filter.process(input);
    if (i >= 4096) power += output * output;
  }
  return sqrtf((float)(power / 4096.0));
}
}

TEST_CASE("filter cutoff control is exponential and round-trips") {
  CHECK(filterCutoffFromControl(0) == doctest::Approx(20.0f));
  CHECK(filterCutoffFromControl(255) == doctest::Approx(20000.0f));
  CHECK(filterCutoffFromControl(128) < 1000.0f);
  for (int value = 0; value <= 255; ++value) {
    CHECK(std::abs((int)filterControlFromCutoff(filterCutoffFromControl(value)) - value) <= 1);
  }
}

TEST_CASE("Aggro and Acid provide distinct LP BP HP responses at both slopes") {
  for (uint8_t character : {uint8_t(3), uint8_t(4)}) {
    for (bool slope24 : {false, true}) {
      const float lpBass = toneRms(character, 0, slope24, 120.0f);
      const float lpTreble = toneRms(character, 0, slope24, 6000.0f);
      const float bpBass = toneRms(character, 1, slope24, 120.0f);
      const float bpMiddle = toneRms(character, 1, slope24, 1000.0f);
      const float bpTreble = toneRms(character, 1, slope24, 6000.0f);
      const float hpBass = toneRms(character, 2, slope24, 120.0f);
      const float hpTreble = toneRms(character, 2, slope24, 6000.0f);
      CHECK(lpBass > lpTreble * 2.0f);
      CHECK(bpMiddle > bpBass);
      CHECK(bpMiddle > bpTreble);
      CHECK(hpTreble > hpBass * 2.0f);
    }
    CHECK(toneRms(character, 0, true, 6000.0f) <
          toneRms(character, 0, false, 6000.0f));
    CHECK(toneRms(character, 2, true, 120.0f) <
          toneRms(character, 2, false, 120.0f));
  }
}

TEST_CASE("Classic slope switches between one and two filter stages") {
  CHECK(toneRms(2, 0, true, 6000.0f) <
        toneRms(2, 0, false, 6000.0f) * 0.4f);
  CHECK(toneRms(2, 2, true, 120.0f) <
        toneRms(2, 2, false, 120.0f) * 0.4f);
}

TEST_CASE("creative Color changes the sound and channels remain independent") {
  for (uint8_t character : {uint8_t(5), uint8_t(6), uint8_t(7)}) {
   for (uint8_t mode : {uint8_t(0), uint8_t(1), uint8_t(2)}) {
    MultimodeFilter low, middle, high, isolated;
    low.init(48000.0f); middle.init(48000.0f); high.init(48000.0f); isolated.init(48000.0f);
    low.configure(true, character, mode, false, 900.0f, 0.7f, 0);
    middle.configure(true, character, mode, false, 900.0f, 0.7f, 128);
    high.configure(true, character, mode, false, 900.0f, 0.7f, 255);
    isolated.configure(true, character, mode, false, 900.0f, 0.7f, 0);
    double lowerDifference = 0.0, upperDifference = 0.0;
    for (int i = 0; i < 4096; ++i) {
      const float input = 0.4f * sinf(0.043f * i) + 0.2f * sinf(0.19f * i);
      const float a = low.process(input, 0);
      const float b = middle.process(input, 0);
      const float c = high.process(input, 0);
      lowerDifference += fabsf(a - b);
      upperDifference += fabsf(b - c);
      CHECK(low.process(0.0f, 1) == doctest::Approx(0.0f));
      CHECK(a == doctest::Approx(isolated.process(input, 0)));
    }
    CHECK(upperDifference > 40.0);
    CHECK(upperDifference > lowerDifference * 1.25);
   }
  }
}

TEST_CASE("filter bypass is exact and all characters stay finite on hot sweeps") {
  for (float rate : {48000.0f, 96000.0f}) {
    MultimodeFilter filter;
    filter.init(rate);
    filter.configure(false, 3, 0, true, 1000.0f, 1.0f);
    CHECK(filter.process(0.375f) == 0.375f);
    for (uint8_t character = 3; character <= 7; ++character)
      for (uint8_t mode = 0; mode < 3; ++mode)
        for (bool slope24 : {false, true}) {
          filter.configure(true, character, mode, slope24, 1000.0f, 1.0f, 255);
          for (int i = 0; i < 4096; ++i) {
            if (!(i % 256))
              filter.configure(true, character, mode, slope24,
                               20.0f + 19980.0f * i / 4096.0f, 1.0f, 255);
            const float output = filter.process(2.0f * sinf(i * 0.13f));
            REQUIRE(std::isfinite(output));
            CHECK(fabsf(output) <= 1.001f);
          }
        }
  }
}
