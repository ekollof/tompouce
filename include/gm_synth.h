#ifndef GM_SYNTH_H
#define GM_SYNTH_H
#include "event.h"
int gm_synth_count(void);
const char *gm_synth_name(int index);
int gm_synth_program(const char *name);
int gm_synth_available(void);
/* Generator only. Assigns owned immutable PCM; audio never calls FluidSynth. */
int gm_synth_prepare(AudioEvent *event);
#endif
