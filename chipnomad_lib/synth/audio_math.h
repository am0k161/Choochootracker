#ifndef CHOOCHOO_AUDIO_MATH_H
#define CHOOCHOO_AUDIO_MATH_H

#include <math.h>

extern float audioTanhTable[1025];

inline float audioTanh(float x) {
#ifdef CCT_AUDIO_TANH_REFERENCE_PATH
  return tanhf(x);
#else
  if (x <= -8.0f) return -1.0f;
  if (x >= 8.0f) return 1.0f;
  const float position = (x + 8.0f) * 64.0f;
  const int index = static_cast<int>(position);
  const float fraction = position - index;
  return audioTanhTable[index] +
         (audioTanhTable[index + 1] - audioTanhTable[index]) * fraction;
#endif
}

// All first-party wavefolders use a period of four. In their normal bounded
// audio domain an integer truncation preserves fmodf's sign semantics without
// paying for the general-purpose libm remainder path.
inline float audioFmod4(float x) {
#ifdef CCT_AUDIO_FMOD_REFERENCE_PATH
  return fmodf(x, 4.0f);
#else
  if (!(x > -128.0f && x < 128.0f)) return fmodf(x, 4.0f);
  return x - static_cast<int>(x * 0.25f) * 4.0f;
#endif
}

#endif
