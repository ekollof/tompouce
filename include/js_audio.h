#ifndef JS_AUDIO_H
#define JS_AUDIO_H
#include "javascript.h"
#include "event.h"
int js_audio_event(const cJSON *hap, double origin, double seconds, AudioEvent *event);
/* Resolve source and publisher-side room state for offline tests. */
int js_audio_resolve_sample(AudioEvent *event, const cJSON *value);
int js_audio_schedule(JsProgram *program, double cycle_start, int cycle_index, unsigned generation);
int js_audio_schedule_arc(JsProgram *program, double origin, double seconds,
                          double begin, double end, unsigned generation);
typedef struct {
    double seconds, anchor_cycle, anchor_time, end;
} JsAudioClock;
int js_audio_schedule_clock(JsProgram *program, JsAudioClock *clock,
                            double begin, double end, double tick_end, unsigned generation);
#endif
