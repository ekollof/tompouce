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
/* Live-streaming scalars for a stretched-sample voice, derived from the
   same event parsing as the baked key but without building PCM. step is
   the mapped playback step including the pitch ratio and Float32 cast,
   gate the envelope gate, begin the source begin fraction, origin the
   source_start timeline in seconds (musical onset minus 40 ms) and
   frames the total rendered sample count. Returns 0 when the event
   cannot prepare (mirroring the baked rejection). */
typedef struct { double step, gate, begin, origin; int frames; } StretchLiveKey;
int stretch_live_key(const AudioEvent *event, StretchLiveKey *out);
#endif
