#include "scheduler.h"
#include "js_audio.h"
#include "websamp.h"
#include "track.h"
#include "pattern.h"
#include "audio.h"
#include "common.h"
#include "sampler.h"
#include "banks.h"
#include "midi.h"
#include "harmony.h"
#include "xform.h"

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
static volatile int song_adv_pending = 0;
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
    song_adv_pending = 0;
    if (on && song_ntracks > 0) {
        song_on = 1;
        song_pos = 0;
        pthread_mutex_unlock(&tracks_lock);
        scheduler_track_goto(0);
        return;
    }
    song_on = on && song_ntracks > 0 ? 1 : 0;
    if (!song_on) song_adv_pending = 0;
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
    song_adv_pending = 0;
}

typedef struct QNode {
    AudioEvent ev;
    struct QNode *next;
} QNode;

static QNode *qhead = NULL;
static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
static unsigned qgeneration = 1;
static int qcount;

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
    while (qhead) { QNode *n = qhead; qhead = qhead->next; native_synth_release(n->ev.native_synth); free(n); }
    qcount = 0;
    if (++qgeneration == 0) qgeneration = 1;
    pthread_mutex_unlock(&qlock);
}

void event_queue_push(AudioEvent *ev) {
    pthread_mutex_lock(&qlock);
    if ((ev->generation && ev->generation != qgeneration) || qcount >= 8192) {
        pthread_mutex_unlock(&qlock); return;
    }
    QNode *node = malloc(sizeof(QNode));
    if (!node) { pthread_mutex_unlock(&qlock); return; }
    node->ev = *ev;
    native_synth_retain(node->ev.native_synth);
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

static void *consumer_thread(void *arg) {
    (void)arg;
    while (scheduler_running) {
        double now = audio_time_now();
        for (;;) {
            pthread_mutex_lock(&qlock);
            QNode *n = qhead;
            if (n && n->ev.time <= now + 0.002) {
                AudioEvent ev = n->ev;
                qhead = n->next;
                qcount--;
                free(n);
                pthread_mutex_unlock(&qlock);
                if (ev.osc_only && audio_is_playing() && !osc_output_send(&ev.osc,ev.time-audio_time_now()))
                    log_console("OSC: output send failed");
                audio_trigger_event(&ev);
                native_synth_release(ev.native_synth);
                if (!ev.osc_only && ev.midi_chan >= 0 && ev.midi_note >= 0 && !ev.is_drum) {
                    int vel = (int)(ev.gain * 127.0);
                    int dur = (int)(ev.duration * 1000.0);
                    midi_note(ev.midi_chan, ev.midi_note, vel, dur);
                }
                {
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
                break;
            }
        }
        usleep(1000);
    }
    return NULL;
}

static int clamp_midi_s(int m) {
    if (m < 0) return 0;
    if (m > 127) return 127;
    return m;
}

/* Scale degrees win when | scale is set. Chord symbols next. Else a note. */
static int token_midis(const Track *t, const char *tok, int *out, int max) {
    if (!tok || max < 1) return 0;
    if (t->scale_n > 0) {
        int deg = 0, acc = 0;
        if (harmony_degree(tok, &deg, &acc)) {
            int m = harmony_degree_midi(deg, acc, t->scale_root, t->scale_steps, t->scale_n);
            out[0] = clamp_midi_s(m + t->semi);
            return 1;
        }
    }
    int n = harmony_chord(tok, t->chord, out, max);
    if (n > 0) {
        for (int i = 0; i < n; i++) out[i] = clamp_midi_s(out[i] + t->semi);
        return n;
    }
    out[0] = clamp_midi_s(note_name_to_midi(tok) + t->semi);
    return 1;
}

static void stamp_fx(Track *t, const PatEvent *pe, int cycle, double when, AudioEvent *ev) {
    if (t->lpf_sig.kind) {
        double h = xform_signal(t->lpf_sig.kind, t->lpf_sig.slow, t->lpf_sig.fast,
            t->lpf_sig.has_range, t->lpf_sig.lo, t->lpf_sig.hi, cycle, when);
        if (h < 20) h = 20;
        if (h > 18000) h = 18000;
        ev->lpf = h;
    }
    if (t->dec_sig.kind) {
        double d = xform_signal(t->dec_sig.kind, t->dec_sig.slow, t->dec_sig.fast,
            t->dec_sig.has_range, t->dec_sig.lo, t->dec_sig.hi, cycle, when);
        if (d < 0) d = 0;
        if (d > 8) d = 8;
        ev->dec = d;
    }
    if (t->end_sig.kind) {
        double e = xform_signal(t->end_sig.kind, t->end_sig.slow, t->end_sig.fast,
            t->end_sig.has_range, t->end_sig.lo, t->end_sig.hi, cycle, when);
        if (e < 0.001) e = 0.001;
        if (e > 1) e = 1;
        ev->smp_end = e;
    } else if (t->smp_end > 0) {
        ev->smp_end = t->smp_end;
    }
    /* 0 means natural speed. A patterned or per-event rate may be negative. */
    double rate = (t->smp_speed != 0.0) ? t->smp_speed : 1.0;
    if (t->speed_pat) {
        double v = 0;
        if (xform_number_at(t->speed_pat, cycle, when, &v) && v != 0.0) rate = v;
    }
    if (pe && pe->rate != 0.0f) rate = pe->rate;
    double mag = fabs(rate);
    if (mag < 0.05) mag = 0.05;
    if (mag > 8.0) mag = 8.0;
    ev->smp_speed = (rate < 0.0) ? -mag : mag;
    if (pe && pe->send > 0) ev->delay_send = pe->send;
    else if (t->delay_amt >= 0) ev->delay_send = t->delay_amt;
    ev->delay_time = t->delay_time;
    ev->delay_fb = t->delay_fb;
    if (t->lpq > 0) ev->lpq = t->lpq;
    else if (t->lpq < 0 && ev->lpf > 0) ev->lpq = 1.0; /* WebAudio default Q */
    if (t->has_lpenv) {
        ev->has_lpenv = 1;
        ev->lpenv = t->lpenv;
        ev->lpa = t->lpa;
    }
}

static void push_pitched(Track *t, const PatEvent *pe, const char *tok, int midi,
                         double cycle_start, double start_frac, double dur_frac,
                         int cycle, double when) {
    if (start_frac < 0) start_frac = 0;
    /* .off() may place the copy a fraction into the next cycle. */
    if (start_frac >= 2.0) return;
    if (dur_frac > 8.0) dur_frac = 8.0;
    if (start_frac < 1.0 && start_frac + dur_frac > 1.0) dur_frac = 1.0 - start_frac;
    if (dur_frac < 0.001) return;
    AudioEvent ev;
    memset(&ev, 0, sizeof(ev));
    if (tok) snprintf(ev.token, sizeof(ev.token), "%s", tok);
    double cycle_dur = audio_sec_per_cycle();
    ev.time = cycle_start + start_frac * cycle_dur;
    ev.duration = dur_frac * cycle_dur;
    if (ev.duration < 0.01) ev.duration = 0.01;
    ev.gain = t->gain;
    ev.inst = t->inst;
    ev.pan = t->pan;
    if (pe && pe->inst >= 0) ev.inst = pe->inst;
    if (pe && pe->pan <= 1.0) {
        double p = pe->pan;
        if (p < 0.0) p = 0.0;
        ev.pan = p * 2.0 - 1.0;
    }
    ev.wt = t->wt;
    ev.smp = -1;
    ev.att = t->att;
    ev.rel = t->rel;
    ev.dec = t->dec;
    ev.sus = t->sus;
    ev.pw = t->pw;
    ev.lpf = t->lpf_hz;
    ev.lfo_tgt = t->lfo_tgt;
    ev.lfo_wave = t->lfo_wave;
    ev.lfo_hz = t->lfo_hz;
    ev.lfo_depth = t->lfo_depth;
    ev.midi_chan = -1;
    ev.midi_note = -1;
    midi = clamp_midi_s(midi);
    ev.freq = midi_to_freq(midi);
    if (t->bankfrag[0]) {
        int id = websamp_acquire(t->bankfrag, midi);
        if (id >= 0) {
            ev.smp = id;
            ev.smp_gate = 1;
        }
    }
    if (ev.smp < 0 && t->bank[0]) {
        ev.smp = smp_acquire_note(t->bank, midi);
        ev.smp_gate = 1;
    }
    if (t->midi_chan >= 0) {
        ev.midi_chan = t->midi_chan;
        ev.midi_note = midi;
    }
    stamp_fx(t, pe, cycle, when, &ev);
    event_queue_push(&ev);
}

static int same_slot(const PatEvent *a, const PatEvent *b) {
    if (fabs(a->start - b->start) >= 1e-6 || fabs(a->dur - b->dur) >= 1e-6) return 0;
    if (fabs(a->pan - b->pan) >= 1e-6) return 0;
    if (a->inst != b->inst) return 0;
    return 1;
}

/* Comma stacks and chord symbols share one time span. Arp indexes that stack. */
static void emit_pitched_group(Track *t, const PatEvent *evs, int n,
                               double cycle_start, int repeat, int r, int cycle_index) {
    int mids[32];
    int nm = 0;
    char label[64];
    label[0] = 0;
    if (n > 0) snprintf(label, sizeof(label), "%s", evs[0].token);
    for (int i = 0; i < n && nm < 32; i++) {
        int tmp[12];
        int c = token_midis(t, evs[i].token, tmp, 12);
        for (int k = 0; k < c && nm < 32; k++) mids[nm++] = clamp_midi_s(tmp[k] + evs[i].extra);
    }
    if (nm == 0) return;
    double start_frac = (evs[0].start / repeat) + (double)r / repeat;
    double dur_frac = evs[0].dur / repeat;
    if (t->arp && nm > 1) {
        PatEventList al;
        patlist_init(&al);
        expand_node(t->arp, &al, 0.0, 1.0, cycle_index);
        for (int i = 0; i < al.n; i++) {
            char *end = NULL;
            long ix = strtol(al.items[i].token, &end, 10);
            if (end == al.items[i].token || (end && *end != 0)) continue;
            long m = ix % nm;
            if (m < 0) m += nm;
            double sf = start_frac + al.items[i].start * dur_frac;
            double df = al.items[i].dur * dur_frac;
            push_pitched(t, &evs[0], label, mids[m], cycle_start, sf, df, cycle_index, sf);
        }
        patlist_free(&al);
        return;
    }
    for (int i = 0; i < nm; i++)
        push_pitched(t, &evs[0], label, mids[i], cycle_start, start_frac, dur_frac,
                     cycle_index, start_frac);
}

static double control_at(Node *pat, int cycle, double when, double fb) {
    if (!pat) return fb;
    PatEventList l;
    patlist_init(&l);
    expand_node(pat, &l, 0.0, 1.0, cycle);
    double v = fb;
    for (int i = 0; i < l.n; i++) {
        double a = l.items[i].start;
        double b = a + l.items[i].dur;
        if (when + 1e-9 < a || when > b + 1e-6) continue;
        char *end = NULL;
        double x = strtod(l.items[i].token, &end);
        if (end != l.items[i].token && end && *end == 0 && isfinite(x)) v = x;
    }
    patlist_free(&l);
    return v;
}

static void apply_controls(Track *t, int cycle, double when) {
    if (t->gain_pat) {
        double g = control_at(t->gain_pat, cycle, when, t->gain);
        if (g < 0) g = 0;
        if (g > 2) g = 2;
        t->gain = g;
    }
    if (t->lpf_pat) {
        double h = control_at(t->lpf_pat, cycle, when, t->lpf_hz > 0 ? t->lpf_hz : 1000);
        if (h >= 20 && h <= 18000) t->lpf_hz = h;
    }
    if (t->pan_pat) {
        double p = control_at(t->pan_pat, cycle, when, 0.5);
        if (p >= 0.0 && p <= 1.0) p = p * 2.0 - 1.0;
        if (p < -1) p = -1;
        if (p > 1) p = 1;
        t->pan = p;
    }
}

static void load_xform(const Track *t, XForm *xf) {
    memset(xf, 0, sizeof(*xf));
    xf->structure = t->struct_pat;
    xf->mask = t->mask_pat;
    xf->off_time = t->off_time;
    xf->has_off = t->has_off;
    xf->off_add = t->off_add;
    xf->off_degrees = t->off_degrees;
    xf->sometimes_p = t->sometimes_p;
    xf->sometimes_fast = t->sometimes_fast;
    xf->sometimes_slow = t->sometimes_slow;
    xf->rarely_p = t->rarely_p;
    xf->rarely_rate = t->rarely_rate;
    xf->rarely_speed = t->rarely_speed;
    xf->rarely_send = t->rarely_send;
    xf->degrade = t->post_degrade;
    xf->jux = t->jux;
    xf->jux_rev = t->jux_rev;
    xf->jux_inst = t->jux ? t->jux_inst : -1;
    xf->off_fn = t->off_fn;
    xf->off_slow = t->off_slow;
    xf->off_late = t->off_late;
    xf->off_inst = t->off_fn ? t->off_inst : -1;
    xf->off_add_pat = t->off_add_pat;
    xf->pat = t->pattern;
    xf->rev = t->rev;
    xf->palindrome = t->palindrome;
}

static void shape_events(Track *t, PatEventList *list, int cycle_index) {
    if (t->jux) xform_jux(list, t->jux_rev, t->jux_inst);
    XForm xf;
    load_xform(t, &xf);
    xform_events(&xf, list, cycle_index);
    if (t->off_fn) xform_off_arrow(&xf, list, cycle_index);
}

/* n("0 1").s("amencutup") with no scale: the number is the sample index. */
static int token_index(const char *tok, int *out) {
    const char *p = tok;
    long v = 0;
    if (!p || !out) return 0;
    if (*p == '+') p++;
    if (*p < '0' || *p > '9') return 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        if (v > 1000000) return 0;
        p++;
    }
    if (*p != 0) return 0;
    *out = (int)v;
    return 1;
}

static void push_n_slice(Track *t, const PatEvent *pe, int index,
                         double cycle_start, double start_frac, double dur_frac,
                         int cycle) {
    AudioEvent ev;
    double cycle_dur;
    if (start_frac < 0) start_frac = 0;
    if (start_frac >= 2.0) return;
    if (dur_frac > 8.0) dur_frac = 8.0;
    if (start_frac < 1.0 && start_frac + dur_frac > 1.0) dur_frac = 1.0 - start_frac;
    if (dur_frac < 0.001) return;
    if (index < 0 || index > 1000000) return;
    memset(&ev, 0, sizeof(ev));
    /* Bound the name so "bank:N" always fits the 64-byte token. */
    snprintf(ev.token, sizeof(ev.token), "%.40s:%d", t->bankfrag, index);
    cycle_dur = audio_sec_per_cycle();
    ev.time = cycle_start + start_frac * cycle_dur;
    ev.duration = dur_frac * cycle_dur;
    if (ev.duration < 0.01) ev.duration = 0.01;
    ev.gain = t->gain;
    ev.pan = t->pan;
    ev.lpf = t->lpf_hz;
    ev.inst = INST_SINE;
    ev.freq = 0; /* speed 1: a MIDI numeral must not pitch the slice */
    ev.smp = websamp_acquire(ev.token, -1);
    /* Array banks play the slice out. clip() is what would cut at the hap. */
    ev.smp_gate = 0;
    ev.att = t->att;
    ev.rel = t->rel;
    ev.midi_chan = -1;
    ev.midi_note = -1;
    if (ev.smp < 0 && t->bank[0]) {
        char num[16];
        snprintf(num, sizeof(num), "%d", index);
        ev.smp = smp_acquire(t->bank, num);
    }
    stamp_fx(t, pe, cycle, start_frac, &ev);
    if (ev.smp < 0) return;
    event_queue_push(&ev);
}

static void generate_track_cycle(Track *t, double cycle_start, int cycle_index) {
    if (t->javascript) { js_audio_schedule(t->javascript,cycle_start,cycle_index,event_queue_generation()); return; }
    PatEventList list; patlist_init(&list);
    /* A signal statement has no mini-notation. Sample at each slice start. */
    if (t->sig && !t->pattern) sig_events(t->sig, &list, cycle_index);
    else expand_node(t->pattern, &list, 0.0, 1.0, cycle_index);
    /* .rev mirrors every cycle. .palindrome mirrors odd cycles only. */
    if (xform_wants_rev(t->rev, t->palindrome, cycle_index))
        xform_mirror(&list);
    int is_drum_track = (strstr(t->keyword,"drum")!=NULL || strcmp(t->keyword,"sound")==0);
    int is_sample_track = (strcmp(t->keyword, "s") == 0);

    /* shuffle: permute tokens across slots (deterministic per cycle) */
    if (t->shuffle && list.n > 1) {
        unsigned int seed = (unsigned int)(cycle_index * 2654435761u + 97u);
        for (int i = list.n - 1; i > 0; i--) {
            seed = seed * 1664525u + 1013904223u;
            int j = (int)(seed % (unsigned int)(i + 1));
            char tmp[64];
            memcpy(tmp, list.items[i].token, 64);
            memcpy(list.items[i].token, list.items[j].token, 64);
            memcpy(list.items[j].token, tmp, 64);
        }
    }

    shape_events(t, &list, cycle_index);

    // fast repeat: compress & repeat within cycle handled? We'll do here.
    int repeat = t->fast_repeat > 1 ? t->fast_repeat : 1;
    for (int r = 0; r < repeat; r++) {
        if (!is_drum_track && !is_sample_track) {
            int as_index = (strcmp(t->keyword, "n") == 0 && t->bankfrag[0]
                            && t->scale_n == 0);
            int i = 0;
            while (i < list.n) {
                int j = i + 1;
                while (j < list.n && same_slot(&list.items[i], &list.items[j])) j++;
                if (t->degrade >= 0.0) {
                    double p = t->degrade > 1.0 ? 1.0 : t->degrade;
                    if (((double)rand() / (double)RAND_MAX) < p) { i = j; continue; }
                }
                double g0 = t->gain, l0 = t->lpf_hz, p0 = t->pan;
                double when = (list.items[i].start / repeat) + (double)r / repeat;
                apply_controls(t, cycle_index, when);
                if (as_index) {
                    int k;
                    for (k = i; k < j; k++) {
                        int ix = 0;
                        double sf = (list.items[k].start / repeat) + (double)r / repeat;
                        double df = list.items[k].dur / repeat;
                        if (token_index(list.items[k].token, &ix))
                            push_n_slice(t, &list.items[k], ix, cycle_start, sf, df, cycle_index);
                        else
                            emit_pitched_group(t, &list.items[k], 1, cycle_start, repeat, r, cycle_index);
                    }
                } else {
                    emit_pitched_group(t, &list.items[i], j - i, cycle_start, repeat, r, cycle_index);
                }
                t->gain = g0; t->lpf_hz = l0; t->pan = p0;
                i = j;
            }
            continue;
        }
        for (int i = 0; i < list.n; i++) {
            /* degrade: drop events stochastically */
            if (t->degrade >= 0.0) {
                double p = t->degrade > 1.0 ? 1.0 : t->degrade;
                if (((double)rand() / (double)RAND_MAX) < p) continue;
            }
            double start_frac = (list.items[i].start / repeat) + (double)r / repeat;
            double dur_frac = list.items[i].dur / repeat;
            double g0 = t->gain, l0 = t->lpf_hz, p0 = t->pan;
            apply_controls(t, cycle_index, start_frac);
            if (start_frac >= 2.0 || start_frac < 0) {
                t->gain = g0; t->lpf_hz = l0; t->pan = p0;
                continue;
            }
            if (start_frac < 1.0 && start_frac + dur_frac > 1.0) {
                dur_frac = 1.0 - start_frac;
                if (dur_frac < 0.001) {
                    t->gain = g0; t->lpf_hz = l0; t->pan = p0;
                    continue;
                }
            }

            // resolve token
            AudioEvent ev;
            memset(&ev,0,sizeof(ev));
            snprintf(ev.token, sizeof(ev.token), "%s", list.items[i].token);
            // timing
            double cycle_dur = audio_sec_per_cycle();
            ev.time = cycle_start + start_frac * cycle_dur;
            ev.duration = dur_frac * cycle_dur;
            if (ev.duration < 0.01) ev.duration = 0.01;
            ev.gain = t->gain;
            ev.pan = t->pan;
            ev.lpf = t->lpf_hz;
            ev.is_drum = is_drum_track;
            ev.wt = t->wt;
            ev.smp = -1;
            ev.att = t->att;
            ev.rel = t->rel;
            ev.midi_chan = -1;
            ev.midi_note = -1;
            if (is_drum_track) {
                ev.inst = INST_SINE; // unused
                ev.freq = 0;
                ev.smp = websamp_acquire(list.items[i].token, -1);
                if (ev.smp < 0 && t->bank[0]) ev.smp = smp_acquire(t->bank, list.items[i].token);
                stamp_fx(t, &list.items[i], cycle_index, start_frac, &ev);
            } else {
                ev.inst = INST_SINE; // unused: one-shot only
                ev.freq = 0;
                ev.smp = smp_acquire_path(list.items[i].token);
                if (ev.smp < 0) {
                    t->gain = g0; t->lpf_hz = l0; t->pan = p0;
                    continue; /* missing sample: silent */
                }
            }
            event_queue_push(&ev);
            t->gain = g0; t->lpf_hz = l0; t->pan = p0;
        }
    }
    patlist_free(&list);
}

static int phase_covers(double sf, double df, double phase, int prev) {
    if (df <= 1e-9 || sf >= 2.0 || sf < -0.001) return 0;
    if (!prev) {
        if (sf < 0.0 || sf >= 1.0) return 0;
        double end = sf + df;
        if (end > 1.0) end = 1.0;
        return phase >= sf && phase < end;
    }
    if (sf >= 1.0) {
        double a = sf - 1.0;
        if (a >= 1.0) return 0;
        double end = a + df;
        if (end > 1.0) end = 1.0;
        return phase >= a && phase < end;
    }
    if (sf >= 0.0 && sf + df > 1.0) {
        double tail = sf + df - 1.0;
        if (tail > 1.0) tail = 1.0;
        return phase < tail;
    }
    return 0;
}

static void prepare_highlight(Track *t, PatEventList *list, int cycle_index) {
    patlist_init(list);
    if (!t->pattern) return;
    expand_node(t->pattern, list, 0.0, 1.0, cycle_index);
    if (xform_wants_rev(t->rev, t->palindrome, cycle_index))
        xform_mirror(list);
    if (t->shuffle && list->n > 1) {
        unsigned int seed = (unsigned int)(cycle_index * 2654435761u + 97u);
        for (int i = list->n - 1; i > 0; i--) {
            seed = seed * 1664525u + 1013904223u;
            int j = (int)(seed % (unsigned int)(i + 1));
            char tmp[64];
            memcpy(tmp, list->items[i].token, 64);
            memcpy(list->items[i].token, list->items[j].token, 64);
            memcpy(list->items[j].token, tmp, 64);
            int s = list->items[i].src, sn = list->items[i].src_n;
            list->items[i].src = list->items[j].src;
            list->items[i].src_n = list->items[j].src_n;
            list->items[j].src = s;
            list->items[j].src_n = sn;
        }
    }
    shape_events(t, list, cycle_index);
}

static void add_play_span(int *off, int *len, int *n, int max,
                          int origin, int bias, int plen, const PatEvent *e) {
    if (*n >= max || !e || e->src < 0 || e->src_n <= 0 || origin < 0) return;
    int col = e->src - bias;
    int nlen = e->src_n;
    if (col < 0 || nlen <= 0) return;
    if (plen > 0) {
        if (col >= plen) return;
        if (col + nlen > plen) nlen = plen - col;
    }
    long abs = (long)origin + col;
    if (abs < 0 || abs > INT_MAX) return;
    if ((long)nlen > INT_MAX - abs) nlen = (int)(INT_MAX - abs);
    for (int i = 0; i < *n; i++)
        if (off[i] == (int)abs && len[i] == nlen) return;
    off[*n] = (int)abs;
    len[*n] = nlen;
    (*n)++;
}

static void spans_at_phase(PatEventList *list, double phase, int prev, int repeat,
                           int origin, int bias, int plen,
                           int *off, int *len, int *n, int max) {
    if (repeat < 1) repeat = 1;
    if (repeat > 64) repeat = 64;
    for (int r = 0; r < repeat; r++) {
        for (int i = 0; i < list->n; i++) {
            double sf = list->items[i].start / repeat + (double)r / (double)repeat;
            double df = list->items[i].dur / repeat;
            if (!phase_covers(sf, df, phase, prev)) continue;
            add_play_span(off, len, n, max, origin, bias, plen, &list->items[i]);
        }
    }
}

/* Quoted controls (struct, mask, gain, …) step in cycle time, not the
   track's .fast() grid. */
static void spans_quoted(const Track *t, int cycle, double phase, int prev,
                         int *off, int *len, int *n, int max) {
    for (int h = 0; h < t->hl_n && h < HL_PAT_MAX; h++) {
        if (!t->hl_pat[h] || t->hl_at[h] < 0) continue;
        PatEventList list;
        patlist_init(&list);
        expand_node(t->hl_pat[h], &list, 0.0, 1.0, cycle);
        spans_at_phase(&list, phase, prev, 1, t->hl_at[h], 0, t->hl_len[h],
                       off, len, n, max);
        patlist_free(&list);
    }
}

static int locate_play_cycle(const Track *t, double now, double *phase, int *idx) {
    double cyc = audio_sec_per_cycle();
    if (!(cyc > 1e-6)) return 0;
    double start = t->next_cycle;
    int i = t->cycle_index;
    int guard = 0;
    while (start > now && i > 0 && guard < 128) {
        start -= cyc;
        i--;
        guard++;
    }
    if (start > now) return 0;
    double ph = (now - start) / cyc;
    if (ph < 0) ph = 0;
    if (ph >= 1.0) {
        int extra = (int)ph;
        if (extra > 8) return 0;
        ph -= extra;
        i += extra;
    }
    if (ph < 0) ph = 0;
    if (ph >= 1.0) ph = 0.999999;
    *phase = ph;
    *idx = i;
    return 1;
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
    int legacy_matches=!editor || !strcmp(editor,current_code);
    int any_solo = 0;
    for (int i = 0; i < track_count; i++)
        if (tracks[i].solo) { any_solo = 1; break; }
    for (int ti = 0; ti < track_count && n < max; ti++) {
        Track *t = &tracks[ti];
        if (t->javascript && t->active && !t->mute && javascript_matches(t->javascript,current_code)) {
            n += javascript_editor_play_styles(t->javascript,editor,now,off+n,len+n,color?color+n:NULL,flags?flags+n:NULL,max-n);
            continue;
        }
        if (!legacy_matches || !t->active || t->mute || (any_solo && !t->solo) || !t->pattern) continue;
        double phase = 0;
        int idx = 0;
        if (!locate_play_cycle(t, now, &phase, &idx)) continue;
        PatEventList list;
        if (t->slow_stride <= 1 || (idx % t->slow_stride) == 0) {
            prepare_highlight(t, &list, idx);
            spans_at_phase(&list, phase, 0, t->fast_repeat, t->pat_off, t->pat_bias, t->pat_len,
                           off, len, &n, max);
            patlist_free(&list);
            spans_quoted(t, idx, phase, 0, off, len, &n, max);
        }
        int prev = idx - 1;
        if (prev >= 0 && (t->slow_stride <= 1 || (prev % t->slow_stride) == 0)) {
            prepare_highlight(t, &list, prev);
            spans_at_phase(&list, phase, 1, t->fast_repeat, t->pat_off, t->pat_bias, t->pat_len,
                           off, len, &n, max);
            patlist_free(&list);
            spans_quoted(t, prev, phase, 1, off, len, &n, max);
        }
    }
    pthread_mutex_unlock(&tracks_lock);
    return n;
}

static void *generator_thread(void *arg) {
    (void)arg;
    // align start
    scheduler_start_time = audio_time_now();
    while (scheduler_running) {
        pthread_mutex_lock(&tracks_lock);
        double now = audio_time_now();
        double lookahead = now + 2.0;
        /* song mode: flag advance when audio crosses a cycle boundary.
           (cycle_index can't be used: the 2s lookahead schedules cycles
           long before they play.) */
        if (song_on && song_ntracks > 1 && track_count > 0) {
            if (song_hold) {
                song_advance_at = now + audio_sec_per_cycle(); /* defer */
            } else {
                if (song_advance_at <= 0) song_advance_at = now + audio_sec_per_cycle();
                if (now >= song_advance_at) {
                    song_adv_pending = 1;
                    song_advance_at = now + audio_sec_per_cycle();
                }
            }
        }
        /* Retain the program and release tracks_lock before executing JS or
           resolving samples. A reload invalidates its queued generation. */
        if (track_count == 1 && tracks[0].javascript) {
            Track *t = &tracks[0];
            JsProgram *program = t->javascript;
            javascript_retain(program);
            double starts[16]; int cycles[16], count = 0;
            double duration = audio_sec_per_cycle();
            unsigned generation = event_queue_generation();
            if (t->next_cycle < now-4*duration) t->next_cycle = now;
            double visual_origin = t->next_cycle-t->cycle_index*duration;
            while (t->next_cycle <= lookahead && count < 16) {
                if (t->active && !t->mute) {
                    starts[count] = t->next_cycle; cycles[count++] = t->cycle_index;
                }
                t->next_cycle += duration; t->cycle_index++;
            }
            pthread_mutex_unlock(&tracks_lock);
            for (int i = 0; i < count; i++) js_audio_schedule(program,starts[i],cycles[i],generation);
            javascript_update_visuals(program,now,visual_origin,duration);
            javascript_free(program);
            if (song_adv_pending) { song_adv_pending = 0; scheduler_song_step(); }
            usleep(5000);
            continue;
        }
        int any_solo = 0;
        for (int i = 0; i < track_count; i++)
            if (tracks[i].solo) { any_solo = 1; break; }
        for (int i = 0; i < track_count; i++) {
            Track *t = &tracks[i];
            if (!t->active) continue;
            /* lazy bank resolve: index may not have been ready at evaluate */
            if (t->bankfrag[0] && !t->bank[0]) {
                char dir[1024];
                if (bank_find_dir(t->bankfrag, dir, sizeof(dir))) {
                    snprintf(t->bank, sizeof(t->bank), "%.255s", dir);
                    log_console("bank '%s' -> %s", t->bankfrag, dir);
                } else if (banks_ready()) {
                    t->bankfrag[0] = 0; /* stop retrying: truly missing */
                    log_console("bank not found (synth fallback)");
                }
            }
            // slow check: only generate on certain cycles
            // clamp stale starts (suspend/hiccup) instead of bursting
            {
                double cyc = audio_sec_per_cycle();
                if (t->next_cycle < now - 4.0 * cyc) t->next_cycle = now;
            }
            /* mute/solo: muted lines stay time-aligned but silent */
            int audible = !t->mute && (!any_solo || t->solo);
            while (t->next_cycle <= lookahead) {
                double cycle_start = t->next_cycle;
                int cycle_index = t->cycle_index;
                // for slow_stride: generate only every Nth cycle
                if (audible && (t->slow_stride <= 1 || (cycle_index % t->slow_stride == 0))) {
                    generate_track_cycle(t, cycle_start, cycle_index);
                }
                t->next_cycle += audio_sec_per_cycle();
                t->cycle_index++;
            }
        }
        pthread_mutex_unlock(&tracks_lock);
        /* song auto-advance runs here (not in the UI thread) so headless
           MCP mode steps too; track_load is lock-free at this point */
        if (song_adv_pending) {
            song_adv_pending = 0;
            scheduler_song_step();
        }
        usleep(5000);
    }
    return NULL;
}

void scheduler_init(void) {
    scheduler_running = 1;
    pthread_create(&consumer_tid, NULL, consumer_thread, NULL);
    pthread_create(&generator_tid, NULL, generator_thread, NULL);
}
void scheduler_shutdown(void) {
    scheduler_running = 0;
    pthread_join(consumer_tid, NULL);
    pthread_join(generator_tid, NULL);
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
void scheduler_stop(void) {
    scheduler_shutdown();
    event_queue_clear();
}
