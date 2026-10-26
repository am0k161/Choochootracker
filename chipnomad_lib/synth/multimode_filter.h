#ifndef CHOOCHOO_MULTIMODE_FILTER_H
#define CHOOCHOO_MULTIMODE_FILTER_H

#include <stdint.h>
#include "external/aphex/k35.h"
#include "rosic_TeeBeeFilter.h"

constexpr int FILTER_CUTOFF_MIN_HZ = 20;
constexpr int FILTER_CUTOFF_MAX_HZ = 20000;

class MultimodeFilter {
 public:
  void init(float sampleRate);
  void configure(bool enabled, uint8_t character, uint8_t mode, bool slope24dB,
                 float cutoffHz, float resonance, uint8_t color = 128);
  void reset();
  float process(float input, int channel = 0);

 private:
  float processStage(float input, int channel, int stage, const float* coeff,
                     float damping, uint8_t mode);
  float processAggro(float input, int channel);
  float processCreative(float input, int channel);

  float sampleRate_ = 48000.0f;
  bool enabled_ = false;
  uint8_t character_ = 0, mode_ = 0, color_ = 128;
  bool slope24dB_ = false;
  float cutoffHz_ = 1000.0f, resonance_ = 0.0f;
  float damping_ = 1.0f, innerDamping_ = 1.0f;
  float coeff_[3][3] = {};
  float ic1eq_[2][3] = {};
  float ic2eq_[2][3] = {};
  float feedback_[2] = {};
  float k35G_ = 0.0f, k35K_ = 0.0f;
  k35_t k35_[2][4] = {};
  rosic::TeeBeeFilter acid_[2];
  int acidMode_ = -1;
  float acidCutoff_ = -1.0f, acidResonance_ = -1.0f;
};

float filterCutoffFromControl(uint8_t value);
uint8_t filterControlFromCutoff(float cutoffHz);

#endif
