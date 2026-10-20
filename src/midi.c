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
    int rc = snd_seq_open(&seq, "default", SND_SEQ_OPEN_OUTPUT, 0);
    if (rc != 0 || !seq) {
        log_console("midi: ALSA sequencer unavailable");
        seq = NULL;
    } else {
        snd_seq_set_client_name(seq, "tompouce");
        seq_port = snd_seq_create_simple_port(seq, "out",
            SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
            SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        if (seq_port < 0) {
            log_console("midi: port creation failed");
            snd_seq_close(seq);
            seq = NULL;
        }
    }
    /* The scheduler thread runs regardless so note-off timing stays
       observable (and audible nowhere) without hardware. */
    midi_ok = seq != NULL;
    midi_running = 1;
    pthread_create(&midi_tid, NULL, midi_worker, NULL);
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
