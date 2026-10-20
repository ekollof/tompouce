#ifndef JS_AUDIO_H
#define JS_AUDIO_H
#include "javascript.h"
#include "event.h"
int js_audio_event(const cJSON *hap, double origin, double seconds, AudioEvent *event);
/* Per-entry duck mapping for reference tests: one orbit/control per target
   entry in order, mirroring the web controller's forEach index. Invalid
   targets are skipped. Returns the entry count, truncated to capacity. */
int js_audio_duck_entries(const cJSON *value, int *orbits, DuckControl *controls, int capacity);
/* Resolve source and publisher-side room state for offline tests. */
int js_audio_resolve_sample(AudioEvent *event, const cJSON *value);
/* Test/offline instrumentation: runs on the preparation worker before source
   resolution. Pass NULL to disable. */
typedef void (*JsAudioPrepObserver)(const AudioEvent *event);
void js_audio_set_prep_observer(JsAudioPrepObserver observer);
int js_audio_schedule(JsProgram *program, double cycle_start, int cycle_index, unsigned generation);
int js_audio_schedule_arc(JsProgram *program, double origin, double seconds,
                          double begin, double end, unsigned generation);
typedef struct {
    double seconds, anchor_cycle, anchor_time, end;
} JsAudioClock;
int js_audio_schedule_clock(JsProgram *program, JsAudioClock *clock,
                            double begin, double end, double tick_end, unsigned generation);
#endif
