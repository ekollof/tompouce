#include "scheduler.h"
#include "slider.h"
#include "js_audio.h"
#include "track.h"
#include "audio.h"
#include "stretch.h"
#include "common.h"
#include "midi.h"
#include "prep_pool.h"

#include <pthread.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

volatile int scheduler_running = 0;
double scheduler_start_time = 0.0;

/* song list: heap strings, grown as tracks are copied */
typedef struct { char *code; } SongTrack;
static SongTrack *song_tracks = NULL;
static int song_cap = 0;
static int song_ntracks = 0;
static int song_pos = -1;
static volatile int song_on = 0;
static volatile int song_hold = 0;
static double song_advance_at = 0.0; /* audio time of next cycle boundary */
/* set when an MCP action wants the editor to follow a position change */
static volatile int song_follow = 0;

void scheduler_song_follow(void) { song_follow = 1; }
int scheduler_song_follow_consume(void) {
    if (!song_follow) return 0;
    song_follow = 0;
    return 1;
}

void scheduler_song_set(int on) {
    pthread_mutex_lock(&tracks_lock);
    on = on ? 1 : 0;
    if (on && song_ntracks > 0) {
        song_on = 1;
        song_pos = 0;
        pthread_mutex_unlock(&tracks_lock);
        scheduler_track_goto(0);
        return;
    }
    song_on = on && song_ntracks > 0 ? 1 : 0;
    pthread_mutex_unlock(&tracks_lock);
}
void scheduler_song_toggle(void) {
    scheduler_song_set(!scheduler_song_on());
}
int scheduler_song_on(void) { return song_on; }
void scheduler_song_hold_set(int on) {
    song_hold = on ? 1 : 0;
}
void scheduler_song_hold_toggle(void) {
    scheduler_song_hold_set(!scheduler_song_hold());
}
int scheduler_song_hold(void) { return song_hold; }
int scheduler_song_pos(void) {
    int p;
    pthread_mutex_lock(&tracks_lock);
    p = (song_ntracks > 0 && song_pos >= 0) ? song_pos : -1;
    pthread_mutex_unlock(&tracks_lock);
    return p;
}
int scheduler_song_index(void) { return scheduler_song_pos(); }
int scheduler_song_count(void) {
    int n;
    pthread_mutex_lock(&tracks_lock);
    n = song_ntracks;
    pthread_mutex_unlock(&tracks_lock);
    return n;
}

static int code_blank(const char *code) {
    if (!code) return 1;
    while (*code && isspace((unsigned char)*code)) code++;
    return *code == 0;
}

static char *song_dup(const char *s) {
    if (!s) s = "";
    size_t n = strlen(s);
    if (n > (size_t)INT_MAX) return NULL;
    char *d = malloc(n + 1);
    if (!d) return NULL;
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

static int song_ensure(int need) {
    if (need <= song_cap) return 1;
    if (need > SONG_TRACK_CEILING) return 0;
    int cap = song_cap ? song_cap : 8;
    while (cap < need) {
        if (cap > SONG_TRACK_CEILING / 2) { cap = need; break; }
        cap *= 2;
    }
    if (cap > SONG_TRACK_CEILING) cap = SONG_TRACK_CEILING;
    if (cap < need) return 0;
    SongTrack *np = realloc(song_tracks, (size_t)cap * sizeof(SongTrack));
    if (!np) return 0;
    for (int i = song_cap; i < cap; i++) np[i].code = NULL;
    song_tracks = np;
    song_cap = cap;
    return 1;
}

static void song_clear_all(void) {
    for (int i = 0; i < song_ntracks; i++) {
        free(song_tracks[i].code);
        song_tracks[i].code = NULL;
    }
    song_ntracks = 0;
    song_pos = -1;
}

int scheduler_track_copy(const char *code) {
    if (code_blank(code)) {
        log_console("nothing to copy");
        return -1;
    }
    int idx = -1;
    pthread_mutex_lock(&tracks_lock);
    if (!song_ensure(song_ntracks + 1)) {
        pthread_mutex_unlock(&tracks_lock);
        log_console("track list full (%d)", SONG_TRACK_CEILING);
        return -1;
    }
    int at = (song_pos >= 0 && song_pos < song_ntracks) ? song_pos + 1 : song_ntracks;
    if (at > song_ntracks) at = song_ntracks;
    if (at < song_ntracks)
        memmove(&song_tracks[at + 1], &song_tracks[at],
                (size_t)(song_ntracks - at) * sizeof(SongTrack));
    song_tracks[at].code = song_dup(code);
    if (!song_tracks[at].code) {
        if (at < song_ntracks)
            memmove(&song_tracks[at], &song_tracks[at + 1],
                    (size_t)(song_ntracks - at) * sizeof(SongTrack));
        pthread_mutex_unlock(&tracks_lock);
        log_console("track copy failed");
        return -1;
    }
    song_ntracks++;
    song_pos = at;
    idx = at;
    char *tmp = song_dup(song_tracks[at].code);
    int shown = song_ntracks;
    pthread_mutex_unlock(&tracks_lock);
    if (tmp) { track_load_from_code(tmp); free(tmp); }
    log_console("copied to track %d/%d", idx + 1, shown);
    return idx;
}

int scheduler_track_code(int i, char *buf, int buflen) {
    if (!buf || buflen <= 0) return 0;
    buf[0] = 0;
    pthread_mutex_lock(&tracks_lock);
    int ok = (i >= 0 && i < song_ntracks && song_tracks[i].code);
    if (ok) snprintf(buf, (size_t)buflen, "%s", song_tracks[i].code);
    pthread_mutex_unlock(&tracks_lock);
    return ok;
}

char *scheduler_track_dup(int i) {
    char *d = NULL;
    pthread_mutex_lock(&tracks_lock);
    if (i >= 0 && i < song_ntracks && song_tracks[i].code)
        d = song_dup(song_tracks[i].code);
    pthread_mutex_unlock(&tracks_lock);
    return d;
}

void scheduler_track_update(int i, const char *code) {
    if (!code) return;
    pthread_mutex_lock(&tracks_lock);
    if (i >= 0 && i < song_ntracks) {
        char *d = song_dup(code);
        if (d) {
            free(song_tracks[i].code);
            song_tracks[i].code = d;
        }
    }
    pthread_mutex_unlock(&tracks_lock);
}

void scheduler_song_replace(const char **codes, int n, int active) {
    char *tmp = NULL;
    pthread_mutex_lock(&tracks_lock);
    song_clear_all();
    if (codes && n > 0) {
        if (n > SONG_TRACK_CEILING) n = SONG_TRACK_CEILING;
        if (!song_ensure(n)) n = song_cap;
        int got = 0;
        for (int i = 0; i < n; i++) {
            char *d = song_dup(codes[i] ? codes[i] : "");
            if (!d) break;
            song_tracks[got].code = d;
            got++;
        }
        song_ntracks = got;
        if (got > 0) {
            if (active < 0) active = 0;
            if (active >= got) active = got - 1;
            song_pos = active;
            tmp = song_dup(song_tracks[active].code);
        } else song_on = 0;
    } else {
        song_on = 0;
    }
    pthread_mutex_unlock(&tracks_lock);
    if (tmp) { track_load_from_code(tmp); free(tmp); }
    else event_queue_clear();
}

/* first content line (skips blanks, comments, bpm) for pane display */
int scheduler_track_preview(int i, char *buf, int buflen) {
    if (!buf || buflen <= 0) return 0;
    buf[0] = 0;
    pthread_mutex_lock(&tracks_lock);
    int ok = (i >= 0 && i < song_ntracks);
    if (ok) {
        const char *p = song_tracks[i].code;
        char line[256];
        while (*p) {
            int k = 0;
            while (*p && *p != '\n' && k < 255) line[k++] = *p++;
            line[k] = 0;
            if (*p == '\n') p++;
            char *t = line;
            while (*t == ' ' || *t == '\t') t++;
            if (!*t || *t == '#') continue;
            if (strncmp(t, "bpm ", 4) == 0 || strncmp(t, "bpm\t", 4) == 0) continue;
            snprintf(buf, (size_t)buflen, "%s", t);
            break;
        }
        if (!buf[0]) snprintf(buf, (size_t)buflen, "%s", "(empty)");
    }
    pthread_mutex_unlock(&tracks_lock);
    return ok;
}

void scheduler_track_goto(int i) {
    pthread_mutex_lock(&tracks_lock);
    if (i < 0 || i >= song_ntracks || !song_tracks[i].code) {
        pthread_mutex_unlock(&tracks_lock);
        return;
    }
    song_pos = i;
    char *tmp = song_dup(song_tracks[i].code);
    pthread_mutex_unlock(&tracks_lock);
    if (tmp) { track_load_from_code(tmp); free(tmp); }
}

void scheduler_track_next(void) {
    pthread_mutex_lock(&tracks_lock);
    int n = song_ntracks, p = song_pos;
    pthread_mutex_unlock(&tracks_lock);
    if (n <= 0) return;
    scheduler_track_goto((p + 1) % n);
}

void scheduler_track_prev(void) {
    pthread_mutex_lock(&tracks_lock);
    int n = song_ntracks, p = song_pos;
    pthread_mutex_unlock(&tracks_lock);
    if (n <= 0) return;
    scheduler_track_goto((p - 1 + n) % n);
}

int scheduler_track_move(int i, int j) {
    int ok = 0;
    pthread_mutex_lock(&tracks_lock);
    if (i >= 0 && i < song_ntracks) {
        if (j < 0) j = 0;
        if (j >= song_ntracks) j = song_ntracks - 1;
        if (i != j) {
            SongTrack t;
            memcpy(&t, &song_tracks[i], sizeof(t));
            if (i < j) {
                for (int k = i; k < j; k++)
                    memcpy(&song_tracks[k], &song_tracks[k+1], sizeof(t));
            } else {
                for (int k = i; k > j; k--)
                    memcpy(&song_tracks[k], &song_tracks[k-1], sizeof(t));
            }
            memcpy(&song_tracks[j], &t, sizeof(t));
            if (song_pos == i) song_pos = j;
            else if (song_pos > i && song_pos <= j) song_pos--;
            else if (song_pos < i && song_pos >= j) song_pos++;
        }
        ok = 1;
    }
    pthread_mutex_unlock(&tracks_lock);
    /* no reload: the active buffer still matches what is playing */
    return ok;
}

int scheduler_track_remove(int i) {
    char *tmp = NULL;
    int have = 0;
    pthread_mutex_lock(&tracks_lock);
    if (i < 0 || i >= song_ntracks) { pthread_mutex_unlock(&tracks_lock); return 0; }
    int was_active = (i == song_pos);
    free(song_tracks[i].code);
    if (i < song_ntracks - 1)
        memmove(&song_tracks[i], &song_tracks[i + 1],
                (size_t)(song_ntracks - i - 1) * sizeof(SongTrack));
    song_ntracks--;
    if (song_ntracks >= 0) song_tracks[song_ntracks].code = NULL;
    if (song_pos >= song_ntracks) song_pos = song_ntracks - 1;
    if (song_ntracks == 0) { song_pos = -1; song_on = 0; }
    if (was_active) {
        if (song_pos >= 0 && song_tracks[song_pos].code) {
            tmp = song_dup(song_tracks[song_pos].code);
            have = 1;
        } else {
            have = -1;
        }
    }
    pthread_mutex_unlock(&tracks_lock);
    if (have > 0 && tmp) track_load_from_code(tmp);
    else if (have < 0) event_queue_clear();
    free(tmp);
    return 1;
}

void scheduler_song_step(void) {
    pthread_mutex_lock(&tracks_lock);
    int go = song_on && !song_hold && song_ntracks > 1;
    int nxt = (song_ntracks > 0) ? (song_pos + 1) % song_ntracks : 0;
    pthread_mutex_unlock(&tracks_lock);
    if (go) scheduler_track_goto(nxt);
}

void scheduler_song_sync(void) {
    song_advance_at = 0.0; /* recomputed on next generator pass */
}

typedef struct QNode {
    AudioEvent ev;
    struct QNode *next;
} QNode;

static QNode *qhead = NULL;
static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
static unsigned qgeneration = 1;
static int qcount;
static double qnot_before = -HUGE_VAL;
static _Thread_local struct {
    QNode *head;
    int count, active, failed, replace;
    unsigned generation;
    double cutoff;
} qstaging;

/* note history for the piano roll (consumer writes, UI reads) */
static NoteHist note_hist[NOTE_HIST_N];
static int note_hist_pos = 0;
static int note_hist_count = 0;
static pthread_mutex_t hist_lock = PTHREAD_MUTEX_INITIALIZER;

int note_hist_copy(NoteHist *out, int max) {
    if (!out || max <= 0) return 0;
    pthread_mutex_lock(&hist_lock);
    int n = note_hist_count < max ? note_hist_count : max;
    int start = (note_hist_pos - note_hist_count + NOTE_HIST_N * 2) % NOTE_HIST_N;
    for (int i = 0; i < n; i++)
        out[i] = note_hist[(start + i) % NOTE_HIST_N];
    pthread_mutex_unlock(&hist_lock);
    return n;
}

static int ev_display_midi(const AudioEvent *ev) {
    if (ev->is_drum) {
        const char *t = ev->token;
        if (strcmp(t, "bd") == 0) return 36;
        if (strcmp(t, "sd") == 0 || strcmp(t, "sn") == 0) return 38;
        if (strcmp(t, "hh") == 0 || strcmp(t, "hat") == 0) return 42;
        if (strcmp(t, "cp") == 0 || strcmp(t, "clap") == 0) return 39;
        if (strcmp(t, "tom") == 0) return 47;
        return 36;
    }
    if (ev->freq > 0) {
        int m = (int)floor(69.0 + 12.0 * log(ev->freq / 440.0) / log(2.0) + 0.5);
        if (m < 0) m = 0;
        if (m > 127) m = 127;
        return m;
    }
    return 60;
}

void event_queue_clear(void) {
    pthread_mutex_lock(&qlock);
    while (qhead) { QNode *n = qhead; qhead = qhead->next; native_synth_release(n->ev.native_synth); stretch_release(n->ev.stretch_buffer); free(n); }
    qcount = 0;
    qnot_before = -HUGE_VAL;
    if (++qgeneration == 0) qgeneration = 1;
    pthread_mutex_unlock(&qlock);
}

void event_queue_new_program(double boundary) {
    pthread_mutex_lock(&qlock);
    QNode **link=&qhead;
    while (*link) {
        QNode *node=*link;
        if (node->ev.time>=boundary) {
            *link=node->next; qcount--;
            native_synth_release(node->ev.native_synth); stretch_release(node->ev.stretch_buffer); free(node);
        } else link=&node->next;
    }
    qnot_before=-HUGE_VAL;
    if (++qgeneration==0) qgeneration=1;
    pthread_mutex_unlock(&qlock);
}

void event_queue_refresh(double cutoff) {
    pthread_mutex_lock(&qlock);
    qstaging.active=1; qstaging.cutoff=cutoff; qstaging.generation=qgeneration;
    qstaging.replace=1;
    pthread_mutex_unlock(&qlock);
}

/* Normal generator publication: append completed work without withdrawing
   already queued future events. Returns 1 when this caller owns the staging
   transaction; an existing refresh transaction keeps ownership. */
int event_queue_publish_begin(void) {
    int owned=0;
    pthread_mutex_lock(&qlock);
    if (!qstaging.active) {
        qstaging.active=1; qstaging.cutoff=-HUGE_VAL; qstaging.generation=qgeneration;
        qstaging.replace=0; owned=1;
    }
    pthread_mutex_unlock(&qlock);
    return owned;
}

/* qlock held. Publishes or discards this thread's staged events. */
static int finish_staging_locked(int success) {
    int commit=success && !qstaging.failed && qgeneration==qstaging.generation;
    double cutoff=qstaging.cutoff;
    if (commit && qstaging.replace) {
        cutoff=fmax(cutoff,audio_time_now()+.002);
        QNode **link=&qhead;
        while (*link) {
            QNode *node=*link;
            if (node->ev.time>cutoff) {
                *link=node->next; qcount--;
                native_synth_release(node->ev.native_synth);
                stretch_release(node->ev.stretch_buffer); free(node);
            } else link=&node->next;
        }
    }
    if (commit) {
        QNode *left=qhead, *right=qstaging.head;
        QNode *merged=NULL, **link=&merged;
        while (left && right) {
            if (left->ev.time<=right->ev.time) { *link=left; left=left->next; }
            else if (qcount<8192) { *link=right; right=right->next; qcount++; }
            else break;
            link=&(*link)->next;
        }
        while (left) { *link=left; left=left->next; link=&(*link)->next; }
        if (!left) while (right && qcount<8192) {
            *link=right; right=right->next; qcount++; link=&(*link)->next;
        }
        *link=NULL;
        while (right) {
            QNode *node=right; right=right->next;
            native_synth_release(node->ev.native_synth);
            stretch_release(node->ev.stretch_buffer); free(node);
        }
        qhead=merged;
        if (qstaging.replace) qnot_before=cutoff;
    } else {
        while (qstaging.head) {
            QNode *node=qstaging.head; qstaging.head=node->next;
            native_synth_release(node->ev.native_synth);
            stretch_release(node->ev.stretch_buffer); free(node);
        }
    }
    memset(&qstaging,0,sizeof(qstaging));
    return commit;
}

int event_queue_refresh_finish(int success) {
    if (!qstaging.active) return 0;
    pthread_mutex_lock(&qlock);
    int commit=finish_staging_locked(success);
    pthread_mutex_unlock(&qlock);
    return commit;
}

int event_queue_publish_finish(int owned, int success) {
    if (!owned) return 1;
    pthread_mutex_lock(&qlock);
    int commit=finish_staging_locked(success);
    pthread_mutex_unlock(&qlock);
    return commit;
}

double event_queue_not_before(void) {
    if (qstaging.active) return qstaging.cutoff;
    pthread_mutex_lock(&qlock); double cutoff=qnot_before;
    pthread_mutex_unlock(&qlock); return cutoff;
}

void event_queue_push(AudioEvent *ev) {
    if (qstaging.active) {
        if ((ev->generation && ev->generation!=qstaging.generation) || ev->time<=qstaging.cutoff) return;
        if (qstaging.count>=8192) { qstaging.failed=1; return; }
        QNode *node=malloc(sizeof(*node)); if (!node) { qstaging.failed=1; return; }
        node->ev=*ev; native_synth_retain(ev->native_synth); stretch_retain(ev->stretch_buffer);
        QNode **link=&qstaging.head;
        while (*link && (*link)->ev.time<=ev->time) link=&(*link)->next;
        node->next=*link; *link=node; qstaging.count++; return;
    }
    pthread_mutex_lock(&qlock);
    if ((ev->generation && ev->generation != qgeneration) || ev->time<=qnot_before || qcount >= 8192) {
        pthread_mutex_unlock(&qlock); return;
    }
    QNode *node = malloc(sizeof(QNode));
    if (!node) { pthread_mutex_unlock(&qlock); return; }
    node->ev = *ev;
    native_synth_retain(node->ev.native_synth);
    stretch_retain(node->ev.stretch_buffer);
    // insert sorted by time
    QNode **pp = &qhead;
    while (*pp && (*pp)->ev.time <= ev->time) pp = &(*pp)->next;
    node->next = *pp;
    *pp = node;
    qcount++;
    pthread_mutex_unlock(&qlock);
}

int event_queue_depth(void) {
    pthread_mutex_lock(&qlock);
    int n = qcount;
    pthread_mutex_unlock(&qlock);
    return n;
}

static void queue_token(char *dst, size_t n, const char *src) {
    size_t w = 0;
    if (!dst || n == 0) return;
    if (src) {
        for (size_t i = 0; src[i] && w + 1 < n; i++) {
            unsigned char c = (unsigned char)src[i];
            if (c <= 32 || c >= 127) c = '_';
            dst[w++] = (char)c;
        }
    }
    if (w == 0) dst[w++] = '-';
    dst[w] = 0;
}

int scheduler_debug_queue(char *out, size_t outlen, int limit) {
    typedef struct {
        char token[24];
        int inst, smp, orbit;
        double when, dur, freq, gain, begin, end, speed;
        double cut, q, ft, delay;
    } Row;
    Row rows[48];
    int n = 0, total, i;
    size_t pos = 0;
    if (!out || outlen < 2) return -1;
    if (limit < 1) limit = 1;
    if (limit > 48) limit = 48;
    out[0] = 0;
    pthread_mutex_lock(&qlock);
    total = qcount;
    for (QNode *p = qhead; p && n < limit; p = p->next) {
        const AudioEvent *ev = &p->ev;
        Row *r = &rows[n++];
        memset(r, 0, sizeof(*r));
        queue_token(r->token, sizeof(r->token), ev->token);
        r->inst = ev->inst;
        r->smp = ev->smp;
        r->orbit = ev->orbit;
        r->when = ev->time;
        r->dur = ev->duration;
        r->freq = ev->freq;
        r->gain = ev->gain;
        r->speed = ev->smp_speed;
        r->delay = ev->delay_send;
        if (ev->dsp.enabled) {
            r->begin = ev->dsp.p[DSP_BEGIN];
            r->end = ev->dsp.p[DSP_END];
            r->cut = ev->dsp.p[DSP_LPF];
            r->q = ev->dsp.p[DSP_LPQ];
            r->ft = ev->dsp.p[DSP_FTYPE];
        } else {
            r->end = (ev->smp_end > 0.0 && ev->smp_end <= 1.0) ? ev->smp_end : 1.0;
            r->cut = ev->lpf;
            r->q = ev->lpq;
            r->ft = -1;
        }
    }
    pthread_mutex_unlock(&qlock);
    {
        int hdr = snprintf(out, outlen, "queue %d showing %d\n", total, n);
        if (hdr < 0 || (size_t)hdr >= outlen) return (int)(outlen - 1);
        pos = (size_t)hdr;
    }
    for (i = 0; i < n; i++) {
        const Row *r = &rows[i];
        int w = snprintf(out + pos, outlen - pos,
            "q%d inst=%s tok=%s when=%.3f dur=%.3f freq=%.2f gain=%.3f "
            "smp=%d begin=%.3f end=%.3f spd=%.3f cut=%.3f q=%.3f ft=%.0f orb=%d dly=%.3f\n",
            i, audio_inst_name(r->inst), r->token, r->when, r->dur, r->freq, r->gain,
            r->smp, r->begin, r->end, r->speed, r->cut, r->q, r->ft, r->orbit, r->delay);
        if (w < 0) break;
        if ((size_t)w >= outlen - pos) return (int)(outlen - 1);
        pos += (size_t)w;
    }
    return (int)pos;
}

unsigned event_queue_generation(void) {
    pthread_mutex_lock(&qlock);
    unsigned n = qgeneration;
    pthread_mutex_unlock(&qlock);
    return n;
}

/* consumer thread */
static pthread_t consumer_tid, generator_tid;

/* Reserve native voice state before the next maximum-size render block.
   MIDI/OSC and history stay on their due-time path. Native state allocation
   happens here, outside both the audio callback and the queue lock. */
static int prepare_queued_audio(double now) {
    AudioEvent event; int found=0;
    pthread_mutex_lock(&qlock);
    for (QNode *node=qhead;node && node->ev.time<=now+.1;node=node->next) {
        if (!node->ev.generation || node->ev.audio_prepared || node->ev.csound_only || node->ev.osc_only) continue;
        node->ev.audio_prepared=1; event=node->ev; found=1;
        native_synth_retain(event.native_synth); stretch_retain(event.stretch_buffer);
        break;
    }
    pthread_mutex_unlock(&qlock);
    if (found) {
        audio_trigger_event(&event);
        native_synth_release(event.native_synth); stretch_release(event.stretch_buffer);
    }
    return found;
}

static void *consumer_thread(void *arg) {
    (void)arg;
    while (scheduler_running) {
        audio_report_errors();
        double now = audio_time_now();
        for (;;) {
            now=audio_time_now();
            pthread_mutex_lock(&qlock);
            QNode *n = qhead;
            if (n && n->ev.time <= now + 0.002) {
                AudioEvent ev = n->ev;
                qhead = n->next;
                qcount--;
                free(n);
                pthread_mutex_unlock(&qlock);
                if (ev.osc_only && !osc_output_send(&ev.osc,ev.time-audio_time_now()))
                    log_console("OSC: output send failed");
                if (!ev.audio_prepared) audio_trigger_event(&ev);
                native_synth_release(ev.native_synth);
                stretch_release(ev.stretch_buffer);
                if (!ev.control_only && !ev.osc_only && !ev.csound_only && ev.midi_chan >= 0 && ev.midi_note >= 0 && !ev.is_drum) {
                    int vel = (int)(ev.gain * 127.0);
                    int dur = (int)(ev.duration * 1000.0);
                    midi_note(ev.midi_chan, ev.midi_note, vel, dur);
                }
                if (!ev.control_only) {
                    NoteHist h;
                    h.t0 = ev.time;
                    h.dur = ev.duration > 0 ? ev.duration : 0.25;
                    h.midi = ev_display_midi(&ev);
                    h.is_drum = ev.is_drum;
                    h.scope_mask = ev.scope_mask; h.generation = ev.generation;
                    pthread_mutex_lock(&hist_lock);
                    note_hist[note_hist_pos] = h;
                    note_hist_pos = (note_hist_pos + 1) % NOTE_HIST_N;
                    if (note_hist_count < NOTE_HIST_N) note_hist_count++;
                    pthread_mutex_unlock(&hist_lock);
                }
            } else {
                pthread_mutex_unlock(&qlock);
                if (prepare_queued_audio(now)) continue;
                break;
            }
        }
        usleep(1000);
    }
    return NULL;
}

int scheduler_play_spans(double now, int *off, int *len, int max) {
    return scheduler_play_styles(now,off,len,NULL,NULL,max);
}

int scheduler_play_styles(double now, int *off, int *len, uint32_t *color, unsigned *flags, int max) {
    return scheduler_editor_play_styles(now,NULL,off,len,color,flags,max);
}

int scheduler_editor_play_styles(double now, const char *editor, int *off, int *len, uint32_t *color, unsigned *flags, int max) {
    if (!off || !len || max < 1 || !audio_is_playing()) return 0;
    if (color) memset(color,0,(size_t)max*sizeof(*color));
    if (flags) memset(flags,0,(size_t)max*sizeof(*flags));
    int n = 0;
    pthread_mutex_lock(&tracks_lock);
    for (int ti=0;ti<track_count && n<max;ti++) {
        Track *t=&tracks[ti];
        if (t->javascript && t->active && !t->mute && javascript_matches(t->javascript,current_code))
            n+=javascript_editor_play_styles(t->javascript,editor,now,off+n,len+n,color?color+n:NULL,flags?flags+n:NULL,max-n);
    }
    pthread_mutex_unlock(&tracks_lock);
    return n;
}

/* tracks_lock held on entry and return. Match web Cyclist/zyklus: 50 ms
   query windows, a 100 ms driver with 100 ms overlap, 10 ms initial phase and
   100 ms output latency. Accepted audio/OSC/Csound events
   are never withdrawn by a control change. External tempo pivots at the next
   query boundary; event-level tempo pivots at its onset inside that window. */
static int schedule_live_javascript(double now) {
    if (track_count!=1 || !tracks[0].javascript) return 0;
    if (!audio_is_playing()) return 1;
    Track *track=&tracks[0];
    if (track->js_seconds<=0) {
        track->js_seconds=audio_sec_per_cycle();
        track->js_origin=track->next_cycle-track->cycle_index*track->js_seconds;
        track->js_cursor=track->cycle_index;
        track->js_tick=now;
        track->js_anchor_cycle=track->js_cursor;
        track->js_anchor_time=track->next_cycle;
        track->js_phase=track->next_cycle-.1;
    }
    double initial_bpm=audio_get_bpm(), seconds=initial_bpm>0?240/initial_bpm:HUGE_VAL;
    if (seconds!=track->js_seconds) {
        double boundary=track->js_phase+.1;
        if (isfinite(seconds)) track->js_origin=boundary-track->js_cursor*seconds;
        track->js_seconds=seconds;
        track->js_anchor_cycle=track->js_cursor;
        track->js_anchor_time=boundary;
    }
    track->slider_revision=slider_revision();
    JsProgram *program=track->javascript;
    javascript_retain(program);
    unsigned generation=track->compile_generation;
    unsigned transport_revision=track->transport_revision;
    double origin=track->js_origin, begin=track->js_cursor, end=begin;
    double anchor_cycle=track->js_anchor_cycle, anchor_time=track->js_anchor_time;
    int event_tempo=0;
    double phase=track->js_phase;
    int query=now+1e-9>=track->js_tick;
    if (query) track->js_tick=now+.1;
    int audible=track->active && !track->mute;
    pthread_mutex_unlock(&tracks_lock);
    if (query && phase<now) {
        double missed=ceil((now-phase)/.05);
        begin+=missed*.05/seconds; end=begin; phase+=missed*.05;
    }
    while (query && phase<now+.2-1e-9) {
        pthread_mutex_lock(&tracks_lock);
        int current=track_count==1 && tracks[0].compile_generation==generation && tracks[0].transport_revision==transport_revision;
        pthread_mutex_unlock(&tracks_lock);
        if (!current) break;
        phase=round(phase*10000)/10000;
        end=begin+.05/seconds;
        JsAudioClock clock={.seconds=seconds,.anchor_cycle=anchor_cycle,.anchor_time=anchor_time,.end=end};
        /* Like Cyclist, an error in one callback must not stop the driver's
           later windows. Keep any tempo checkpoint already accepted. */
        if (phase>=now && audible && isfinite(seconds))
            (void)js_audio_schedule_clock(program,&clock,begin,end,phase+.15,generation);
        if (clock.seconds!=seconds || clock.anchor_cycle!=anchor_cycle || clock.anchor_time!=anchor_time) event_tempo=1;
        seconds=clock.seconds; anchor_cycle=clock.anchor_cycle; anchor_time=clock.anchor_time;
        if (isfinite(seconds)) origin=anchor_time-anchor_cycle*seconds;
        end=clock.end; begin=end; phase+=.05;
    }
    if (isfinite(seconds)) javascript_update_visuals(program,now,origin,seconds);
    javascript_free(program);
    pthread_mutex_lock(&tracks_lock);
    if (track_count==1 && tracks[0].compile_generation==generation && tracks[0].transport_revision==transport_revision) {
        tracks[0].js_origin=origin; tracks[0].js_seconds=seconds;
        tracks[0].js_anchor_cycle=anchor_cycle;
        tracks[0].js_anchor_time=anchor_time; tracks[0].js_phase=phase;
        if (event_tempo && audio_get_bpm()==initial_bpm) audio_set_bpm(240/seconds);
        tracks[0].js_cursor=end;
        tracks[0].cycle_index=tracks[0].js_cursor>=INT_MAX?INT_MAX:tracks[0].js_cursor<=0?0:(int)floor(tracks[0].js_cursor);
        tracks[0].next_cycle=isfinite(seconds)?origin+tracks[0].cycle_index*seconds:phase+.1;
    }
    return 1;
}

static void *generator_thread(void *arg) {
    (void)arg;
    // align start
    scheduler_start_time = audio_time_now();
    while (scheduler_running) {
        pthread_mutex_lock(&tracks_lock);
        double now = audio_time_now();
        if (schedule_live_javascript(now)) {
            pthread_mutex_unlock(&tracks_lock);
            if (song_on && song_ntracks>1) {
                if (song_hold || song_advance_at<=0) song_advance_at=now+audio_sec_per_cycle();
                if (now>=song_advance_at) { scheduler_song_step(); song_advance_at=now+audio_sec_per_cycle(); }
            }
            usleep(5000); continue;
        }
        pthread_mutex_unlock(&tracks_lock);
        usleep(5000);
    }
    return NULL;
}

void scheduler_init(void) {
    prep_pool_start(0,0);
    scheduler_running = 1;
    pthread_create(&consumer_tid, NULL, consumer_thread, NULL);
    pthread_create(&generator_tid, NULL, generator_thread, NULL);
}
void scheduler_shutdown(void) {
    scheduler_running = 0;
    pthread_join(consumer_tid, NULL);
    pthread_join(generator_tid, NULL);
    prep_pool_stop();
    osc_output_shutdown();
    pthread_mutex_lock(&tracks_lock);
    song_clear_all();
    free(song_tracks);
    song_tracks = NULL;
    song_cap = 0;
    pthread_mutex_unlock(&tracks_lock);
}
void scheduler_start(void) {
    if (!scheduler_running) scheduler_init();
}
void scheduler_transport_set(int on) {
    pthread_mutex_lock(&tracks_lock);
    int was=audio_is_playing();
    audio_toggle_play(on);
    if (!on || !was) {
        double now=audio_time_now();
        for (int i=0;i<track_count;i++) if (tracks[i].javascript) {
            Track *track=&tracks[i];
            track->transport_revision++;
            track->js_cursor=track->js_anchor_cycle=0;
            track->cycle_index=0;
            track->js_phase=on?round((now+.01)*10000)/10000:0;
            track->js_anchor_time=track->js_phase+.1;
            track->js_origin=track->js_anchor_time;
            track->js_tick=on?now:HUGE_VAL;
            track->js_seconds=audio_sec_per_cycle();
            track->next_cycle=on?track->js_origin:0;
        }
    }
    pthread_mutex_unlock(&tracks_lock);
}
void scheduler_stop(void) {
    scheduler_shutdown();
    event_queue_clear();
}
