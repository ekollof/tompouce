#ifndef NATIVE_SYNTH_H
#define NATIVE_SYNTH_H
#include <cjson/cJSON.h>
typedef struct NativeSynth NativeSynth;
typedef struct NativeSynthState NativeSynthState;
/* Preparation/creation/destruction are never called from the audio callback.
   Prepare returns one reference; queue/voice owners retain their own references.
   Unreferenced immutable programs can be evicted from the bounded cache. */
int native_synth_sound(const char *sound);
const NativeSynth *native_synth_prepare(const cJSON *value, double frequency, double duration);
void native_synth_retain(const NativeSynth *program);
void native_synth_release(const NativeSynth *program);
NativeSynthState *native_synth_state_new(const NativeSynth *program);
void native_synth_state_free(NativeSynthState *state);
int native_synth_effect(const NativeSynth *program);
int native_synth_stereo(const NativeSynth *program);
int native_synth_enveloped(const NativeSynth *program);
double native_synth_tail(const NativeSynth *program);
/* Callback safe: bounded native operations only; 0 marks the end of ZzFX. */
int native_synth_process(NativeSynthState *state, double age, double gate, double frequency,
                         double origin, double input_l, double input_r, double *left, double *right);
#endif
