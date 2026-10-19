#ifndef TRACK_H
#define TRACK_H
#include "common.h"
#include "event.h"
#include "audio.h"
#include "javascript.h"

typedef struct Track {
    JsProgram *javascript;
    int visual_requests;
    unsigned compile_generation;
    unsigned slider_revision;
    unsigned transport_revision;
    double js_origin, js_seconds, js_cursor, js_tick, js_anchor_cycle, js_anchor_time, js_phase;
    char keyword[16];
    int mute, solo, active;
    double next_cycle;
    int cycle_index;
} Track;

/* 1 accepted; 0 evaluation failed (the last playable pattern is retained). */
int track_load_from_code(const char *code_buf);

/* arrays */
extern Track tracks[MAX_TRACKS];
extern int track_count;
extern pthread_mutex_t tracks_lock;
extern char *current_code;
/* Copy s into current_code. 0 if malloc fails or s does not fit in an int
   offset. The previous text is left unchanged on failure. */
int current_code_set(const char *s);

#endif
