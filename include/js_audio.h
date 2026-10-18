#ifndef JS_AUDIO_H
#define JS_AUDIO_H
#include "javascript.h"
#include "event.h"
int js_audio_event(const cJSON *hap, double origin, double seconds, AudioEvent *event);
void js_audio_schedule(JsProgram *program, double cycle_start, int cycle_index, unsigned generation);
#endif
