#ifndef LIVE_LV2_H
#define LIVE_LV2_H
#include <stddef.h>

/* Scheduler-owned live LV2 instruments. Each slot owns one confined worker,
   a bounded render thread and a bounded stereo sample ring. Note/CC events are
   queued by the scheduler and consumed by the render thread; the audio
   callback only reads the ring and never performs IPC or allocation. Slots are
   never reused, so a running callback cannot observe a reset ring. */
#define LIVE_LV2_SLOTS 8

/* Acquire an instrument for `uri`, starting a worker/thread off the audio
   callback. Returns a slot index, or -1 when the plugin has no MIDI input, is
   not an instrument, the registry is full or startup failed. */
int live_lv2_acquire(const char *uri, double sample_rate, double now,
    char *error, size_t error_size);
/* Queue a note-on at `at` seconds, CC 0..127 before it when `cc >= 0`, and a
   note-off at `at + duration`. Returns 0 when the bounded queue is full. */
int live_lv2_note(int slot, double at, double duration, int channel, int note,
    int velocity, int cc, int cc_value);
int live_lv2_cc(int slot, double at, int channel, int control, int value);
/* Queue a named LV2 control-port value at `at`; bounded metadata clamps it. */
int live_lv2_control(int slot, double at, const char *name, float value);
/* Queue an installed LV2 preset (URI or display label) at `at`. */
int live_lv2_preset(int slot, double at, const char *preset);
/* Queue an LV2 state file from the confined state root at `at`. */
int live_lv2_state(int slot, double at, const char *state);
/* Inspect the bounded control-port metadata copied at acquisition. */
int live_lv2_parameter_count(int slot);
int live_lv2_parameter_info(int slot, int index, char name[64], float *min,
    float *max, float *value);
/* Queue MIDI all-notes-off on every channel (scheduler thread only). */
void live_lv2_all_notes_off(void);
/* Add available stereo frames to left/right; missing frames are left
   untouched. Safe from the audio callback. */
void live_lv2_mix(float *left, float *right, unsigned frames);
/* Stop every worker/thread and release slots. Never call from the callback. */
void live_lv2_shutdown(void);
unsigned long long live_lv2_underruns(void);
/* Next audio time the render thread will produce, in seconds. */
double live_lv2_time(int slot);

#endif
