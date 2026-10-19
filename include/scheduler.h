#ifndef SCHEDULER_H
#define SCHEDULER_H
#include "common.h"
#include "event.h"
#include "track.h"

void scheduler_init(void);
void scheduler_shutdown(void);
void event_queue_clear(void);
void event_queue_new_program(double boundary);
/* Generator-only transaction: pushes are staged until finish. Existing notes
   keep playing during preparation; finish atomically replaces future events. */
void event_queue_refresh(double cutoff);
int event_queue_refresh_finish(int success);
/* Normal publication transaction: returns whether this thread owns it. */
int event_queue_publish_begin(void);
int event_queue_publish_finish(int owned, int success);
double event_queue_not_before(void);
void event_queue_push(AudioEvent *ev);
int event_queue_depth(void);
/* Read-only text of the next queued events. limit is clamped 1..48.
   Returns bytes written, excluding the NUL, or -1. */
int scheduler_debug_queue(char *out, size_t outlen, int limit);
unsigned event_queue_generation(void);
/* background threads started by init */

void scheduler_start(void);
/* Stop resets the query clock; already committed notes and tails finish.
   Starting an already running transport is idempotent. */
void scheduler_transport_set(int playing);
void scheduler_stop(void);
extern volatile int scheduler_running;
extern double scheduler_start_time;

#define NOTE_HIST_N 256
typedef struct {
    double t0;    /* trigger time (audio clock) */
    double dur;   /* scheduled duration */
    int midi;     /* display pitch 0-127 (drums mapped to GM) */
    int is_drum;
    unsigned scope_mask, generation;
} NoteHist;

/* recent triggered notes, oldest first; returns count (<= max) */
int note_hist_copy(NoteHist *out, int max);
/* Growable song list. SONG_TRACK_CEILING is a memory guard, not a
   musical bar limit. Each snapshot is an exact-size heap string. */
#define SONG_TRACK_CEILING 8192

/* Song list: each track holds an entire code buffer (all voices).
   Live mode loops the editor buffer; song mode plays one track
   per cycle, starting at 0. Hold suppresses auto-advance. */
void scheduler_song_set(int on);   /* on restarts at track 0 */
void scheduler_song_toggle(void);
int scheduler_song_on(void);
void scheduler_song_hold_set(int on);
void scheduler_song_hold_toggle(void);
int scheduler_song_hold(void);
int scheduler_song_pos(void);      /* active index, -1 when list empty */
int scheduler_song_index(void);    /* active index, -1 when list empty */
int scheduler_song_count(void);
/* snapshot code into a new track after the current position; the new
   track becomes active. Returns index or -1. */
int scheduler_track_copy(const char *code);
/* go to track (loads + evaluates); manual navigation */
void scheduler_track_goto(int i);
void scheduler_track_next(void);
void scheduler_track_prev(void);
/* move track i to position j (clamped); remove track i. 1 on success. */
int scheduler_track_move(int i, int j);
int scheduler_track_remove(int i);/* song advance step (called by generator when a cycle finishes) */
void scheduler_song_step(void);
/* copy of track i's code into buf (1 on success). Truncates to buflen. */
int scheduler_track_code(int i, char *buf, int buflen);
/* malloc'd copy of track i, or NULL. Caller frees. */
char *scheduler_track_dup(int i);
/* replace the whole song list (grows until SONG_TRACK_CEILING) and load
   the active buffer into the engine; active clamped into range. n<=0 clears. */
void scheduler_song_replace(const char **codes, int n, int active);
/* overwrite track i's code (F5 commit); silent no-op when out of range */
void scheduler_track_update(int i, const char *code);
/* first content line of track i into buf (1 on success) */
int scheduler_track_preview(int i, char *buf, int buflen);
/* reset cycle tracking after any fresh compile */
void scheduler_song_sync(void);
/* editor-follow request for the next position change (MCP actions) */
void scheduler_song_follow(void);
int scheduler_song_follow_consume(void);
/* Byte spans in current_code that are sounding at audio time `now`.
   Each span is half-open [off, off+len). Returns how many were written. */
int scheduler_play_spans(double now, int *off, int *len, int max);
int scheduler_play_styles(double now, int *off, int *len, uint32_t *color, unsigned *flags, int max);
/* Translate cached JS spans after visual-only editor changes. */
int scheduler_editor_play_styles(double now, const char *editor, int *off, int *len, uint32_t *color, unsigned *flags, int max);

#endif
