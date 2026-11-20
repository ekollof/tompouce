#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

typedef void (*AudioOutputRender)(float *output, unsigned long frames);
/* Foreground startup/shutdown; rendering stays on the chosen output thread. */
/* Named output targets (exact PipeWire node names). Index 0 is the default
   stream; 1..AUDIO_OUTPUT_NAMED_MAX are lazily created named streams. */
#define AUDIO_OUTPUT_NAMED_MAX 4
/* Resolve a pattern .out(name) to a target index; unknown names return 0 and
   emit one console warning. Candidate names come from TOMPOUCE_OUTPUT_NAMES. */
int audio_output_target(const char *name);
const char *audio_output_target_name(int index);
int audio_output_named_count(void);

int audio_output_start(AudioOutputRender render, unsigned long frames);
void audio_output_stop(void);
int audio_output_active(void);
const char *audio_output_name(void);
/* Missed device cycles observed on the output thread. Only the PipeWire
   backend accounts process gaps; other backends leave the counters at zero
   (unmeasured, not zero underruns). Lock-free; reset while the output
   thread is idle or before the measured interval. */
void audio_output_xrun_stats(unsigned long long *calls, unsigned long long *gaps,
    unsigned long long *max_gap_ns, unsigned long long *empty);
void audio_output_xrun_reset(void);
/* Backend-specific missed-buffer count: PipeWire gaps, OpenAL queue empties,
   zero for backends that do not account for them. */
unsigned long long audio_output_underruns(void);

#ifdef TOMPOUCE_RENDER_PROFILE
/* Compile-time diagnostic: callback render duration histogram. */
void audio_render_profile_stats(unsigned long long *calls,
    unsigned long long *max_ns, unsigned long long *over_2ms,
    unsigned long long *over_3ms, unsigned long long *total_ns);
void audio_render_profile_reset(void);
#endif

#endif
