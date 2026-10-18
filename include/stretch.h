#ifndef STRETCH_H
#define STRETCH_H
#include "event.h"
/* Generator-only preparation; returned immutable PCM has one owned reference.
   Queue and voices retain it. No FFT, allocation or cache access in audio. */
const StretchBuffer *stretch_prepare(const AudioEvent *event);
void stretch_retain(const StretchBuffer *buffer);
void stretch_release(const StretchBuffer *buffer);
int stretch_frames(const StretchBuffer *buffer);
const float *stretch_pcm(const StretchBuffer *buffer);
#endif
