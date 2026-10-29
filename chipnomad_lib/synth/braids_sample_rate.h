#ifndef CHOOCHOO_BRAIDS_SAMPLE_RATE_H
#define CHOOCHOO_BRAIDS_SAMPLE_RATE_H

#include <math.h>
#include <stdint.h>

inline int16_t braidsNativePitchOffset(float sampleRate) {
  const float rate = sampleRate > 0.0f ? sampleRate : 96000.0f;
  return static_cast<int16_t>(lroundf(1536.0f * log2f(96000.0f / rate)));
}

#endif
