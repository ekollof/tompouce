#include "midi.h"
#include "common.h"
#include "audio.h"
#include <alsa/asoundlib.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>

#define MIDI_MAX_OFFS 1024

static pthread_mutex_t midi_in_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t midi_in_tid;
static int midi_in_running;
static void *midi_input_worker(void *arg);

typedef struct { double at; int ch; int note; } OffEv;


/* Bounded scheduling observation for tests. Recording is independent of
   hardware: note on/off intent is captured whether or not ALSA accepts
   bytes, so routing stays verifiable without a sequencer. */
static MidiCapturedNote midi_captured[MIDI_CAPTURE_CAPACITY];
static unsigned midi_captured_count;
static int midi_capture_enabled;
void midi_capture_enable(int on) {
    midi_capture_enabled = on ? 1 : 0;
    if (on) midi_captured_count = 0;
}
unsigned midi_captured_notes(MidiCapturedNote *out, unsigned capacity) {
    unsigned count = midi_captured_count < capacity ? midi_captured_count : capacity;
    for (unsigned i = 0; i < count; i++) out[i] = midi_captured[i];
    return midi_captured_count;
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
            if (midi_capture_enabled && midi_captured_count<MIDI_CAPTURE_CAPACITY) {
                midi_captured[midi_captured_count++]=(MidiCapturedNote){
                    .on=0,.chan=e.ch,.note=e.note,.vel=64,.at=e.at};
            }
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
    pthread_create(&midi_tid, NULL, midi_worker, NULL);
    if (seq) {
        midi_in_running = 1;
        pthread_create(&midi_in_tid, NULL, midi_input_worker, NULL);
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
    if (seq) pthread_join(midi_in_tid, NULL);
    pthread_mutex_lock(&midi_lock);
    if (seq) { snd_seq_close(seq); seq = NULL; }
    midi_ok = 0;
    pthread_mutex_unlock(&midi_lock);
}

int midi_ready(void) { return midi_ok; }

void midi_note(int chan, int note, int vel, int dur_ms) {
    if (chan < 0 || chan > 15 || note < 0 || note > 127) return;
    if (vel < 1) vel = 1;
    if (vel > 127) vel = 127;
    if (dur_ms < 50) dur_ms = 50;
    if (midi_capture_enabled && midi_captured_count<MIDI_CAPTURE_CAPACITY) {
        midi_captured[midi_captured_count++]=(MidiCapturedNote){
            .on=1,.chan=chan,.note=note,.vel=vel,.at=audio_time_now()};
    }
    pthread_mutex_lock(&midi_lock);
    if (!midi_running) { pthread_mutex_unlock(&midi_lock); return; }
    if (midi_ok) send_raw(1, chan, note, vel);
    if (n_offs < MIDI_MAX_OFFS) {
        OffEv e;
        e.at = audio_time_now() + dur_ms / 1000.0;
        e.ch = chan;
        e.note = note;
        int i = n_offs;
        while (i > 0 && offs[i - 1].at > e.at) { offs[i] = offs[i - 1]; i--; }
        offs[i] = e;
        n_offs++;
        pthread_cond_signal(&midi_cond);
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

static void midi_input_event(int type, int chan, int p1, int p2, double at) {
    if (chan < 0 || chan > 15) return;
    pthread_mutex_lock(&midi_in_lock);
    if (type == 1) {
        double value = fmax(0, fmin(127, p2)) / 127.0;
        if (p1 >= 0 && p1 < 128) {
            midi_cc_all[p1] = value;
            midi_cc_chan[chan][p1] = value;
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
unsigned midi_take_keys(MidiHeldKey *out, unsigned capacity, double horizon) {
    unsigned count = 0;
    pthread_mutex_lock(&midi_in_lock);
    for (unsigned i = 0; i < midi_held_count && count < capacity; i++) {
        if (!midi_held[i].consumed && midi_held[i].at <= horizon) {
            midi_held[i].consumed = 1;
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

typedef struct { int client, port; } MidiSender;
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

/* Forms: "client:port", numeric client id, device-name substring, or an
   ordinal index. Returns 1 and subscribes our input when found. */
static int midi_subscribe_sender(const char *name, int ordinal) {
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
    for (unsigned i = 0; i < midi_sender_count; i++)
        if (midi_senders[i].client == found[pick].client && midi_senders[i].port == found[pick].port) return 1;
    if (midi_sender_count >= MIDI_SENDERS) return 0;
    snd_seq_addr_t src = {(unsigned)found[pick].client, (unsigned)found[pick].port};
    snd_seq_addr_t dst = {(unsigned)snd_seq_client_id(seq), (unsigned)seq_port};
    snd_seq_port_subscribe_t *subs;
    snd_seq_port_subscribe_alloca(&subs);
    snd_seq_port_subscribe_set_sender(subs, &src);
    snd_seq_port_subscribe_set_dest(subs, &dst);
    { int rc=snd_seq_subscribe_port(seq, subs);
      if (rc) return 0; }
    midi_senders[midi_sender_count++] = found[pick];
    return 1;
}

static int midi_has_live_sender(void) {
    if (!seq) return 0;
    snd_seq_query_subscribe_t *query;
    snd_seq_query_subscribe_alloca(&query);
    snd_seq_addr_t me = {(unsigned)snd_seq_client_id(seq), (unsigned)seq_port};
    snd_seq_query_subscribe_set_root(query, &me);
    snd_seq_query_subscribe_set_type(query, SND_SEQ_QUERY_SUBS_WRITE);
    snd_seq_query_subscribe_set_index(query, 0);
    return snd_seq_query_port_subscribers(seq, query) >= 0;
}

/* Open a MIDI sender for input, best effort like the web's waiting state.
   Name forms follow midi_subscribe_sender; a negative ordinal selects the
   Nth sender for numeric input. Returns 1 when events can arrive. */
int midi_open_input(const char *name, int ordinal) {
    if (!seq) {
        log_console("midi: ALSA sequencer unavailable");
        return 0;
    }
    if (midi_subscribe_sender(name, ordinal)) return 1;
    if (midi_has_live_sender()) return 1;
    if (name && *name) log_console("midi: input '%s' not found (connect with aconnect)", name);
    else log_console("midi: no input sender found (connect with aconnect)");
    return 0;
}

int midi_input_live(void) {
    return midi_has_live_sender();
}

/* Deterministic test injection: writes the input stores as if the worker
   parsed an ALSA event, without needing hardware. */
void midi_test_inject(int type, int chan, int p1, int p2) {
    midi_input_event(type, chan, p1, p2, audio_time_now());
}

static void *midi_input_worker(void *arg) {
    (void)arg;
    int nfds = snd_seq_poll_descriptors_count(seq, POLLIN);
    struct pollfd *fds = calloc(nfds > 0 ? (size_t)nfds : 1, sizeof(*fds));
    if (!fds) return NULL;
    while (midi_in_running) {
        if (nfds > 0) {
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
            int ready = poll(fds, (nfds_t)nfds, 0);
            if (ready <= 0) break;
            snd_seq_event_t *event = NULL;
            if (snd_seq_event_input(seq, &event) < 0 || !event) break;
            double at = audio_time_now();
            if (event->type == SND_SEQ_EVENT_NOTEON)
                midi_input_event(0, event->data.note.channel, event->data.note.note, event->data.note.velocity, at);
            else if (event->type == SND_SEQ_EVENT_NOTEOFF)
                midi_input_event(0, event->data.note.channel, event->data.note.note, 0, at);
            else if (event->type == SND_SEQ_EVENT_CONTROLLER)
                midi_input_event(1, event->data.control.channel, event->data.control.param, event->data.control.value, at);
        }
    }
    free(fds);
    return NULL;
}
