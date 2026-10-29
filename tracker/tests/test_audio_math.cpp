#include "doctest.h"

#include <algorithm>
#include <cmath>

#include "synth/audio_math.h"

TEST_CASE("audio tanh lookup stays close to libm") {
  float maximumError = 0.0f;
  for (int i = -32768; i <= 32768; ++i) {
    float x = i / 4096.0f;
    maximumError = std::max(maximumError, std::fabs(audioTanh(x) - std::tanh(x)));
  }
  CHECK(maximumError < 0.00003f);
  CHECK(audioTanh(-100.0f) == -1.0f);
  CHECK(audioTanh(100.0f) == 1.0f);
}

TEST_CASE("period-four audio remainder preserves fmod semantics") {
  for (int i = -524287; i <= 524287; ++i) {
    const float x = i / 4096.0f;
    CHECK(audioFmod4(x) == std::fmod(x, 4.0f));
  }
  CHECK(std::isnan(audioFmod4(NAN)));
  CHECK(audioFmod4(10000.25f) == std::fmod(10000.25f, 4.0f));
}
