#ifndef MOBILE_GROOVE_ACHCHID_VOICE_H
#define MOBILE_GROOVE_ACHCHID_VOICE_H

#include <stdint.h>
#include "braids/macro_oscillator.h"
#include "rosic_Open303.h"
#include "sid_voice.h"

class AChChidVoice {
 public:
  void init(float sampleRate);
  void configure(uint8_t wave, int16_t fine, uint8_t model, uint16_t timbre, uint16_t color, uint8_t saturation,
                 uint8_t sidShape, uint16_t sidPulseWidth,
                 uint16_t cutoff, uint8_t resonance, uint8_t envMod, uint16_t decay,
                 uint8_t accent, float gain);
  void noteOn(int note, bool accent, bool slide, uint8_t slideValue);
  void noteOff();
  void kill();
  void render(float* output, int samples);
  bool active() const { return active_; }
  float envelopeLevel() const { return active_ ? 1.0f : 0.0f; }
 private:
  rosic::Open303 synth_;
  braids::MacroOscillator braids_;
  SIDOscillator sid_;
  rosic::LeakyIntegrator sidPitchSlew_;
  double sidPitchTarget_ = 440.0;
  uint8_t wave_ = 1;
  int openWaveform_ = -1;
  int model_ = -1, timbre_ = -1, color_ = -1;
  int sidShape_ = -1, sidPulseWidth_ = -1;
  int cutoff_ = -1, resonance_ = -1, envMod_ = -1, decay_ = -1, accent_ = -1;
  int16_t braidsBlock_[24] = {};
  uint8_t braidsSync_[24] = {};
  int braidsPosition_ = 24;
  int16_t pitch_ = 60 << 7;
  int16_t braidsPitchOffset_ = 0;
  int16_t fine_ = 0;
  uint8_t saturation_ = 0;
  float gain_ = 1.0f;
  bool active_ = false;
  void renderBraidsBlock();
};
#endif
