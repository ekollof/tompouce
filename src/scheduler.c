#include "scheduler.h"
#include "track.h"
#include "pattern.h"
#include "audio.h"
#include "common.h"
#include "sampler.h"
#include "banks.h"
#include "midi.h"
#include "harmony.h"

#include <pthread.h>
#include <ctype.h>
#include <math.h>

volatile int scheduler_running = 0;
double scheduler_start_time = 0.0;

/* song list: full code buffers; scheduler plays the active one per cycle */
typedef struct { char code[SONG_CODE_LEN]; } SongTrack;
static SongTrack song_tracks[MAX_SONG_TRACKS];
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

int scheduler_track_copy(const char *code) {
    if (code_blank(code)) {
        log_console("nothing to copy");
        return -1;
    }
    int idx = -1;
    pthread_mutex_lock(&tracks_lock);
    if (song_ntracks >= MAX_SONG_TRACKS) {
        pthread_mutex_unlock(&tracks_lock);
        log_console("track list full (%d)", MAX_SONG_TRACKS);
        return -1;
    }
    int at = (song_pos >= 0 && song_pos < song_ntracks) ? song_pos + 1 : song_ntracks;
    if (at > song_ntracks) at = song_ntracks;
    for (int i = song_ntracks; i > at; i--)
        memcpy(&song_tracks[i], &song_tracks[i-1], sizeof(SongTrack));
    snprintf(song_tracks[at].code, sizeof(song_tracks[at].code), "%s", code);
    song_ntracks++;
    song_pos = at;
    idx = at;
    char tmp[SONG_CODE_LEN];
    snprintf(tmp, sizeof(tmp), "%s", song_tracks[at].code);
    pthread_mutex_unlock(&tracks_lock);
    track_load_from_code(tmp);
    log_console("copied to track %d/%d", idx + 1, song_ntracks);
    return idx;
}

int scheduler_track_code(int i, char *buf, int buflen) {
    if (!buf || buflen <= 0) return 0;
    pthread_mutex_lock(&tracks_lock);
    int ok = (i >= 0 && i < song_ntracks);
    if (ok) snprintf(buf, (size_t)buflen, "%s", song_tracks[i].code);
    pthread_mutex_unlock(&tracks_lock);
    return ok;
}

void scheduler_track_update(int i, const char *code) {
    if (!code) return;
    pthread_mutex_lock(&tracks_lock);
    if (i >= 0 && i < song_ntracks)
        snprintf(song_tracks[i].code, sizeof(song_tracks[i].code), "%s", code);
    pthread_mutex_unlock(&tracks_lock);
}

void scheduler_song_replace(const char **codes, int n, int active) {
    char tmp[SONG_CODE_LEN];
    int have = 0;
    pthread_mutex_lock(&tracks_lock);
    song_ntracks = 0;
    song_pos = -1;
    if (codes && n > 0) {
        if (n > MAX_SONG_TRACKS) n = MAX_SONG_TRACKS;
        for (int i = 0; i < n; i++) {
            if (codes[i]) snprintf(song_tracks[i].code, sizeof(song_tracks[i].code), "%s", codes[i]);
            else song_tracks[i].code[0] = 0;
        }
        song_ntracks = n;
        if (active < 0) active = 0;
        if (active >= n) active = n - 1;
        song_pos = active;
        snprintf(tmp, sizeof(tmp), "%s", song_tracks[active].code);
        have = 1;
    } else {
        song_on = 0;
    }
    pthread_mutex_unlock(&tracks_lock);
    if (have) track_load_from_code(tmp);
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
    char tmp[SONG_CODE_LEN];
    pthread_mutex_lock(&tracks_lock);
    if (i < 0 || i >= song_ntracks) {
        pthread_mutex_unlock(&tracks_lock);
        return;
    }
    song_pos = i;
    snprintf(tmp, sizeof(tmp), "%s", song_tracks[i].code);
    pthread_mutex_unlock(&tracks_lock);
    track_load_from_code(tmp);
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
    char tmp[SONG_CODE_LEN];
    int have = 0;
    pthread_mutex_lock(&tracks_lock);
    if (i < 0 || i >= song_ntracks) { pthread_mutex_unlock(&tracks_lock); return 0; }
    int was_active = (i == song_pos);
    for (int k = i; k < song_ntracks - 1; k++)
        memcpy(&song_tracks[k], &song_tracks[k+1], sizeof(SongTrack));
    song_ntracks--;
    if (song_pos >= song_ntracks) song_pos = song_ntracks - 1;
    if (song_ntracks == 0) { song_pos = -1; song_on = 0; }
    if (was_active) {
        if (song_pos >= 0) {
            snprintf(tmp, sizeof(tmp), "%s", song_tracks[song_pos].code);
            have = 1;
        } else {
            have = -1; /* list empty: clear queue below */
        }
    }
    pthread_mutex_unlock(&tracks_lock);
    if (have > 0) track_load_from_code(tmp);
    else if (have < 0) event_queue_clear();
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
    while (qhead) { QNode *n = qhead; qhead = qhead->next; free(n); }
    pthread_mutex_unlock(&qlock);
}

void event_queue_push(AudioEvent *ev) {
    pthread_mutex_lock(&qlock);
    QNode *node = malloc(sizeof(QNode));
    if (!node) { pthread_mutex_unlock(&qlock); return; }
    node->ev = *ev;
    // insert sorted by time
    QNode **pp = &qhead;
    while (*pp && (*pp)->ev.time <= ev->time) pp = &(*pp)->next;
    node->next = *pp;
    *pp = node;
    pthread_mutex_unlock(&qlock);
}

int event_queue_depth(void) {
    int n = 0;
    pthread_mutex_lock(&qlock);
    for (QNode *p = qhead; p; p = p->next) n++;
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
                free(n);
                pthread_mutex_unlock(&qlock);
                audio_trigger_event(&ev);
                if (ev.midi_chan >= 0 && ev.midi_note >= 0 && !ev.is_drum) {
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

static void push_pitched(Track *t, const char *tok, int midi, double cycle_start,
                         double start_frac, double dur_frac) {
    if (start_frac < 0) start_frac = 0;
    if (start_frac >= 1.0) return;
    if (start_frac + dur_frac > 1.0) dur_frac = 1.0 - start_frac;
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
    if (t->bank[0]) {
        ev.smp = smp_acquire_note(t->bank, midi);
        ev.smp_gate = 1;
    }
    if (t->midi_chan >= 0) {
        ev.midi_chan = t->midi_chan;
        ev.midi_note = midi;
    }
    event_queue_push(&ev);
}

static int same_slot(const PatEvent *a, const PatEvent *b) {
    return fabs(a->start - b->start) < 1e-6 && fabs(a->dur - b->dur) < 1e-6;
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
        for (int k = 0; k < c && nm < 32; k++) mids[nm++] = tmp[k];
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
            push_pitched(t, label, mids[m], cycle_start, sf, df);
        }
        patlist_free(&al);
        return;
    }
    for (int i = 0; i < nm; i++)
        push_pitched(t, label, mids[i], cycle_start, start_frac, dur_frac);
}

static void generate_track_cycle(Track *t, double cycle_start, int cycle_index) {
    PatEventList list; patlist_init(&list);
    // expand pattern (per base cycle)
    expand_node(t->pattern, &list, 0.0, 1.0, cycle_index);
    // apply rev: remap start
    if (t->rev) {
        for (int i = 0; i < list.n; i++) {
            double s = list.items[i].start;
            double d = list.items[i].dur;
            list.items[i].start = 1.0 - (s + d);
            if (list.items[i].start < 0) list.items[i].start = 0;
        }
    }
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

    // fast repeat: compress & repeat within cycle handled? We'll do here.
    int repeat = t->fast_repeat > 1 ? t->fast_repeat : 1;
    for (int r = 0; r < repeat; r++) {
        if (!is_drum_track && !is_sample_track) {
            int i = 0;
            while (i < list.n) {
                int j = i + 1;
                while (j < list.n && same_slot(&list.items[i], &list.items[j])) j++;
                if (t->degrade >= 0.0) {
                    double p = t->degrade > 1.0 ? 1.0 : t->degrade;
                    if (((double)rand() / (double)RAND_MAX) < p) { i = j; continue; }
                }
                emit_pitched_group(t, &list.items[i], j - i, cycle_start, repeat, r, cycle_index);
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
            // clamp
            if (start_frac + dur_frac > 1.0) {
                dur_frac = 1.0 - start_frac;
                if (dur_frac < 0.001) continue;
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
            ev.pan = 0.0;
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
                if (t->bank[0]) ev.smp = smp_acquire(t->bank, list.items[i].token);
            } else {
                ev.inst = INST_SINE; // unused: one-shot only
                ev.freq = 0;
                ev.smp = smp_acquire_path(list.items[i].token);
                if (ev.smp < 0) continue; /* missing sample: silent */
            }
            event_queue_push(&ev);
        }
    }
    patlist_free(&list);
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
}
void scheduler_start(void) {
    if (!scheduler_running) scheduler_init();
}
void scheduler_stop(void) {
    scheduler_shutdown();
    event_queue_clear();
}