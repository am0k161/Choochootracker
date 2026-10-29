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
    float power = 0.0f;
    float peak = 0.0f;
    float previousInputLeft = previousInput_[0], previousInputRight = previousInput_[1];
    float previousOutputLeft = previousOutput_[0], previousOutputRight = previousOutput_[1];
    for (int frame = 0; frame < frames; ++frame) {
      const int i = frame * 2;
      const float left = sanitize(mix[i]);
      const float right = sanitize(mix[i + 1]);
      const float filteredLeft = left - previousInputLeft + dcDecay_ * previousOutputLeft;
      const float filteredRight = right - previousInputRight + dcDecay_ * previousOutputRight;
      previousInputLeft = left;
      previousInputRight = right;
      previousOutputLeft = filteredLeft;
      previousOutputRight = filteredRight;
      mix[i] = filteredLeft;
      mix[i + 1] = filteredRight;
      power += filteredLeft * filteredLeft + filteredRight * filteredRight;
      peak = std::max(peak, std::max(std::fabs(filteredLeft), std::fabs(filteredRight)));
    }
    previousInput_[0] = previousInputLeft;
    previousInput_[1] = previousInputRight;
    previousOutput_[0] = previousOutputLeft;
    previousOutput_[1] = previousOutputRight;
    float target = 1.0f;
    if (peak > 0.8f) target = 0.8f / peak;
    const float meanSquare = power / (frames * 2);
    if (meanSquare > 0.35f * 0.35f)
      target = std::min(target, 0.35f / std::sqrt(meanSquare));
    // Instant attack protects this already-rendered block; 250 ms release
    // avoids repeatedly raising and dropping the gain on dense passages.
    gain_ = std::min(target, gain_ + frames * releasePerFrame_);
    for (int i = 0; i < frames * 2; ++i) {
      const float sample = mix[i] * gain_;
      output[i] = sample >= 1.0f ? 32767 : sample <= -1.0f ? -32768 :
        static_cast<int16_t>(sample * 32767.0f);
    }
  }

 private:
  // Keep overload ratios intact so the block limiter can attenuate them
  // cleanly. The generous bound only prevents finite garbage from overflowing
  // the power accumulator; speaker protection is applied after analysis.
  static float sanitize(float sample) {
    if (!std::isfinite(sample)) return 0.0f;
    return std::clamp(sample, -16.0f, 16.0f);
  }

  float gain_ = 1.0f;
  float dcDecay_ = 0.0f;
  float releasePerFrame_ = 0.0f;
  float previousInput_[2] = {};
  float previousOutput_[2] = {};
};

#endif
