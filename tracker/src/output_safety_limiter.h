#ifndef CHOOCHOO_OUTPUT_SAFETY_LIMITER_H
#define CHOOCHOO_OUTPUT_SAFETY_LIMITER_H

#include <algorithm>
#include <cmath>
#include <cstdint>

class OutputSafetyLimiter {
 public:
  void reset() {
    gain_ = 1.0f;
    previousInput_[0] = previousInput_[1] = 0.0f;
    previousOutput_[0] = previousOutput_[1] = 0.0f;
  }

  void process(int16_t* stereo, float* scratch, int frames, int sampleRate) {
    if (!stereo || !scratch || frames <= 0) return;
    const int count = frames * 2;
    const int rate = sampleRate > 0 ? sampleRate : 48000;
    const float dcDecay = std::exp(-94.24778f / rate); // 15 Hz high-pass
    double power = 0.0;
    float peak = 0.0f;
    for (int i = 0; i < count; ++i) {
      const float sample = stereo[i] / 32768.0f;
      const int channel = i & 1;
      const float filtered = sample - previousInput_[channel] + dcDecay * previousOutput_[channel];
      previousInput_[channel] = sample;
      previousOutput_[channel] = filtered;
      scratch[i] = filtered;
      power += filtered * filtered;
      peak = std::max(peak, std::fabs(filtered));
    }
    const float rms = std::sqrt(power / count);
    float target = 1.0f;
    if (peak > 0.8f) target = 0.8f / peak;
    if (rms > 0.35f) target = std::min(target, 0.35f / rms);
    // Instant attack protects this already-rendered block; 250 ms release
    // avoids repeatedly raising and dropping the gain on dense passages.
    const float release = frames / (0.25f * rate);
    gain_ = std::min(target, gain_ + release);
    for (int i = 0; i < count; ++i)
      stereo[i] = static_cast<int16_t>(scratch[i] * gain_ * 32767.0f);
  }

 private:
  float gain_ = 1.0f;
  float previousInput_[2] = {};
  float previousOutput_[2] = {};
};

#endif
