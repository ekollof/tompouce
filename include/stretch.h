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
double stretch_age_offset(const StretchBuffer *buffer);
/* Generator-only immutable PCM sources share the bounded/refcounted cache.
   lookup returns an owned reference; store consumes interleaved stereo PCM. */
const StretchBuffer *stretch_pcm_lookup(const char *key);
const StretchBuffer *stretch_pcm_store(const char *key, float *pcm, int frames);
#endif
