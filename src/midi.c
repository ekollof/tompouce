#include "midi.h"
#include "common.h"
#include "audio.h"
#include <alsa/asoundlib.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdatomic.h>

#define MIDI_MAX_OFFS 1024

static pthread_mutex_t midi_in_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t midi_in_tid;
static atomic_int midi_in_running;
static int midi_in_started;
static void *midi_input_worker(void *arg);

typedef struct { double at; int ch; int note; } OffEv;

void midi_state_saved_cc(int client, int port, int chan, int cc, double value);


/* Bounded scheduling observation for tests. Recording is independent of
   hardware: note on/off intent is captured whether or not ALSA accepts
   bytes, so routing stays verifiable without a sequencer. The unified log
   also records dominant MIDI extras (CC, program, bend, touch, sysex,
   transport) in send order. */
static MidiCaptured midi_log[MIDI_CAPTURE_CAPACITY];
static unsigned midi_log_count;
static int midi_capture_enabled;
static pthread_mutex_t midi_log_lock = PTHREAD_MUTEX_INITIALIZER;
void midi_capture_enable(int on) {
    pthread_mutex_lock(&midi_log_lock);
    midi_capture_enabled = on ? 1 : 0;
    if (on) midi_log_count = 0;
    pthread_mutex_unlock(&midi_log_lock);
}
static void midi_log_timed(int kind, int chan, int d1, int d2,
    const unsigned char *sysex, int sysex_len, double duration) {
    pthread_mutex_lock(&midi_log_lock);
    if (!midi_capture_enabled || midi_log_count >= MIDI_CAPTURE_CAPACITY) {
        pthread_mutex_unlock(&midi_log_lock); return;
    }
    MidiCaptured *slot = &midi_log[midi_log_count++];
    slot->kind = kind; slot->chan = chan; slot->d1 = d1; slot->d2 = d2;
    slot->sysex_len = 0;
    if (sysex && sysex_len > 0) {
        if (sysex_len > (int)sizeof(slot->sysex)) sysex_len = (int)sizeof(slot->sysex);
        memcpy(slot->sysex, sysex, (size_t)sysex_len);
        slot->sysex_len = sysex_len;
    }
    slot->at = audio_time_now();
    slot->duration=duration;
    pthread_mutex_unlock(&midi_log_lock);
}
static void midi_log_add(int kind, int chan, int d1, int d2,
    const unsigned char *sysex, int sysex_len) {
    midi_log_timed(kind,chan,d1,d2,sysex,sysex_len,0);
}
unsigned midi_captured_notes(MidiCapturedNote *out, unsigned capacity) {
    unsigned total = 0, copied = 0;
    pthread_mutex_lock(&midi_log_lock);
    for (unsigned i = 0; i < midi_log_count; i++) {
        int note = midi_log[i].kind == MIDI_CAP_NOTE_ON || midi_log[i].kind == MIDI_CAP_NOTE_OFF;
        if (!note) continue;
        if (copied < capacity && out)
            out[copied++] = (MidiCapturedNote){.on = midi_log[i].kind == MIDI_CAP_NOTE_ON,
                .chan = midi_log[i].chan, .note = midi_log[i].d1,
                .vel = midi_log[i].d2, .at = midi_log[i].at};
        total++;
    }
    pthread_mutex_unlock(&midi_log_lock);
    return total;
}
unsigned midi_captured_all(MidiCaptured *out, unsigned capacity) {
    pthread_mutex_lock(&midi_log_lock);
    unsigned count = midi_log_count < capacity ? midi_log_count : capacity;
    if (out) for (unsigned i = 0; i < count; i++) out[i] = midi_log[i];
    unsigned total = midi_log_count;
    pthread_mutex_unlock(&midi_log_lock);
    return total;
}

static snd_seq_t *seq = NULL;
static int seq_port = -1;
static int midi_ok = 0;
static pthread_mutex_t midi_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t midi_cond = PTHREAD_COND_INITIALIZER;
static pthread_t midi_tid;
static int midi_running = 0;
static OffEv offs[MIDI_MAX_OFFS];
static int n_offs = 0;

static void send_raw(int on, int ch, int note, int vel) {
    /* midi_lock held */
    if (!midi_ok) return;
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    if (on) snd_seq_ev_set_noteon(&ev, ch, note, vel);
    else snd_seq_ev_set_noteoff(&ev, ch, note, vel);
    snd_seq_ev_set_source(&ev, seq_port);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    snd_seq_event_output(seq, &ev);
    snd_seq_drain_output(seq);
}

static void *midi_worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&midi_lock);
    while (midi_running) {
        if (n_offs == 0) {
            pthread_cond_wait(&midi_cond, &midi_lock);
            continue;
        }
        double now = audio_time_now();
        double wait = offs[0].at - now;
        if (wait <= 0) {
            OffEv e = offs[0];
            memmove(offs, offs + 1, (size_t)(n_offs - 1) * sizeof(OffEv));
            n_offs--;
            send_raw(0, e.ch, e.note, 64);
            midi_log_add(MIDI_CAP_NOTE_OFF, e.ch, e.note, 64, NULL, 0);
            continue;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        long ns = ts.tv_nsec + (long)(wait * 1e9);
        ts.tv_sec += ns / 1000000000L;
        ts.tv_nsec = ns % 1000000000L;
        pthread_cond_timedwait(&midi_cond, &midi_lock, &ts);
    }
    pthread_mutex_unlock(&midi_lock);
    return NULL;
}

void midi_init(void) {
    if (midi_ok || midi_running) return;
    /* Duplex: subscriptions made through this handle must deliver inbound
       events to our input port, which output-only handles cannot do. */
    int rc = snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0);
    if (rc != 0 || !seq) {
        log_console("midi: ALSA sequencer unavailable");
        seq = NULL;
    } else {
        snd_seq_set_client_name(seq, "tompouce");
        seq_port = snd_seq_create_simple_port(seq, "out",
            SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ |
            SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
            SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        if (seq_port < 0) {
            log_console("midi: port creation failed");
            snd_seq_close(seq);
            seq = NULL;
        }
    }
    /* The scheduler thread runs regardless so note-off timing stays
       observable (and audible nowhere) without hardware. The input thread
       needs a sequencer to poll and only runs with one. */
    midi_ok = seq != NULL;
    midi_running = 1;
    if (pthread_create(&midi_tid, NULL, midi_worker, NULL)) {
        midi_running = midi_ok = 0;
        if (seq) { snd_seq_close(seq); seq = NULL; }
        log_console("midi: note worker startup failed");
        return;
    }
    if (seq) {
        /* Watch connects/disconnects so subscriptions follow replugged
           devices, like the web input's reconnect loop. Best effort. */
        snd_seq_port_subscribe_t *announce;
        snd_seq_port_subscribe_alloca(&announce);
        snd_seq_addr_t system = {SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE};
        snd_seq_addr_t me = {(unsigned)snd_seq_client_id(seq), (unsigned)seq_port};
        snd_seq_port_subscribe_set_sender(announce, &system);
        snd_seq_port_subscribe_set_dest(announce, &me);
        if (snd_seq_subscribe_port(seq, announce))
            log_console("midi: announce subscription failed; reconnect disabled");
        midi_in_running = 1;
        if (pthread_create(&midi_in_tid, NULL, midi_input_worker, NULL)) {
            midi_in_running = 0;
            log_console("midi: input worker startup failed");
        } else midi_in_started = 1;
    }
    if (midi_ok) log_console("midi out %d:%d (connect with aconnect)",
        snd_seq_client_id(seq), seq_port);
}

void midi_shutdown(void) {
    pthread_mutex_lock(&midi_lock);
    if (!midi_running && !midi_ok) { pthread_mutex_unlock(&midi_lock); return; }
    midi_running = 0;
    n_offs = 0;
    pthread_cond_signal(&midi_cond);
    pthread_mutex_unlock(&midi_lock);
    pthread_join(midi_tid, NULL);
    midi_in_running = 0;
    if (midi_in_started) { pthread_join(midi_in_tid, NULL); midi_in_started = 0; }
    pthread_mutex_lock(&midi_lock);
    if (seq) { snd_seq_close(seq); seq = NULL; }
    midi_ok = 0;
    pthread_mutex_unlock(&midi_lock);
}

int midi_ready(void) { return midi_ok; }

void midi_note(int chan, int note, int vel, int dur_ms) {
    midi_note_for(chan,note,vel,fmax(50,dur_ms)/1000.0);
}

void midi_note_for(int chan, int note, int vel, double duration) {
    if (chan<0 || chan>15) return;
    unsigned char channel=(unsigned char)chan;
    midi_notes_for(&channel,1,note,vel,duration);
}

void midi_notes_for(const unsigned char *channels,int count,int note,int vel,double duration) {
    if (!channels || count<1 || count>16 || note<0 || note>127) return;
    for (int i=0;i<count;i++) if (channels[i]>15) return;
    if (!isfinite(duration) || duration<0 || duration>86400) return;
    if (vel < 0) vel = 0;
    if (vel > 127) vel = 127;
    pthread_mutex_lock(&midi_lock);
    if (midi_running && count>MIDI_MAX_OFFS-n_offs) {
        pthread_mutex_unlock(&midi_lock);
        log_console("midi: note-off queue full; note not sent"); return;
    }
    double at=audio_time_now()+duration;
    for (int channel=0;channel<count;channel++) {
        int chan=channels[channel];
        midi_log_timed(MIDI_CAP_NOTE_ON,chan,note,vel,NULL,0,duration);
        if (!midi_running) continue;
        if (midi_ok) send_raw(1,chan,note,vel);
        OffEv e;
        e.at = at;
        e.ch = chan;
        e.note = note;
        int i = n_offs;
        while (i > 0 && offs[i - 1].at > e.at) { offs[i] = offs[i - 1]; i--; }
        offs[i] = e;
        n_offs++;
    }
    if (midi_running) pthread_cond_signal(&midi_cond);
    pthread_mutex_unlock(&midi_lock);
}

/* Dominant MIDI extras. Values arrive validated from the scheduler; ranges
   are rechecked defensively. Without hardware the sends are silent no-ops,
   but the capture still records scheduling intent. */
static void send_fixed_locked(snd_seq_event_t *ev) {
    snd_seq_ev_set_source(ev, seq_port);
    snd_seq_ev_set_subs(ev);
    snd_seq_ev_set_direct(ev);
    snd_seq_event_output(seq, ev);
    snd_seq_drain_output(seq);
}

void midi_cc(int chan, int cc, int value) {
    if (chan < 0 || chan > 15 || cc < 0 || cc > 127 || value < 0 || value > 127) return;
    const unsigned char pairs[1][2]={{(unsigned char)cc,(unsigned char)value}};
    midi_cc_batch(chan,pairs,1);
}

void midi_cc_batch(int chan, const unsigned char (*pairs)[2], int count) {
    if (chan<0 || chan>15 || !pairs || count<1 || count>32) return;
    for (int i=0;i<count;i++) if (pairs[i][0]>127 || pairs[i][1]>127) return;
    pthread_mutex_lock(&midi_lock);
    for (int i=0;i<count;i++) {
        midi_log_add(MIDI_CAP_CC,chan,pairs[i][0],pairs[i][1],NULL,0);
        if (midi_ok && midi_running) {
            snd_seq_event_t ev;
            snd_seq_ev_clear(&ev);
            snd_seq_ev_set_controller(&ev,chan,pairs[i][0],pairs[i][1]);
            send_fixed_locked(&ev);
        }
    }
    pthread_mutex_unlock(&midi_lock);
}

void midi_program(int chan, int prog) {
    if (chan < 0 || chan > 15 || prog < 0 || prog > 127) return;
    midi_log_add(MIDI_CAP_PROG, chan, prog, 0, NULL, 0);
    pthread_mutex_lock(&midi_lock);
    if (midi_ok && midi_running) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_pgmchange(&ev, chan, prog);
        send_fixed_locked(&ev);
    }
    pthread_mutex_unlock(&midi_lock);
}

void midi_bend(int chan, int value) {
    if (chan < 0 || chan > 15 || value < -8192 || value > 8191) return;
    midi_log_add(MIDI_CAP_BEND, chan, value, 0, NULL, 0);
    pthread_mutex_lock(&midi_lock);
    if (midi_ok && midi_running) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_pitchbend(&ev, chan, value);
        send_fixed_locked(&ev);
    }
    pthread_mutex_unlock(&midi_lock);
}

void midi_touch(int chan, int value) {
    if (chan < 0 || chan > 15 || value < 0 || value > 127) return;
    midi_log_add(MIDI_CAP_TOUCH, chan, value, 0, NULL, 0);
    pthread_mutex_lock(&midi_lock);
    if (midi_ok && midi_running) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_chanpress(&ev, chan, value);
        send_fixed_locked(&ev);
    }
    pthread_mutex_unlock(&midi_lock);
}

void midi_sysex(const unsigned char *id, int id_len, const unsigned char *data, int len) {
    if (!id || id_len < 1 || id_len > 3 || !data || len < 0 || len > 256) return;
    unsigned char framed[261];
    framed[0] = 0xF0;
    memcpy(framed + 1, id, (size_t)id_len);
    if (len) memcpy(framed + 1 + id_len, data, (size_t)len);
    framed[1 + id_len + len] = 0xF7;
    int total = id_len + len + 2;
    midi_log_add(MIDI_CAP_SYSEX, 0, id_len, len, framed, total);
    pthread_mutex_lock(&midi_lock);
    if (midi_ok && midi_running) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_sysex(&ev, total, framed);
        snd_seq_ev_set_source(&ev, seq_port);
        snd_seq_ev_set_subs(&ev);
        snd_seq_ev_set_direct(&ev);
        snd_seq_event_output(seq, &ev);
        snd_seq_drain_output(seq);
    }
    pthread_mutex_unlock(&midi_lock);
}

void midi_transport(int cmd) {
    if (cmd < 1 || cmd > 4) return;
    midi_log_add(MIDI_CAP_TRANSPORT, 0, cmd, 0, NULL, 0);
    pthread_mutex_lock(&midi_lock);
    if (midi_ok && midi_running) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        ev.type = cmd == 1 ? SND_SEQ_EVENT_CLOCK : cmd == 2 ? SND_SEQ_EVENT_START :
            cmd == 3 ? SND_SEQ_EVENT_CONTINUE : SND_SEQ_EVENT_STOP;
        send_fixed_locked(&ev);
    }
    pthread_mutex_unlock(&midi_lock);
}

/* MIDI controller input: one merged store plus per-channel stores, exactly
   like the web input (values normalized 0..1, channels 1..16, default 0).
   Keyboard notes are held with arrival times for query windows to consume.
   Everything is bounded; out-of-range channels read 0 and never store. */
static double midi_cc_all[128];
static double midi_cc_chan[16][128];
enum { MIDI_HELD_KEYS = 256 };
static MidiHeldKey midi_held[MIDI_HELD_KEYS];
static unsigned midi_held_count;

static void midi_input_event(int type, int chan, int p1, int p2, double at, int client, int port) {
    if (chan < 0 || chan > 15) return;
    int saved_cc = -1;
    pthread_mutex_lock(&midi_in_lock);
    if (type == 1) {
        double value = fmax(0, fmin(127, p2)) / 127.0;
        if (p1 >= 0 && p1 < 128) {
            midi_cc_all[p1] = value;
            midi_cc_chan[chan][p1] = value;
            saved_cc = p1;
        }
    } else if (type == 0 && p2 > 0) {
        if (p1 >= 0 && p1 < 128) {
            unsigned slot;
            if (midi_held_count >= MIDI_HELD_KEYS) {
                memmove(midi_held, midi_held + 1, (MIDI_HELD_KEYS - 1) * sizeof(midi_held[0]));
                slot = MIDI_HELD_KEYS - 1;
            } else slot = midi_held_count++;
            midi_held[slot] = (MidiHeldKey){.note=p1, .vel=fmax(0, fmin(127, p2)) / 127.0,
                .chan=chan + 1, .at=at, .consumed=0};
        }
    }
    pthread_mutex_unlock(&midi_in_lock);
    if (saved_cc >= 0) midi_state_saved_cc(client, port, chan, saved_cc, 0);
}

double midi_cc_value(int cc, int chan) {
    if (cc < 0 || cc >= 128) return 0;
    pthread_mutex_lock(&midi_in_lock);
    double value = (chan >= 1 && chan <= 16) ? midi_cc_chan[chan - 1][cc] : chan == 0 ? midi_cc_all[cc] : 0;
    pthread_mutex_unlock(&midi_in_lock);
    return value;
}

/* Copy unconsumed held keys with onset at or before horizon into out,
   marking them consumed so each fires once. Returns the count copied. */
static unsigned midi_read_keys(MidiHeldKey *out, unsigned capacity, double horizon, int consume) {
    if (!out) return 0;
    unsigned count = 0;
    pthread_mutex_lock(&midi_in_lock);
    for (unsigned i = 0; i < midi_held_count && count < capacity; i++) {
        if (!midi_held[i].consumed && midi_held[i].at <= horizon) {
            if (consume) midi_held[i].consumed = 1;
            out[count].note = midi_held[i].note;
            out[count].vel = midi_held[i].vel;
            out[count].chan = midi_held[i].chan;
            out[count].at = midi_held[i].at;
            out[count].consumed = 0;
            count++;
        }
    }
    pthread_mutex_unlock(&midi_in_lock);
    return count;
}

unsigned midi_take_keys(MidiHeldKey *out, unsigned capacity, double horizon) {
    return midi_read_keys(out, capacity, horizon, 1);
}

unsigned midi_peek_keys(MidiHeldKey *out, unsigned capacity) {
    return midi_read_keys(out, capacity, INFINITY, 0);
}

typedef struct { int client, port; char name[128]; char client_name[64]; int named, alive; } MidiSender;
enum { MIDI_SENDERS = 8 };
static MidiSender midi_senders[MIDI_SENDERS];
static unsigned midi_sender_count;

static int midi_enumerate_senders(MidiSender *out, char (*names)[128], unsigned capacity) {
    unsigned count = 0;
    if (!seq) return 0;
    snd_seq_client_info_t *cinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(seq, cinfo) >= 0 && count < capacity) {
        int client = snd_seq_client_info_get_client(cinfo);
        if (client == snd_seq_client_id(seq)) continue;
        snd_seq_port_info_t *pinfo;
        snd_seq_port_info_alloca(&pinfo);
        snd_seq_port_info_set_client(pinfo, client);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(seq, pinfo) >= 0 && count < capacity) {
            unsigned caps = snd_seq_port_info_get_capability(pinfo);
            unsigned type = snd_seq_port_info_get_type(pinfo);
            if (!(caps & SND_SEQ_PORT_CAP_READ) || !(caps & SND_SEQ_PORT_CAP_SUBS_READ)) continue;
            if (!(type & SND_SEQ_PORT_TYPE_MIDI_GENERIC)) continue;
            out[count].client = client;
            out[count].port = snd_seq_port_info_get_port(pinfo);
            if (names) snprintf(names[count], 128, "%s:%d", snd_seq_client_info_get_name(cinfo), out[count].port);
            count++;
        }
    }
    return (int)count;
}

/* True when client:port currently feeds our input (announce excluded). */
static int midi_subscription_live(int client, int port) {
    if (!seq || client < 0 || port < 0) return 0;
    snd_seq_query_subscribe_t *query;
    snd_seq_query_subscribe_alloca(&query);
    snd_seq_addr_t me = {(unsigned)snd_seq_client_id(seq), (unsigned)seq_port};
    for (int index = 0; ; index++) {
        snd_seq_query_subscribe_set_root(query, &me);
        snd_seq_query_subscribe_set_type(query, SND_SEQ_QUERY_SUBS_WRITE);
        snd_seq_query_subscribe_set_index(query, index);
        if (snd_seq_query_port_subscribers(seq, query) < 0) return 0;
        const snd_seq_addr_t *sender = snd_seq_query_subscribe_get_addr(query);
        if (sender && sender->client == (unsigned)client && sender->port == (unsigned)port) return 1;
    }
}

/* Forms: "client:port", numeric client id, device-name substring, or an
   ordinal index. Returns 1 and subscribes our input when found. On success
   and matched!=NULL, receives the subscribed sender's display name. Stale
   records never short-circuit: client ids are reused after closes, so an
   existing record is verified before it counts, and dead same-name slots
   are recycled. */
static int midi_subscribe_sender(const char *name, int ordinal, char (*matched)[128]) {
    if (!seq) return 0;
    MidiSender found[64];
    char names[64][128];
    int count = midi_enumerate_senders(found, names, 64);
    int pick = -1;
    if (name && *name) {
        int client = -1, port = -1;
        if (sscanf(name, "%d:%d", &client, &port) == 2) {
            for (int i = 0; i < count; i++)
                if (found[i].client == client && found[i].port == port) { pick = i; break; }
        } else {
            char *end = NULL;
            long id = strtol(name, &end, 10);
            if (end && !*end) {
                for (int i = 0; i < count; i++)
                    if (found[i].client == id) { pick = i; break; }
            } else {
                for (int i = 0; i < count; i++)
                    if (strstr(names[i], name)) { pick = i; break; }
            }
        }
    } else if (ordinal >= 0 && ordinal < count) pick = ordinal;
    if (pick < 0) return 0;
    const char *pickcolon = strrchr(names[pick], ':');
    size_t pickclient = pickcolon ? (size_t)(pickcolon - names[pick]) : strlen(names[pick]);
    pthread_mutex_lock(&midi_in_lock);
    unsigned slot = midi_sender_count;
    for (unsigned i = 0; i < midi_sender_count; i++)
        if (midi_senders[i].client == found[pick].client && midi_senders[i].port == found[pick].port) slot = i;
    if (slot < midi_sender_count && slot < MIDI_SENDERS &&
        midi_subscription_live(midi_senders[slot].client, midi_senders[slot].port)) {
        midi_senders[slot].alive = 1;
        if (matched) memcpy(*matched, midi_senders[slot].name, sizeof(*matched));
        pthread_mutex_unlock(&midi_in_lock);
        return 1;
    }
    if (slot >= midi_sender_count || slot >= MIDI_SENDERS) {
        /* Recycle a dead slot for a replugged device before growing. */
        slot = midi_sender_count;
        for (unsigned i = 0; i < midi_sender_count; i++)
            if (!midi_senders[i].alive && strlen(midi_senders[i].client_name) == pickclient &&
                !memcmp(midi_senders[i].client_name, names[pick], pickclient)) { slot = i; break; }
    }
    if (slot >= MIDI_SENDERS) { pthread_mutex_unlock(&midi_in_lock); return 0; }
    pthread_mutex_unlock(&midi_in_lock);
    snd_seq_addr_t src = {(unsigned)found[pick].client, (unsigned)found[pick].port};
    snd_seq_addr_t dst = {(unsigned)snd_seq_client_id(seq), (unsigned)seq_port};
    snd_seq_port_subscribe_t *subs;
    snd_seq_port_subscribe_alloca(&subs);
    snd_seq_port_subscribe_set_sender(subs, &src);
    snd_seq_port_subscribe_set_dest(subs, &dst);
    { int rc=snd_seq_subscribe_port(seq, subs);
      if (rc) return 0; }
    pthread_mutex_lock(&midi_in_lock);
    if (slot >= MIDI_SENDERS) { pthread_mutex_unlock(&midi_in_lock); return 0; }
    midi_senders[slot] = found[pick];
    { size_t len = strlen(names[pick]);
      if (len >= sizeof(midi_senders[slot].name)) len = sizeof(midi_senders[slot].name) - 1;
      memcpy(midi_senders[slot].name, names[pick], len);
      midi_senders[slot].name[len] = 0; }
    { const char *colon = strrchr(names[pick], ':');
      size_t len = colon ? (size_t)(colon - names[pick]) : strlen(names[pick]);
      if (len >= sizeof(midi_senders[slot].client_name)) len = sizeof(midi_senders[slot].client_name) - 1;
      memcpy(midi_senders[slot].client_name, names[pick], len);
      midi_senders[slot].client_name[len] = 0; }
    midi_senders[slot].named = name && *name ? 1 : 0;
    midi_senders[slot].alive = 1;
    if (matched) memcpy(*matched, midi_senders[slot].name, sizeof(*matched));
    if (slot == midi_sender_count) midi_sender_count++;
    pthread_mutex_unlock(&midi_in_lock);
    return 1;
}

static int midi_has_live_sender(void) {
    if (!seq) return 0;
    snd_seq_query_subscribe_t *query;
    snd_seq_query_subscribe_alloca(&query);
    snd_seq_addr_t me = {(unsigned)snd_seq_client_id(seq), (unsigned)seq_port};
    snd_seq_addr_t ann = {SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE};
    /* Our own announce subscription writes to us; it is not a sender. */
    for (int index = 0; ; index++) {
        snd_seq_query_subscribe_set_root(query, &me);
        snd_seq_query_subscribe_set_type(query, SND_SEQ_QUERY_SUBS_WRITE);
        snd_seq_query_subscribe_set_index(query, index);
        if (snd_seq_query_port_subscribers(seq, query) < 0) return 0;
        const snd_seq_addr_t *sender = snd_seq_query_subscribe_get_addr(query);
        if (!sender || sender->client != ann.client || sender->port != ann.port) return 1;
    }
}

/* Writable ports, for restoring persisted CC values to motorized controls. */
typedef struct { int chan, cc, value; } MidiRestoreEntry;
static int midi_state_snapshot(const char *name, MidiRestoreEntry *out, int cap);
static int midi_enumerate_outputs(MidiSender *out, unsigned capacity) {
    unsigned count = 0;
    if (!seq || !out) return 0;
    snd_seq_client_info_t *cinfo;
    snd_seq_client_info_alloca(&cinfo);
    snd_seq_client_info_set_client(cinfo, -1);
    while (snd_seq_query_next_client(seq, cinfo) >= 0 && count < capacity) {
        int client = snd_seq_client_info_get_client(cinfo);
        if (client == snd_seq_client_id(seq)) continue;
        snd_seq_port_info_t *pinfo;
        snd_seq_port_info_alloca(&pinfo);
        snd_seq_port_info_set_client(pinfo, client);
        snd_seq_port_info_set_port(pinfo, -1);
        while (snd_seq_query_next_port(seq, pinfo) >= 0 && count < capacity) {
            unsigned caps = snd_seq_port_info_get_capability(pinfo);
            unsigned type = snd_seq_port_info_get_type(pinfo);
            if (!(caps & SND_SEQ_PORT_CAP_WRITE) || !(caps & SND_SEQ_PORT_CAP_SUBS_WRITE)) continue;
            if (!(type & SND_SEQ_PORT_TYPE_MIDI_GENERIC)) continue;
            out[count].client = client;
            out[count].port = snd_seq_port_info_get_port(pinfo);
            count++;
        }
    }
    return (int)count;
}

/* Address restores explicitly: subscribed broadcast would also change every
   connected synth and temporarily route ordinary notes to the controller. */
static int midi_send_cc_queued(int client, int port, int chan, int cc, int value) {
    /* midi_lock held. */
    if (!midi_ok || !midi_running) return 0;
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_controller(&ev, chan, cc, value);
    snd_seq_ev_set_source(&ev, seq_port);
    snd_seq_ev_set_dest(&ev, client, port);
    snd_seq_ev_set_direct(&ev);
    int rc = snd_seq_event_output_direct(seq, &ev);
    if (rc >= 0) snd_seq_drain_output(seq);
    return rc >= 0;
}

int midi_restore_sender(const char *client_name) {
    if (!client_name || !*client_name) return 0;
    int client = -1, named = 0;
    pthread_mutex_lock(&midi_in_lock);
    for (unsigned i = 0; i < midi_sender_count; i++)
        if (midi_senders[i].named && !strcmp(midi_senders[i].client_name, client_name)) {
            client = midi_senders[i].client; named = 1;
            break;
        }
    pthread_mutex_unlock(&midi_in_lock);
    if (!named || !seq) return 0;
    MidiRestoreEntry entries[128 * 16];
    int count = midi_state_snapshot(client_name, entries, (int)(sizeof(entries) / sizeof(entries[0])));
    if (!count) return 0;
    MidiSender outputs[64];
    int noutputs = midi_enumerate_outputs(outputs, 64);
    int dest = -1;
    for (int i = 0; i < noutputs; i++)
        if (outputs[i].client == client) { dest = i; break; }
    if (dest < 0) {
        log_console("midi: input '%s' has no writable port for CC restore", client_name);
        return 0;
    }
    int sent = 0;
    /* Unlocked gaps: never stall note scheduling on restore pacing. */
    for (int i = 0; i < count; i++) {
        pthread_mutex_lock(&midi_lock);
        int rc = midi_send_cc_queued(outputs[dest].client, outputs[dest].port,
            entries[i].chan, entries[i].cc, entries[i].value);
        pthread_mutex_unlock(&midi_lock);
        sent += rc;
        struct timespec gap = {.tv_nsec = 5000000};
        nanosleep(&gap, NULL);
    }
    if (sent != count) log_console("midi: CC restore to '%s' sent %d/%d", client_name, sent, count);
    return sent;
}

int midi_open_input(const char *name, int ordinal) {
    /* Web parity: persisted reader defaults load even without hardware. Only
       string names persist; numeric/ordinal opens never do. */
    if (name && *name) midi_state_load(name);
    if (!seq) {
        log_console("midi: ALSA sequencer unavailable");
        return 0;
    }
    char subscribed[128] = "";
    if (midi_subscribe_sender(name, ordinal, &subscribed)) {
        /* Like the web input on connect, load persisted defaults and
           restore motorized controls, keyed by stable client name. */
        char key[64] = "";
        pthread_mutex_lock(&midi_in_lock);
        for (unsigned i = 0; i < midi_sender_count; i++)
            if (!strcmp(midi_senders[i].name, subscribed)) {
                memcpy(key, midi_senders[i].client_name, sizeof(key));
                break;
            }
        pthread_mutex_unlock(&midi_in_lock);
        if (key[0]) {
            midi_state_load(key);
            midi_restore_sender(key);
        }
        return 1;
    }
    if ((!name || !*name) && ordinal == 0 && midi_has_live_sender()) return 1;
    if (name && *name) log_console("midi: input '%s' not found (connect with aconnect)", name);
    else log_console("midi: no input sender found (connect with aconnect)");
    return 0;
}

int midi_input_live(void) {
    return midi_has_live_sender();
}

/* Persisted CC state, mirroring the web input's per-device localStorage.
   One sparse JSON file per string sender name; numeric/ordinal opens never
   persist. Layout: {"all":{"7":0.5},"3":{"7":0.5}} with 1..16 channels. */
static int midi_state_name_ok(const char *name, char *sanitized, size_t n) {
    if (!name || !*name || !sanitized || n == 0) return 0;
    size_t len = 0;
    for (const char *p = name; *p && len + 1 < n && len < 48; p++) {
        unsigned char c = (unsigned char)*p;
        sanitized[len++] = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ? (char)c : '_';
    }
    sanitized[len] = 0;
    return len > 0;
}

static int midi_state_path(const char *name, char *out, size_t n) {
    char sanitized[64], root[1024];
    if (!midi_state_name_ok(name, sanitized, sizeof(sanitized)) || !out || n == 0) return 0;
    user_dir(root, sizeof(root), "XDG_CONFIG_HOME", "tompouce", ".config/tompouce");
    if (!root[0]) return 0;
    if (snprintf(out, n, "%s/midi-input-%s.json", root, sanitized) >= (int)n) return 0;
    return 1;
}

static void midi_state_apply(const cJSON *doc) {
    if (!cJSON_IsObject(doc)) return;
    const cJSON *scope;
    cJSON_ArrayForEach(scope, doc) {
        const char *key = scope->string ? scope->string : "";
        int chan = !strcmp(key, "all") ? 0 : atoi(key);
        if (chan < 0 || chan > 16 || (chan == 0 && strcmp(key, "all"))) continue;
        if (!cJSON_IsObject(scope)) continue;
        const cJSON *entry;
        cJSON_ArrayForEach(entry, scope) {
            char *end = NULL;
            long cc = strtol(entry->string ? entry->string : "", &end, 10);
            if (!end || *end || cc < 0 || cc > 127 || !cJSON_IsNumber(entry)) continue;
            double value = fmax(0, fmin(1, entry->valuedouble));
            if (chan == 0) midi_cc_all[cc] = value;
            else midi_cc_chan[chan - 1][cc] = value;
        }
    }
}

int midi_state_load(const char *name) {
    char path[1152];
    if (!midi_state_path(name, path, sizeof(path))) return -1;
    FILE *file = fopen(path, "r");
    if (!file) return 0;
    char *text = NULL;
    size_t cap = 0, len = 0;
    int chunk;
    while ((chunk = fgetc(file)) != EOF && len + 1 < 65536) {
        if (len + 1 >= cap) {
            size_t grown = cap ? cap * 2 : 1024;
            char *next = realloc(text, grown);
            if (!next) break;
            text = next; cap = grown;
        }
        text[len++] = (char)chunk;
    }
    fclose(file);
    if (!text) return 0;
    text[len] = 0;
    cJSON *doc = cJSON_Parse(text);
    free(text);
    if (!doc) {
        log_console("midi: ignoring unreadable state for '%s'", name ? name : "");
        return 0;
    }
    int count = 0;
    const cJSON *scope;
    cJSON_ArrayForEach(scope, doc) if (cJSON_IsObject(scope)) {
        const cJSON *entry;
        cJSON_ArrayForEach(entry, scope) if (cJSON_IsNumber(entry)) count++;
    }
    pthread_mutex_lock(&midi_in_lock);
    midi_state_apply(doc);
    pthread_mutex_unlock(&midi_in_lock);
    cJSON_Delete(doc);
    return count;
}

/* Snapshot one sender's persisted values for restore without holding locks
   while sending. Returns entries appended to out (cap bounds). */
static int midi_state_snapshot(const char *name, MidiRestoreEntry *out, int cap) {
    char path[1152];
    if (!midi_state_path(name, path, sizeof(path)) || !out || cap <= 0) return 0;
    FILE *file = fopen(path, "r");
    if (!file) return 0;
    char text[65536]; size_t len = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[len] = 0;
    cJSON *doc = cJSON_Parse(text);
    if (!doc) return 0;
    int count = 0;
    const char *channels[] = {"1","2","3","4","5","6","7","8","9","10","11","12","13","14","15","16"};
    for (int c = 0; c < 16 && count < cap; c++) {
        const cJSON *scope = cJSON_GetObjectItemCaseSensitive(doc, channels[c]);
        if (!cJSON_IsObject(scope)) continue;
        const cJSON *entry;
        cJSON_ArrayForEach(entry, scope) {
            char *end = NULL;
            long cc = strtol(entry->string ? entry->string : "", &end, 10);
            if (!end || *end || cc < 0 || cc > 127 || !cJSON_IsNumber(entry)) continue;
            out[count++] = (MidiRestoreEntry){.chan = c, .cc = (int)cc,
                .value = (int)lround(fmax(0, fmin(1, entry->valuedouble)) * 127)};
            if (count >= cap) break;
        }
    }
    cJSON_Delete(doc);
    return count;
}

/* Read-modify-write one CC entry: the file holds only what this sender (or
   a load) wrote, never the whole merged store, so unrelated senders and
   earlier tests cannot leak in. Zero is a real entry, like the web store.
   Keys follow the web convention: merged "all" plus 1-based channels. */
static void midi_state_write(const char *name, int chan, int cc) {
    static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
    char path[1152];
    if (!midi_state_path(name, path, sizeof(path))) return;
    { char root[1024], *slash = strrchr(path, '/');
      if (slash && (size_t)(slash - path) < sizeof(root)) {
          memcpy(root, path, (size_t)(slash - path)); root[slash - path] = 0;
          mkdir(root, 0700);
      } }
    double value = 0;
    pthread_mutex_lock(&midi_in_lock);
    value = midi_cc_chan[chan][cc];
    pthread_mutex_unlock(&midi_in_lock);
    /* Serialize the read-modify-write and publish atomically: a concurrent
       or interrupted writer must never leave a truncated state file, which
       would silently drop every sender's saved controllers. */
    pthread_mutex_lock(&state_lock);
    FILE *read = fopen(path, "r");
    cJSON *doc = NULL;
    if (read) {
        char text[65536]; size_t len = fread(text, 1, sizeof(text) - 1, read);
        fclose(read); text[len] = 0;
        doc = cJSON_Parse(text);
    }
    if (!doc) { doc = cJSON_CreateObject(); if (!doc) { pthread_mutex_unlock(&state_lock); return; } }
    char key[8]; snprintf(key, sizeof(key), "%d", cc);
    char scope[8]; snprintf(scope, sizeof(scope), "%d", chan + 1);
    const char *scopes[] = {"all", scope};
    for (unsigned s = 0; s < 2; s++) {
        cJSON *obj = cJSON_GetObjectItemCaseSensitive(doc, scopes[s]);
        if (!cJSON_IsObject(obj)) {
            obj = cJSON_CreateObject();
            if (!obj) { cJSON_Delete(doc); pthread_mutex_unlock(&state_lock); return; }
            cJSON_AddItemToObject(doc, scopes[s], obj);
        }
        cJSON_DeleteItemFromObject(obj, key);
        if (cJSON_AddNumberToObject(obj, key, value) == NULL) {
            cJSON_Delete(doc); pthread_mutex_unlock(&state_lock); return;
        }
    }
    char *text = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    if (!text) { pthread_mutex_unlock(&state_lock); return; }
    char temporary[1200];
    int temp_ok = snprintf(temporary, sizeof(temporary), "%s.tmp", path) > 0;
    FILE *file = temp_ok ? fopen(temporary, "w") : NULL;
    int ok = file && fputs(text, file) >= 0 && fclose(file) == 0 && rename(temporary, path) == 0;
    if (!ok && temp_ok) unlink(temporary);
    free(text);
    pthread_mutex_unlock(&state_lock);
}

void midi_state_saved_cc(int client, int port, int chan, int cc, double value) {
    (void)value;
    if (chan < 0 || chan > 15 || cc < 0 || cc > 127) return;
    char key[64] = "";
    pthread_mutex_lock(&midi_in_lock);
    for (unsigned i = 0; i < midi_sender_count; i++)
        if (midi_senders[i].named && midi_senders[i].client == client && midi_senders[i].port == port) {
            memcpy(key, midi_senders[i].client_name, sizeof(key));
            break;
        }
    pthread_mutex_unlock(&midi_in_lock);
    if (!key[0]) return;
    midi_state_write(key, chan, cc);
}

/* Deterministic test injection: writes the input stores as if the worker
   parsed an ALSA event, without needing hardware. */
void midi_test_inject(int type, int chan, int p1, int p2) {
    midi_input_event(type, chan, p1, p2, audio_time_now(), -1, -1);
}

void midi_test_inject_from(int client, int port, int type, int chan, int p1, int p2) {
    midi_input_event(type, chan, p1, p2, audio_time_now(), client, port);
}

void midi_test_add_sender(int client, int port, const char *name, int named) {
    pthread_mutex_lock(&midi_in_lock);
    unsigned at = midi_sender_count;
    for (unsigned i = 0; i < midi_sender_count; i++)
        if (midi_senders[i].client == client && midi_senders[i].port == port) at = i;
    if (at < MIDI_SENDERS) {
        size_t len = name ? strlen(name) : 0;
        if (len >= sizeof(midi_senders[at].name)) len = sizeof(midi_senders[at].name) - 1;
        memcpy(midi_senders[at].name, name ? name : "", len);
        midi_senders[at].name[len] = 0;
        midi_senders[at].client = client; midi_senders[at].port = port;
        { const char *colon = strrchr(midi_senders[at].name, ':');
          size_t clen = colon ? (size_t)(colon - midi_senders[at].name) : len;
          if (clen >= sizeof(midi_senders[at].client_name)) clen = sizeof(midi_senders[at].client_name) - 1;
          memcpy(midi_senders[at].client_name, midi_senders[at].name, clen);
          midi_senders[at].client_name[clen] = 0; }
        midi_senders[at].named = named ? 1 : 0;
        midi_senders[at].alive = 1;
        if (at == midi_sender_count) midi_sender_count++;
    }
    pthread_mutex_unlock(&midi_in_lock);
}

int midi_test_sender_alive(int client, int port) {
    int alive = 0;
    pthread_mutex_lock(&midi_in_lock);
    for (unsigned i = 0; i < midi_sender_count; i++)
        if (midi_senders[i].client == client && midi_senders[i].port == port) alive = midi_senders[i].alive;
    pthread_mutex_unlock(&midi_in_lock);
    return alive;
}

/* Reconnect core, shared by the announce worker path and tests. Resubscribes
   named senders whose client name matches and restores their CCs. Records
   whose alive flag disagrees with the kernel (missed EXIT) are healed first
   so they cannot blind future revives. */
static int midi_sender_note_dead(int client, int port);
static int midi_sender_revive(const char *client_name) {
    int revived = 0;
    if (!seq || !client_name) return 0;
    for (;;) {
        struct { int client, port, alive; } cands[MIDI_SENDERS];
        unsigned n = 0;
        pthread_mutex_lock(&midi_in_lock);
        for (unsigned i = 0; i < midi_sender_count && n < MIDI_SENDERS; i++)
            if (midi_senders[i].named && !strcmp(midi_senders[i].client_name, client_name)) {
                cands[n].client = midi_senders[i].client;
                cands[n].port = midi_senders[i].port;
                cands[n].alive = midi_senders[i].alive;
                n++;
            }
        pthread_mutex_unlock(&midi_in_lock);
        if (!n) break;
        int need = 0, connected = 0;
        for (unsigned i = 0; i < n; i++) {
                if (!cands[i].alive) need = 1;
            else if (midi_subscription_live(cands[i].client, cands[i].port)) connected = 1;
            else {
                midi_sender_note_dead(cands[i].client, cands[i].port);
                need = 1;
            }
        }
        if (connected) return revived;
        if (!need) break;
        /* Rematch by client name: replugged devices keep names, not ids. */
        if (midi_subscribe_sender(client_name, -1, NULL)) {
            pthread_mutex_lock(&midi_in_lock);
            for (unsigned i = 0; i < midi_sender_count; i++)
                if (!strcmp(midi_senders[i].client_name, client_name)) midi_senders[i].alive = 1;
            pthread_mutex_unlock(&midi_in_lock);
            midi_restore_sender(client_name);
            log_console("midi: input '%s' reconnected", client_name);
            revived = 1;
        } else break;
    }
    return revived;
}

static int midi_sender_note_dead(int client, int port) {
    int changed = 0;
    pthread_mutex_lock(&midi_in_lock);
    for (unsigned i = 0; i < midi_sender_count; i++)
        if (midi_senders[i].alive && midi_senders[i].client == client && midi_senders[i].port == port) {
            midi_senders[i].alive = 0; changed = 1;
        }
    pthread_mutex_unlock(&midi_in_lock);
    if (changed) log_console("midi: input %d:%d disconnected", client, port);
    return changed;
}

int midi_test_announce(int added, int client, int port, const char *client_name) {
    if (added) {
        /* Report the name match that gates a revive; the resubscribe itself
           needs hardware. Without a match, or without hardware to act on a
           match, nothing changes. */
        int match = 0;
        pthread_mutex_lock(&midi_in_lock);
        for (unsigned i = 0; i < midi_sender_count; i++)
            if (midi_senders[i].named && !midi_senders[i].alive && client_name &&
                !strcmp(midi_senders[i].client_name, client_name)) match = 1;
        pthread_mutex_unlock(&midi_in_lock);
        if (match && seq) midi_sender_revive(client_name);
        return match;
    }
    return midi_sender_note_dead(client, port);
}

/* Mark dead only after verifying the subscription is really gone: under
   rapid delete/recreate churn a stale EXIT can arrive after a successful
   revive, and must not kill the fresh state. */
static int midi_sender_verified_dead(int client, int port) {
    if (midi_subscription_live(client, port)) return 0;
    return midi_sender_note_dead(client, port);
}

static void midi_announce_event(const snd_seq_event_t *event) {
    if (!event) return;
    if (event->type == SND_SEQ_EVENT_PORT_EXIT || event->type == SND_SEQ_EVENT_CLIENT_EXIT ||
        event->type == SND_SEQ_EVENT_PORT_UNSUBSCRIBED) {
        int client = -1, port = -1;
        if (event->type == SND_SEQ_EVENT_PORT_UNSUBSCRIBED) {
            const snd_seq_addr_t *sender = &event->data.connect.sender;
            const snd_seq_addr_t *dest = &event->data.connect.dest;
            if (dest->client != (unsigned)snd_seq_client_id(seq) || dest->port != (unsigned)seq_port) return;
            client = (int)sender->client; port = (int)sender->port;
        } else if (event->type == SND_SEQ_EVENT_PORT_EXIT) {
            client = event->data.addr.client; port = event->data.addr.port;
        } else {
            client = event->data.addr.client; port = -1;
        }
        pthread_mutex_lock(&midi_in_lock);
        struct { int client, port; } hits[MIDI_SENDERS];
        unsigned nhits = 0;
        for (unsigned i = 0; i < midi_sender_count && nhits < MIDI_SENDERS; i++)
            if (midi_senders[i].alive && midi_senders[i].client == client &&
                (port < 0 || midi_senders[i].port == port)) {
                hits[nhits].client = midi_senders[i].client;
                hits[nhits].port = midi_senders[i].port;
                nhits++;
            }
        pthread_mutex_unlock(&midi_in_lock);
        for (unsigned i = 0; i < nhits; i++) midi_sender_verified_dead(hits[i].client, hits[i].port);
    } else if (event->type == SND_SEQ_EVENT_PORT_START || event->type == SND_SEQ_EVENT_CLIENT_START) {
        snd_seq_port_info_t *pinfo;
        snd_seq_port_info_alloca(&pinfo);
        int client = event->data.addr.client;
        if (client < 0) return;
        /* Replugged devices keep names, not ids: match dead senders by
           client name, then resubscribe and restore. */
        snd_seq_client_info_t *cinfo;
        snd_seq_client_info_alloca(&cinfo);
        if (snd_seq_get_any_client_info(seq, client, cinfo) < 0) return;
        midi_sender_revive(snd_seq_client_info_get_name(cinfo));
    }
}

static void *midi_input_worker(void *arg) {
    (void)arg;
    int nfds = snd_seq_poll_descriptors_count(seq, POLLIN);
    struct pollfd *fds = calloc(nfds > 0 ? (size_t)nfds : 1, sizeof(*fds));
    if (!fds) return NULL;
    struct timespec last_check;
    clock_gettime(CLOCK_MONOTONIC, &last_check);
    while (midi_in_running) {
        struct timespec tick;
        clock_gettime(CLOCK_MONOTONIC, &tick);
        if (tick.tv_sec - last_check.tv_sec >= 2) {
            last_check = tick;
            /* Safety net for transports that drop announce events: heal
               missed EXITs and retry missed STARTs. Quiet when settled. */
            struct { int client, port; char name[64]; int alive; } snap[MIDI_SENDERS];
            unsigned n = 0;
            pthread_mutex_lock(&midi_in_lock);
            for (unsigned i = 0; i < midi_sender_count && n < MIDI_SENDERS; i++) {
                snap[n].client = midi_senders[i].client; snap[n].port = midi_senders[i].port;
                memcpy(snap[n].name, midi_senders[i].client_name, sizeof(snap[n].name));
                snap[n].alive = midi_senders[i].alive; n++;
            }
            pthread_mutex_unlock(&midi_in_lock);
            for (unsigned i = 0; i < n; i++) {
                if (snap[i].alive && !midi_subscription_live(snap[i].client, snap[i].port))
                    midi_sender_note_dead(snap[i].client, snap[i].port);
                else if (!snap[i].alive) midi_sender_revive(snap[i].name);
            }
        }
        if (snd_seq_event_input_pending(seq, 0) > 0) {
            /* ALSA can buffer several events in one kernel read. */
        } else if (nfds > 0) {
            snd_seq_poll_descriptors(seq, fds, (unsigned)nfds, POLLIN);
            int ready = poll(fds, (nfds_t)nfds, 10);
            if (ready <= 0) continue;
        } else {
            struct timespec pause = {.tv_nsec = 10000000};
            nanosleep(&pause, NULL);
            continue;
        }
        /* Single consumer: poll-guarded reads never block, so shutdown
           always reaches the running flag above. */
        for (;;) {
            for (int i = 0; i < nfds; i++) { fds[i].revents = 0; }
            snd_seq_poll_descriptors(seq, fds, (unsigned)nfds, POLLIN);
            if (snd_seq_event_input_pending(seq, 0) <= 0 &&
                poll(fds, (nfds_t)nfds, 0) <= 0) break;
            snd_seq_event_t *event = NULL;
            if (snd_seq_event_input(seq, &event) < 0 || !event) break;
            double at = audio_time_now();
            if (event->type == SND_SEQ_EVENT_NOTEON)
                midi_input_event(0, event->data.note.channel, event->data.note.note, event->data.note.velocity, at,
                    event->source.client, event->source.port);
            else if (event->type == SND_SEQ_EVENT_NOTEOFF)
                midi_input_event(0, event->data.note.channel, event->data.note.note, 0, at,
                    event->source.client, event->source.port);
            else if (event->type == SND_SEQ_EVENT_CONTROLLER)
                midi_input_event(1, event->data.control.channel, event->data.control.param, event->data.control.value, at,
                    event->source.client, event->source.port);
            else if (event->type == SND_SEQ_EVENT_PORT_EXIT || event->type == SND_SEQ_EVENT_CLIENT_EXIT ||
                event->type == SND_SEQ_EVENT_PORT_UNSUBSCRIBED || event->type == SND_SEQ_EVENT_PORT_START ||
                event->type == SND_SEQ_EVENT_CLIENT_START)
                midi_announce_event(event);
        }
    }
    free(fds);
    return NULL;
}
