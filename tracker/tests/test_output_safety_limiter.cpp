#include "doctest.h"
#include "output_safety_limiter.h"

#include <algorithm>
#include <cmath>
#include <vector>

TEST_CASE("output safety limiter controls sustained level and isolated peaks") {
  OutputSafetyLimiter limiter;
  std::vector<int16_t> block(1024, 32767);
  std::vector<float> scratch(block.size());
  for (int i = 0; i < 100; ++i) {
    for (size_t j = 0; j < block.size(); ++j) block[j] = j & 2 ? 32767 : -32768;
    limiter.process(block.data(), scratch.data(), 512, 48000);
    double power = 0.0;
    for (int16_t sample : block) power += (sample / 32768.0) * (sample / 32768.0);
    CHECK(std::sqrt(power / block.size()) <= 0.351);
  }

  limiter.reset();
  std::fill(block.begin(), block.end(), 0);
  block[42] = 32767;
  limiter.process(block.data(), scratch.data(), 512, 48000);
  CHECK(block[42] <= 26214);

  limiter.reset();
  for (int i = 0; i < 100; ++i) {
    std::fill(block.begin(), block.end(), 10000);
    limiter.process(block.data(), scratch.data(), 512, 48000);
  }
  CHECK(*std::max_element(block.begin(), block.end()) < 100);
}

TEST_CASE("output DC blocker preserves 30 Hz bass") {
  OutputSafetyLimiter limiter;
  std::vector<int16_t> block(1024);
  std::vector<float> scratch(block.size());
  double power = 0.0;
  int count = 0;
  for (int b = 0; b < 188; ++b) {
    for (int frame = 0; frame < 512; ++frame) {
      const float phase = 6.28318530718f * 30.0f * (b * 512 + frame) / 48000.0f;
      block[frame * 2] = block[frame * 2 + 1] = static_cast<int16_t>(6553.0f * std::sin(phase));
    }
    limiter.process(block.data(), scratch.data(), 512, 48000);
    if (b >= 94)
      for (int frame = 0; frame < 512; ++frame) {
        const double sample = block[frame * 2] / 32768.0;
        power += sample * sample;
        ++count;
      }
  }
  CHECK(std::sqrt(power / count) > 0.12);
}
