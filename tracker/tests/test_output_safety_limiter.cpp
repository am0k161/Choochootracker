#include "doctest.h"
#include "output_safety_limiter.h"

#include <algorithm>
#include <cmath>
#include <vector>

TEST_CASE("output safety limiter controls sustained level and isolated peaks") {
  OutputSafetyLimiter limiter;
  std::vector<float> mix(1024);
  std::vector<int16_t> block(mix.size());
  limiter.reset(48000);
  for (int i = 0; i < 100; ++i) {
    for (size_t j = 0; j < mix.size(); ++j) mix[j] = j & 2 ? 1.0f : -1.0f;
    limiter.process(mix.data(), block.data(), 512);
    double power = 0.0;
    for (int16_t sample : block) power += (sample / 32768.0) * (sample / 32768.0);
    CHECK(std::sqrt(power / block.size()) <= 0.351);
  }

  limiter.reset(48000);
  std::fill(mix.begin(), mix.end(), 0.0f);
  mix[42] = 1.0f;
  limiter.process(mix.data(), block.data(), 512);
  CHECK(block[42] <= 26214);

  limiter.reset(48000);
  for (int i = 0; i < 100; ++i) {
    std::fill(mix.begin(), mix.end(), 10000.0f / 32768.0f);
    limiter.process(mix.data(), block.data(), 512);
  }
  CHECK(*std::max_element(block.begin(), block.end()) < 100);
}

TEST_CASE("output DC blocker preserves 30 Hz bass") {
  OutputSafetyLimiter limiter;
  std::vector<float> mix(1024);
  std::vector<int16_t> block(mix.size());
  limiter.reset(48000);
  double power = 0.0;
  int count = 0;
  for (int b = 0; b < 188; ++b) {
    for (int frame = 0; frame < 512; ++frame) {
      const float phase = 6.28318530718f * 30.0f * (b * 512 + frame) / 48000.0f;
      mix[frame * 2] = mix[frame * 2 + 1] = 0.2f * std::sin(phase);
    }
    limiter.process(mix.data(), block.data(), 512);
    if (b >= 94)
      for (int frame = 0; frame < 512; ++frame) {
        const double sample = block[frame * 2] / 32768.0;
        power += sample * sample;
        ++count;
      }
  }
  CHECK(std::sqrt(power / count) > 0.12);
}

TEST_CASE("output limiter sanitizes invalid samples at supported rates") {
  for (int rate : {32000, 44100, 48000}) {
    OutputSafetyLimiter limiter;
    limiter.reset(rate);
    float mix[] = {NAN, INFINITY, -INFINITY, 2.0f, -2.0f, 0.0f};
    int16_t output[6]{};
    limiter.process(mix, output, 3);
    for (int16_t sample : output) CHECK(std::abs((int)sample) <= 26214);
  }
}

TEST_CASE("output limiter attenuates overload before conversion") {
  OutputSafetyLimiter limiter;
  limiter.reset(48000);
  std::vector<float> mix(1024);
  std::vector<int16_t> output(mix.size());
  for (int frame = 0; frame < 512; ++frame) {
    const float sign = frame & 1 ? -1.0f : 1.0f;
    mix[frame * 2] = sign * 2.0f;
    mix[frame * 2 + 1] = sign;
  }
  limiter.process(mix.data(), output.data(), 512);
  const int left = std::abs((int)output[400]);
  const int right = std::abs((int)output[401]);
  CHECK(left <= 26214);
  CHECK(left == doctest::Approx(right * 2).epsilon(0.02));
}
