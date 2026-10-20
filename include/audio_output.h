#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

typedef void (*AudioOutputRender)(float *output, unsigned long frames);
/* Foreground startup/shutdown; rendering stays on the chosen output thread. */
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

#endif
