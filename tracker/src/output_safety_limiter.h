#ifndef CHOOCHOO_OUTPUT_SAFETY_LIMITER_H
#define CHOOCHOO_OUTPUT_SAFETY_LIMITER_H

#include <algorithm>
#include <cmath>
#include <cstdint>

class OutputSafetyLimiter {
 public:
  OutputSafetyLimiter() { reset(48000); }

  void reset(int sampleRate) {
    const int rate = sampleRate > 0 ? sampleRate : 48000;
    gain_ = 1.0f;
    previousInput_[0] = previousInput_[1] = 0.0f;
    previousOutput_[0] = previousOutput_[1] = 0.0f;
    dcDecay_ = std::exp(-94.24778f / rate); // 15 Hz high-pass
    releasePerFrame_ = 1.0f / (0.25f * rate);
  }

  void process(float* mix, int16_t* output, int frames) {
    if (!mix || !output || frames <= 0) return;
    const int count = frames * 2;
    float power = 0.0f;
    float peak = 0.0f;
    for (int i = 0; i < count; ++i) {
      const float raw = std::isfinite(mix[i]) ? mix[i] : 0.0f;
      const float sample = std::clamp(raw, -1.0f, 32767.0f / 32768.0f);
      const int channel = i & 1;
      const float filtered = sample - previousInput_[channel] + dcDecay_ * previousOutput_[channel];
      previousInput_[channel] = sample;
      previousOutput_[channel] = filtered;
      mix[i] = filtered;
      power += filtered * filtered;
      peak = std::max(peak, std::fabs(filtered));
    }
    float target = 1.0f;
    if (peak > 0.8f) target = 0.8f / peak;
    const float meanSquare = power / count;
    if (meanSquare > 0.35f * 0.35f)
      target = std::min(target, 0.35f / std::sqrt(meanSquare));
    // Instant attack protects this already-rendered block; 250 ms release
    // avoids repeatedly raising and dropping the gain on dense passages.
    gain_ = std::min(target, gain_ + frames * releasePerFrame_);
    for (int i = 0; i < count; ++i) {
      const float sample = mix[i] * gain_;
      output[i] = sample >= 1.0f ? 32767 : sample <= -1.0f ? -32768 :
        static_cast<int16_t>(sample * 32767.0f);
    }
  }

 private:
  float gain_ = 1.0f;
  float dcDecay_ = 0.0f;
  float releasePerFrame_ = 0.0f;
  float previousInput_[2] = {};
  float previousOutput_[2] = {};
};

#endif
