#ifndef AUDIO_OUTPUT_H
#define AUDIO_OUTPUT_H

typedef void (*AudioOutputRender)(float *output, unsigned long frames);
/* Foreground startup/shutdown; rendering stays on the chosen output thread. */
int audio_output_start(AudioOutputRender render, unsigned long frames);
void audio_output_stop(void);
int audio_output_active(void);
const char *audio_output_name(void);

#endif
