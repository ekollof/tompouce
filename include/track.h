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
    JsProgram *pending;          /* staged program, owned (NEW, collab) */
    cJSON *pending_metadata;     /* staged metadata, owned (NEW, collab) */
    double pending_cycle;        /* room-cycle activation point (NEW, collab) */
    unsigned pending_seq;        /* author eval order (NEW, collab) */
    int collab_clock;            /* anchors come from the room clock (NEW) */
} Track;

/* 1 accepted; 0 evaluation failed (the last playable pattern is retained). */
int track_load_from_code(const char *code_buf);
/* Shared activation: swap in program/metadata at the given cursor. The
   caller must hold tracks_lock; preparation must have happened off the
   audio callback. Ownership of program/metadata transfers here.
   keep_state preserves local monitor/transport state for room activations;
   the immediate path passes 0 and resets exactly as before. */
void track_activate_locked(JsProgram *program, cJSON *metadata,
    double music_cursor, double boundary, double tick, int keep_state);
/* Stage an already compiled program for activation at activate_cycle.
   Preparation (compile, csound, samples) must happen before this call, never
   in the audio callback. Supersedes any earlier staged program. Returns 1
   staged (ownership transfers); 0 when there is no live track, in which
   case the caller keeps program/metadata. */
int track_stage_program(JsProgram *program, cJSON *metadata,
    double activate_cycle, unsigned eval_seq);
/* Drop any staged program without activating it. */
void track_clear_pending(void);

/* arrays */
extern Track tracks[MAX_TRACKS];
extern int track_count;
extern pthread_mutex_t tracks_lock;
extern char *current_code;
/* Copy s into current_code. 0 if malloc fails or s does not fit in an int
   offset. The previous text is left unchanged on failure. */
int current_code_set(const char *s);

#endif
