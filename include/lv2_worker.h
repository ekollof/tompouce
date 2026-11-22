#ifndef LV2_WORKER_H
#define LV2_WORKER_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* Confined LV2 plugin worker. Plugin code runs only in a child process with
   fixed limits; the engine never dlopen()s a plugin. Audio frames are bounded
   by LV2_WORKER_FRAMES and sent as fixed binary blocks over pipes. */
#define LV2_WORKER_FRAMES 512
#define LV2_WORKER_CHANNELS 2

typedef struct Lv2Worker Lv2Worker;

Lv2Worker *lv2_worker_start(const char *plugin_uri, double sample_rate,
    char *error, size_t error_size);
/* Process one bounded stereo block. Returns 1 on success. */
int lv2_worker_process(Lv2Worker *worker, const float *input, float *output,
    unsigned frames, char *error, size_t error_size);
/* Set a control port value in the worker (bounded index, no DSP side effects). */
int lv2_worker_set_control(Lv2Worker *worker, uint32_t port, float value,
    char *error, size_t error_size);
/* Stage one bounded 1..3 byte MIDI message (note on/off, CC, etc.) for the
   next audio block at frame 0; the plugin receives it through an atom MIDI
   input port. */
int lv2_worker_send_midi(Lv2Worker *worker, const unsigned char *bytes, int count,
    char *error, size_t error_size);
/* Stage a message at a frame offset inside the next audio block. Offsets at or
   beyond the processed block length are clamped to its last frame. A block
   carries at most LV2_WORKER_MIDI_EVENTS events; ordering is by offset and
   then insertion. */
#define LV2_WORKER_MIDI_EVENTS 16
int lv2_worker_send_midi_at(Lv2Worker *worker, uint32_t offset,
    const unsigned char *bytes, int count, char *error, size_t error_size);
void lv2_worker_stop(Lv2Worker *worker);
pid_t lv2_worker_pid(const Lv2Worker *worker);

#endif
