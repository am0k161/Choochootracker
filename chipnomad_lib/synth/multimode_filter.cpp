#include "multimode_filter.h"
#include "audio_math.h"

#include <math.h>
#include <string.h>

static constexpr float kPi = 3.14159265358979323846f;
static constexpr float kMinCutoff = FILTER_CUTOFF_MIN_HZ;
static constexpr float kMaxCutoff = FILTER_CUTOFF_MAX_HZ;

static float clampCutoff(float cutoff, float sampleRate) {
  if (!(cutoff >= kMinCutoff)) cutoff = kMinCutoff;
  if (cutoff > kMaxCutoff) cutoff = kMaxCutoff;
  if (cutoff > sampleRate * 0.45f) cutoff = sampleRate * 0.45f;
  return cutoff;
}

static void coefficients(float cutoff, float sampleRate, float damping, float* out) {
  const float g = tanf(kPi * clampCutoff(cutoff, sampleRate) / sampleRate);
  out[0] = 1.0f / (1.0f + g * (g + damping));
  out[1] = g * out[0];
  out[2] = g * out[1];
}

void MultimodeFilter::init(float sampleRate) {
  sampleRate_ = sampleRate > 0.0f ? sampleRate : 48000.0f;
  feedbackDecay_ = expf(-1.0f / (0.02f * sampleRate_));
  // Open303's coefficient approximation is specified through fs/8. Two
  // internal steps reach 12 kHz at a 48 kHz host rate.
  for (auto& filter : acid_) filter.setSampleRate(sampleRate_ * 2.0f);
  character_ = 255;
  acidMode_ = -1;
  acidCutoff_ = acidResonance_ = -1.0f;
  configure(false, 0, 0, false, kMaxCutoff, 0.0f);
}

void MultimodeFilter::configure(bool enabled, uint8_t character, uint8_t mode,
                                bool slope24dB, float cutoffHz, float resonance,
                                uint8_t color) {
  // Projects saved while Prism existed keep their Color setting and play as Snarl.
  const uint8_t nextCharacter = character == 7 ? 6 : character > 7 ? 1 : character;
  const bool characterChanged = nextCharacter != character_;
  if (characterChanged) reset();
  enabled_ = enabled;
  character_ = nextCharacter;
  mode_ = mode > 2 ? 0 : mode;
  slope24dB_ = slope24dB;
  color_ = color;
  cutoffHz_ = clampCutoff(cutoffHz, sampleRate_);
  resonance_ = fmaxf(0.0f, fminf(1.0f, resonance));

  if (character_ == 3) {
    const float g = tanf(kPi * cutoffHz_ / sampleRate_);
    k35G_ = g / (1.0f + g);
    k35K_ = 1.99f * resonance_ * resonance_;
  } else if (character_ == 4) {
    const int selected = mode_ == 0
      ? (slope24dB_ ? rosic::TeeBeeFilter::TB_303 : rosic::TeeBeeFilter::LP_12)
      : mode_ == 1
        ? (slope24dB_ ? rosic::TeeBeeFilter::BP_12_12 : rosic::TeeBeeFilter::BP_6_6)
        : (slope24dB_ ? rosic::TeeBeeFilter::HP_24 : rosic::TeeBeeFilter::HP_12);
    if (selected != acidMode_) {
      for (auto& filter : acid_) filter.setMode(selected);
      acidMode_ = selected;
    }
    const float acidCutoff = fminf(cutoffHz_, fminf(12000.0f, sampleRate_ * 0.25f));
    if (acidCutoff != acidCutoff_) {
      for (auto& filter : acid_) filter.setCutoff(acidCutoff);
      acidCutoff_ = acidCutoff;
    }
    const float percent = resonance_ * 95.0f;
    if (percent != acidResonance_) {
      for (auto& filter : acid_) filter.setResonance(percent);
      acidResonance_ = percent;
    }
  } else {
    // Preserve the previous Clean/Classic resonance taper.
    const float r2 = resonance_ * resonance_;
    const float r4 = r2 * r2;
    damping_ = character_ <= 2
      ? 1.0f / (0.5f + (character_ == 2 ? r4 * 0.7f : r4) * 19.5f)
      : 1.5f - 1.25f * r2;
    coefficients(cutoffHz_, sampleRate_, damping_, coeff_[0]);
    if (character_ >= 5) {
      const float onePoleTangent = tanf(kPi * cutoffHz_ / sampleRate_);
      onePoleG_ = onePoleTangent / (1.0f + onePoleTangent);
      // 00 = 0%, 80 = 150%, FF = 300%; the upper half adds the wild range.
      const float normal = fminf(color_ / 128.0f, 1.0f);
      const float extension = fmaxf((int(color_) - 128) * (2.0f / 127.0f), 0.0f);
      if (character_ == 6) {
        snarlTargetDelay_ = fmaxf(8.0f, fminf(240.0f,
          sampleRate_ / (cutoffHz_ * (0.35f + 0.2f * normal + 0.1f * extension))));
        if (characterChanged)
          snarlDelaySamples_[0] = snarlDelaySamples_[1] = snarlTargetDelay_;
      }
      if (character_ == 5) {
        innerDamping_ = fmaxf(0.12f,
          0.55f + 0.35f * (1.0f - resonance_) - 0.35f * extension);
        coefficients(cutoffHz_ * (0.25f + 0.15f * normal + 0.15f * extension),
                     sampleRate_, innerDamping_, coeff_[1]);
      }
    }
  }
}

void MultimodeFilter::reset() {
  memset(ic1eq_, 0, sizeof(ic1eq_));
  memset(ic2eq_, 0, sizeof(ic2eq_));
  memset(feedback_, 0, sizeof(feedback_));
  memset(feedbackEnvelope_, 0, sizeof(feedbackEnvelope_));
  memset(onePoleState_, 0, sizeof(onePoleState_));
  memset(snarlDelay_, 0, sizeof(snarlDelay_));
  memset(snarlDelaySamples_, 0, sizeof(snarlDelaySamples_));
  memset(snarlWrite_, 0, sizeof(snarlWrite_));
  if (character_ == 6)
    snarlDelaySamples_[0] = snarlDelaySamples_[1] = snarlTargetDelay_;
  for (auto& channel : k35_)
    for (auto& cell : channel) k35_reset(&cell);
  for (auto& filter : acid_) filter.reset();
}

float MultimodeFilter::processStage(float input, int channel, int stage,
                                    const float* a, float damping, uint8_t mode) {
  const float v3 = input - ic2eq_[channel][stage];
  const float band = a[0] * ic1eq_[channel][stage] + a[1] * v3;
  const float low = ic2eq_[channel][stage] + a[1] * ic1eq_[channel][stage] + a[2] * v3;
  const float high = input - damping * band - low;
  ic1eq_[channel][stage] = 2.0f * band - ic1eq_[channel][stage];
  ic2eq_[channel][stage] = 2.0f * low - ic2eq_[channel][stage];
  return mode == 1 ? band : mode == 2 ? high : low;
}

float MultimodeFilter::processAggro(float input, int channel) {
  float output = audioTanh(input * 1.4f);
  const int cells = slope24dB_ ? 2 : 1;
  if (mode_ == 1) {
    for (int i = 0; i < cells; ++i)
      output = k35_hpf_tick(&k35_[channel][i], output, k35G_, k35K_);
    for (int i = 0; i < cells; ++i)
      output = k35_lpf_tick(&k35_[channel][i + 2], output, k35G_, k35K_);
  } else {
    for (int i = 0; i < cells; ++i) {
      k35_t* cell = &k35_[channel][i];
      output = mode_ == 2 ? k35_hpf_tick(cell, output, k35G_, k35K_)
                          : k35_lpf_tick(cell, output, k35G_, k35K_);
    }
  }
  return audioTanh(output * 1.2f);
}

float MultimodeFilter::processCreative(float input, int channel) {
  const float normal = fminf(color_ / 128.0f, 1.0f);
  const float extension = fmaxf((int(color_) - 128) * (2.0f / 127.0f), 0.0f);
  const float previous = feedback_[channel];
  float driven;
  if (character_ == 5) {
    // Roar: a gated, filtered feedback loop driven harder by Color.
    feedbackEnvelope_[channel] = fmaxf(fabsf(input),
                                      feedbackEnvelope_[channel] * feedbackDecay_);
    driven = audioTanh(input * (1.05f + 0.45f * normal + 2.0f * extension) +
                       previous * (0.1f + normal + 2.0f * extension));
  } else {
    driven = audioTanh(input * 0.85f);
  }
  float output = processStage(driven, channel, 0, coeff_[0], damping_, mode_);
  if (character_ == 6) {
    // Snarl: a short, saturated delay loop adds moving resonant teeth.
    float& delay = snarlDelaySamples_[channel];
    delay += 0.005f * (snarlTargetDelay_ - delay);
    const int whole = (int)delay;
    const float fraction = delay - whole;
    const int read = (snarlWrite_[channel] - whole) & (kSnarlDelaySize - 1);
    const int older = (read - 1) & (kSnarlDelaySize - 1);
    const float delayed = snarlDelay_[channel][read] * (1.0f - fraction) +
                          snarlDelay_[channel][older] * fraction;
    const float feedback = 0.3f + 0.15f * resonance_ +
                           0.15f * normal + 0.1f * extension;
    snarlDelay_[channel][snarlWrite_[channel]] =
      audioTanh(output * (1.3f + normal + extension) + delayed * feedback);
    snarlWrite_[channel] = (snarlWrite_[channel] + 1) & (kSnarlDelaySize - 1);
    output = output * 0.65f + delayed * (0.55f + 0.2f * normal + 0.1f * extension);
  }
  // A third pole makes LP/HP 18 dB/oct. BP has a steeper upper skirt.
  const float v = (output - onePoleState_[channel]) * onePoleG_;
  const float low = v + onePoleState_[channel];
  onePoleState_[channel] = low + v;
  output = mode_ == 2 ? output - low : low;
  if (character_ == 5) {
    const float feedbackHigh = processStage(output, channel, 1, coeff_[1],
                                            innerDamping_, 2);
    const float gate = fminf(1.0f, feedbackEnvelope_[channel] * 4.0f);
    feedback_[channel] = gate * (0.2f + 0.08f * normal + 0.05f * extension) *
                         audioTanh(feedbackHigh * (1.0f + normal + extension));
  }
  const float level = character_ == 5
    ? 1.0f / (1.0f + 0.45f * normal + 0.45f * extension)
    : 1.2f / (1.0f + 0.35f * normal + extension);
  return audioTanh(output * 1.1f) * level;
}

float MultimodeFilter::process(float input, int channel) {
  if (!enabled_ || character_ == 0) return input;
  if (channel < 0 || channel > 1) channel = 0;
  if (character_ == 3) return processAggro(input, channel);
  if (character_ == 4) {
    float output = 0.0f;
    for (int step = 0; step < 2; ++step)
      output += 0.5f * (float)acid_[channel].getSample(input);
    if (!isfinite(output)) { acid_[channel].reset(); return 0.0f; }
    return audioTanh(0.7f * output);
  }
  if (character_ >= 5) return processCreative(input, channel);
  // A resonant SVF stage peaks at roughly 1 / damping. Two stages can
  // multiply that gain again, otherwise a sustained note clips the output.
  const float resonanceGain = fminf(1.0f, damping_);
  float output = processStage(input, channel, 0, coeff_[0], damping_, mode_);
  output *= resonanceGain;
  if (slope24dB_)
    output = processStage(output, channel, 1, coeff_[0], damping_, mode_);
  if (slope24dB_) output *= resonanceGain;
  if (!isfinite(output)) { reset(); return 0.0f; }
  return output;
}

float filterCutoffFromControl(uint8_t value) {
  return kMinCutoff * powf(kMaxCutoff / kMinCutoff, value / 255.0f);
}

uint8_t filterControlFromCutoff(float cutoffHz) {
  if (cutoffHz <= kMinCutoff) return 0;
  if (cutoffHz >= kMaxCutoff) return 255;
  return (uint8_t)(255.0f * logf(cutoffHz / kMinCutoff) /
                   logf(kMaxCutoff / kMinCutoff) + 0.5f);
}
