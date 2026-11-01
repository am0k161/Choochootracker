#include "achchid_voice.h"
#include "audio_math.h"
#include "braids_sample_rate.h"
#include <math.h>
#include <string.h>

#ifdef CCT_DISABLE_CONFIG_CACHE
static constexpr bool kCacheConfigurations = false;
#else
static constexpr bool kCacheConfigurations = true;
#endif

static float achchidTanh(float value) {
#ifdef CCT_ACHCHID_REFERENCE_SHAPER
  return tanhf(value);
#else
  return audioTanh(value);
#endif
}

void AChChidVoice::init(float sampleRate) {
  synth_.setSampleRate(sampleRate);
  braidsPitchOffset_ = braidsNativePitchOffset(sampleRate);
  braids_.Init(); braids_.set_shape(braids::MACRO_OSC_SHAPE_CSAW);
  braids_.set_parameters(16384, 16384);
  sid_.init(sampleRate); sidPitchSlew_.setSampleRate(sampleRate);
  openWaveform_ = model_ = timbre_ = color_ = -1;
  sidShape_ = sidPulseWidth_ = -1;
  cutoff_ = resonance_ = envMod_ = decay_ = accent_ = -1;
}
void AChChidVoice::configure(uint8_t wave, int16_t fine, uint8_t model, uint16_t timbre, uint16_t color, uint8_t saturation,
                              uint8_t sidShape, uint16_t sidPulseWidth,
                              uint16_t cutoff, uint8_t resonance, uint8_t envMod, uint16_t decay,
                              uint8_t accent, float gain) {
  wave_ = wave > 3 ? 1 : wave; gain_ = gain;
  const int nextOpenWaveform = wave_ == 0 ? 1 : 0;
  if (!kCacheConfigurations || openWaveform_ != nextOpenWaveform) {
    synth_.setWaveform(nextOpenWaveform);
    openWaveform_ = nextOpenWaveform;
  }
  if (!kCacheConfigurations || cutoff_ != cutoff) { synth_.setCutoff(cutoff); cutoff_ = cutoff; }
  if (!kCacheConfigurations || resonance_ != resonance) { synth_.setResonance(resonance); resonance_ = resonance; }
  if (!kCacheConfigurations || envMod_ != envMod) { synth_.setEnvMod(envMod); envMod_ = envMod; }
  if (!kCacheConfigurations || decay_ != decay) { synth_.setDecay(decay); decay_ = decay; }
  if (!kCacheConfigurations || accent_ != accent) { synth_.setAccent(accent); accent_ = accent; }
  const int nextModel = model > 46 ? 46 : model;
  if (!kCacheConfigurations || model_ != nextModel) {
    braids_.set_shape(static_cast<braids::MacroOscillatorShape>(nextModel));
    model_ = nextModel;
  }
  const int nextTimbre = timbre > 32767 ? 32767 : timbre;
  const int nextColor = color > 32767 ? 32767 : color;
  if (!kCacheConfigurations || timbre_ != nextTimbre || color_ != nextColor) {
    braids_.set_parameters(nextTimbre, nextColor);
    timbre_ = nextTimbre;
    color_ = nextColor;
  }
  if (!kCacheConfigurations || sidShape_ != sidShape || sidPulseWidth_ != sidPulseWidth) {
    sid_.configure(sidShape, sidPulseWidth);
    sidShape_ = sidShape;
    sidPulseWidth_ = sidPulseWidth;
  }
  fine_ = fine; saturation_ = saturation;
}
void AChChidVoice::noteOn(int note, bool accent, bool slide, uint8_t slideValue) {
  // Open303 scales its public setter by 0.2; compensate so ASL 00 is 60 ms.
  synth_.setSlideTime(5.0 * (60.0 + slideValue * 4.0));
  // Align Braids' pitch table with Open303 at the host rate.
  pitch_ = (note + 12) * 128 + fine_ * 128 / 100 + braidsPitchOffset_;
  synth_.setTuning(440.0 * exp2(fine_ / 1200.0));
  sidPitchTarget_ = 440.0 * exp2((note + 12 + fine_ / 100.0 - 69) / 12.0);
  sidPitchSlew_.setTimeConstant(60.0 + slideValue * 4.0);
  if (!slide || !active_) sidPitchSlew_.setState(sidPitchTarget_);
  sid_.noteOn(slide && active_);
  if (slide && active_) synth_.slide(note + 12, accent); else synth_.trigger(note + 12, accent);
  braids_.Strike(); active_ = true;
}
void AChChidVoice::noteOff() { synth_.release(); }
void AChChidVoice::kill() { synth_.allNotesOff(); active_ = false; }
void AChChidVoice::renderBraidsBlock() {
  memset(braidsSync_, 0, sizeof(braidsSync_)); braids_.set_pitch(pitch_);
  braids_.Render(braidsSync_, braidsBlock_, 24); braidsPosition_ = 0;
}
void AChChidVoice::render(float* output, int samples) {
  const float amount = saturation_ / 255.0f;
  const float drive = 1.0f + amount * 5.0f;
#ifdef CCT_ACHCHID_REFERENCE_SHAPER
  const bool fold = true;
  const float foldMix = std::max(0.0f, std::min(1.0f, (amount - .55f) / .45f));
#else
  const bool fold = saturation_ > 140;
  const float foldMix = fold ? (amount - .55f) / .45f : 0.0f;
#endif
  for (int i = 0; i < samples; ++i) {
    float sample;
    if (wave_ >= 2) {
      float oscillatorSample;
      if (wave_ == 2) { if (braidsPosition_ == 24) renderBraidsBlock();
        oscillatorSample = braidsBlock_[braidsPosition_++] / 32768.0f;
      } else oscillatorSample = sid_.sample(sidPitchSlew_.getSample(sidPitchTarget_));
      // MME's shaper gives a clear soft-clip-to-wavefold range before the
      // Open303 filter, with no static offset for its highpass to remove.
      float saturated = achchidTanh(oscillatorSample * drive);
      if (fold) {
        const float folded = fabsf(audioFmod4(saturated * (1.0f + amount * 3.0f) + 1.0f) - 2.0f) - 1.0f;
        saturated += (folded - saturated) * foldMix;
      }
      oscillatorSample = saturated;
      sample = synth_.getSampleWithExternalInput(oscillatorSample);
    } else sample = synth_.getSample();
    // Continuous soft limiting prevents Open303 resonance/accent peaks from
    // overdriving the shared mixer without a discontinuous clip threshold.
    output[i] = 0.85f * achchidTanh(sample * gain_ / 0.85f);
  }
}
