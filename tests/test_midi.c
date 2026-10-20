/* Dominant MIDI routing: commit flags plus threaded scheduler-to-ALSA
   delivery. The capture ring observes scheduling intent with or without
   ALSA hardware, so this runs anywhere. */
#include "../src/audio.c"
#include "../src/scheduler.c"
#include "midi.h"
#include <assert.h>
#include <alsa/asoundlib.h>
#include <sys/stat.h>
#include <time.h>

static double monotonic(void) {
    struct timespec now; assert(!clock_gettime(CLOCK_MONOTONIC,&now));
    return now.tv_sec+now.tv_nsec/1e9;
}

static void isolate_environment(void) {
    char root[]="/tmp/tompouce-midi-XXXXXX";
    assert(mkdtemp(root));
    char banks[512], cache[512];
    assert(snprintf(banks,sizeof(banks),"%s/banks",root)>0);
    assert(snprintf(cache,sizeof(cache),"%s/cache",root)>0);
    assert(!mkdir(banks,0700) && !mkdir(cache,0700));
    assert(setenv("XDG_CONFIG_HOME",root,1)==0);
    assert(setenv("XDG_CACHE_HOME",cache,1)==0);
    assert(setenv("TOMPOUCE_BANKS",banks,1)==0);
}

typedef struct { int midi_only, chan, note, osc_only, csound_only; double gain; } CommittedNote;

static int collect_committed(CommittedNote *out, int capacity) {
    int count=0;
    for (QNode *node=qhead;node && count<capacity;node=node->next) {
        if (node->ev.control_only || node->ev.eviction_only) continue;
        out[count++]=(CommittedNote){node->ev.midi_only,node->ev.midi_chan,node->ev.midi_note,
            node->ev.osc_only,node->ev.csound_only,node->ev.gain};
    }
    return count;
}

static void schedule_loaded(int want_depth) {
    unsigned generation=event_queue_generation();
    assert(js_audio_schedule(tracks[0].javascript,0,0,generation));
    /* Immediate (MIDI/OSC/Csound/rest) events commit synchronously; source
       haps need their preparation drained. */
    for (int i=0;i<2000 && event_queue_depth()<want_depth;i++) {
        if (!prepare_queued_audio(0)) {
            struct timespec pause={.tv_nsec=1000000}; nanosleep(&pause,NULL);
        }
    }
}

static void test_commit_routing(void) {
    CommittedNote notes[16];
    event_queue_clear(); audio_time=0; audio_toggle_play(1);
    assert(prep_pool_start(0,0));
#ifdef HAVE_CSOUND
    /* First: engine creation happens in a quiet process (worker spawn timing
       under sanitizers degrades with heap churn). */
    fprintf(stderr,"DBG plain-csound\n");
    assert(track_load_from_code("await loadCsound`instr ComboTone\\na1 oscili p5,p4\\nout a1,a1\\nendin`; note('c3')"));
    fprintf(stderr,"DBG plain-csound ok\n");
    assert(track_load_from_code("await loadCsound`instr ComboTone\\na1 oscili p5,p4\\nout a1,a1\\nendin`; note('c3').csound('ComboTone')"));
    schedule_loaded(1);
    event_queue_clear();
    assert(track_load_from_code("note(60).midi().csound('ComboTone')"));
    schedule_loaded(1);
    assert(collect_committed(notes,16)==1);
    assert(notes[0].midi_only && notes[0].csound_only && !notes[0].osc_only);
    assert(notes[0].chan==0 && notes[0].note==60);
    event_queue_clear();
#endif
    assert(track_load_from_code("note(\"60 62\").midi()"));
    schedule_loaded(2);
    assert(collect_committed(notes,16)==2);
    for (int i=0;i<2;i++) {
        assert(notes[i].midi_only && notes[i].chan==0 && !notes[i].osc_only && !notes[i].csound_only);
        assert(notes[i].note==60+i*2);
    }
    event_queue_clear();
    assert(track_load_from_code("note(60).midichan(3).midi()"));
    schedule_loaded(1);
    assert(collect_committed(notes,16)==1);
    assert(notes[0].midi_only && notes[0].chan==2 && notes[0].note==60);
    event_queue_clear();
    assert(track_load_from_code("note(60).s('sine').midi()"));
    schedule_loaded(1);
    assert(collect_committed(notes,16)==1);
    assert(notes[0].midi_only && notes[0].note==60);
    event_queue_clear();
    assert(track_load_from_code("note(60).s('sine').midichan(1)"));
    schedule_loaded(1);
    assert(collect_committed(notes,16)==1);
    assert(!notes[0].midi_only && notes[0].chan==0 && notes[0].note==60);
    event_queue_clear();
    assert(track_load_from_code("note(60).midi().osc()"));
    schedule_loaded(1);
    assert(collect_committed(notes,16)==1);
    assert(notes[0].midi_only && notes[0].osc_only && !notes[0].csound_only);
    event_queue_clear();
    assert(track_load_from_code("note(60).gain(1).velocity(.5).midi()"));
    schedule_loaded(1);
    assert(collect_committed(notes,16)==1);
    assert(notes[0].midi_only && fabs(notes[0].gain-.5)<1e-12);
    event_queue_clear();
    assert(track_load_from_code("note(60).midi({midichannel: 4})"));
    schedule_loaded(1);
    assert(collect_committed(notes,16)==1);
    assert(notes[0].midi_only && notes[0].chan==3);
    event_queue_clear();
    assert(track_load_from_code("s('~').midi()"));
    schedule_loaded(0);
    assert(collect_committed(notes,16)==0);
    event_queue_clear();
    assert(!track_load_from_code("note(60).midi(note(61))"));
    event_queue_clear();
    puts("midi commit: dominance flags, channels, combos, rests and Pattern rejection ok");
}

static int wait_captured(int want_ons, double timeout_s, int *peak_voices) {
    static float spin[256*2];
    double end=monotonic()+timeout_s;
    static MidiCapturedNote seen[512];
    for (;;) {
        pa_callback(NULL,spin,128,NULL,0,NULL);
        if (peak_voices) {
            int live=audio_active_voices();
            if (live>*peak_voices) *peak_voices=live;
        }
        unsigned total=midi_captured_notes(seen,512);
        int ons=0;
        for (unsigned i=0;i<total && i<512;i++) ons+=seen[i].on!=0;
        if (ons>=want_ons) return 1;
        if (monotonic()>=end) return 0;
    }
}

static void test_threaded_delivery(void) {
    midi_init();
    midi_capture_enable(1);
    scheduler_init();
    audio_toggle_play(1);
    int peak_voices=0;
    static float pump[256*2];
    assert(track_load_from_code("note(\"60 62 64\").gain(1).velocity(.5).midi()"));
    assert(wait_captured(6,30,&peak_voices));
    for (int i=0;i<500;i++) {
        pa_callback(NULL,pump,128,NULL,0,NULL);
        int live=audio_active_voices();
        if (live>peak_voices) peak_voices=live;
    }
    assert(peak_voices==0);
    MidiCapturedNote seen[512];
    unsigned total=midi_captured_notes(seen,512);
    /* The transport can join mid-cycle, so onsets follow the pattern rotation
       rather than starting at its head. */
    int rotation[6]={-1,-1,-1,-1,-1,-1},gathered=0;
    for (unsigned i=0;i<total && i<512 && gathered<6;i++) {
        if (!seen[i].on) continue;
        assert(seen[i].chan==0 && seen[i].vel==63);
        assert(seen[i].note==60 || seen[i].note==62 || seen[i].note==64);
        rotation[gathered++]=seen[i].note;
    }
    assert(gathered==6);
    for (int i=1;i<6;i++) {
        int want=rotation[i-1]==64?60:rotation[i-1]+2;
        assert(rotation[i]==want);
    }
    /* Every on is eventually paired with its off. */
    int cursor=0;
    for (int r=0;r<6;r++) {
        int off=0;
        for (unsigned i=(unsigned)cursor;i<total && i<512;i++)
            if (!seen[i].on && seen[i].note==rotation[r]) { off=1; cursor=(int)i+1; break; }
        assert(off);
    }
    /* Legacy local-plus-ALSA path still sounds and emits. */
    assert(track_load_from_code("note(67).s('sine').midichan(2)"));
    int found67=0;
    double end67=monotonic()+30;
    static MidiCapturedNote seen67[512];
    for (;;) {
        pa_callback(NULL,pump,128,NULL,0,NULL);
        unsigned total67=midi_captured_notes(seen67,512);
        for (unsigned i=0;i<total67 && i<512;i++)
            if (seen67[i].on && seen67[i].chan==1 && seen67[i].note==67) found67=1;
        if (found67) break;
        assert(monotonic()<end67);
    }
    int sounded=0;
    for (int i=0;i<2000 && !sounded;i++) {
        pa_callback(NULL,pump,128,NULL,0,NULL);
        if (audio_active_voices()>0) sounded=1;
    }
    assert(sounded);
    assert(found67);
    scheduler_shutdown();
    midi_capture_enable(0);
    puts("midi delivery: ordered notes with velocity, no local voices, legacy path intact");
}

static void test_midi_input_stores(void) {
    MidiHeldKey keys[300];
    assert(midi_cc_value(120,0)==0 && midi_cc_value(120,3)==0);
    midi_test_inject(1,2,120,64);
    assert(fabs(midi_cc_value(120,0)-64/127.0)<1e-12);
    assert(fabs(midi_cc_value(120,3)-64/127.0)<1e-12);
    assert(midi_cc_value(120,4)==0);
    assert(midi_cc_value(120,99)==0);
    midi_test_inject(1,5,120,127);
    assert(fabs(midi_cc_value(120,0)-1)<1e-12);
    assert(fabs(midi_cc_value(120,6)-1)<1e-12);
    assert(fabs(midi_cc_value(120,3)-64/127.0)<1e-12);
    midi_test_inject(1,99,120,100);
    midi_test_inject(1,0,200,100);
    assert(fabs(midi_cc_value(120,0)-1)<1e-12);
    midi_test_inject(0,0,60,0);
    assert(midi_take_keys(keys,64,-1)==0);
    midi_test_inject(0,0,60,100);
    midi_test_inject(0,1,62,127);
    assert(midi_take_keys(keys,64,1e12)==2);
    assert(keys[0].note==60 && keys[0].chan==1 && fabs(keys[0].vel-100/127.0)<1e-12);
    assert(keys[1].note==62 && keys[1].chan==2 && keys[1].vel==1);
    assert(midi_take_keys(keys,64,1e12)==0);
    midi_test_inject(0,0,64,100);
    assert(midi_take_keys(keys,64,-1)==0);
    assert(midi_take_keys(keys,64,1e12)==1 && keys[0].note==64);
    for (int i=0;i<301;i++) midi_test_inject(0,0,i%128,100);
    assert(midi_take_keys(keys,300,1e12)==256 && keys[0].note==45 && keys[255].note==44);
    puts("midi input stores: merged/per-channel CC, consume-once keys, bounds ok");
}

/* MIDI controller input through real evaluation and queries. CC readers
   follow injected values across cycles; keyboard patterns emit held notes
   once with fixed durations and channel attribution. */
static double committed_lpf(void) {
    for (QNode *node=qhead;node;node=node->next) {
        if (node->ev.control_only || node->ev.eviction_only) continue;
        return node->ev.dsp.p[DSP_LPF];
    }
    return NAN;
}

static void test_midi_input_js(void) {
    event_queue_clear(); audio_time=0; audio_toggle_play(1);
    assert(track_load_from_code("const cc = await midin('no-such-device-xyz'); note(60).s('sine').lpf(cc(7).range(0,1000))"));
    schedule_loaded(1);
    assert(fabs(committed_lpf()-0)<1e-9);
    midi_test_inject(1,0,7,64);
    event_queue_clear();
    assert(track_load_from_code("const cc = await midin('no-such-device-xyz'); note(60).s('sine').lpf(cc(7).range(0,1000))"));
    schedule_loaded(1);
    assert(fabs(committed_lpf()-1000*64/127.0)<1e-6);
    midi_test_inject(1,2,8,127);
    event_queue_clear();
    assert(track_load_from_code("const cc = await midin('no-such-device-xyz'); note(60).s('sine').lpf(cc(8,3).range(0,100))"));
    schedule_loaded(1);
    assert(fabs(committed_lpf()-100)<1e-6);
    event_queue_clear();
    assert(!track_load_from_code("await midin(note(60))"));
    assert(!track_load_from_code("await midikeys('no-such-device-xyz')"));
    assert(!track_load_from_code("await midikeys(note(60))"));
    event_queue_clear();
    puts("midi input JS: live CC reads, channels, Pattern rejection, missing-device errors ok");
}

/* Persisted CC state without hardware: injection through a named sender
   writes the sender file (zeros included); ordinal/unnamed senders never
   persist; loading restores reader defaults. */
static void test_midi_persist(void) {
    midi_test_add_sender(41, 0, "PersistBox:0", 1);
    midi_test_add_sender(42, 0, "OtherBox:0", 0);
    midi_test_inject_from(41, 0, 1, 2, 7, 64);
    assert(fabs(midi_cc_value(7, 0) - 64 / 127.0) < 1e-12);
    midi_test_inject_from(41, 0, 1, 2, 9, 0);
    assert(midi_cc_value(9, 0) == 0);
    midi_test_inject_from(42, 0, 1, 2, 11, 100);
    assert(midi_state_load("OtherBox:0") == 0);
    /* Dirty the live stores without saving, then reload: values and zeros
       both come back from the sender file. */
    midi_test_inject(1, 2, 7, 100);
    midi_test_inject(1, 2, 9, 100);
    assert(fabs(midi_cc_value(7, 0) - 100 / 127.0) < 1e-12);
    int loaded = midi_state_load("PersistBox");
    assert(loaded == 4);
    assert(fabs(midi_cc_value(7, 0) - 64 / 127.0) < 1e-12);
    assert(fabs(midi_cc_value(7, 3) - 64 / 127.0) < 1e-12);
    assert(midi_cc_value(9, 0) == 0);
    assert(midi_state_load("") == -1);
    assert(midi_state_load("a/b") == 0);
    assert(midi_restore_sender("PersistBox") == 0);
    puts("midi persist: named-sender files, zeros, ordinal exclusion, load-as-defaults ok");
}

/* Reconnect matching without hardware: exit marks senders dead, unrelated
   traffic is ignored, ordinal senders never revive by name. */
static void test_midi_reconnect(void) {
    midi_test_add_sender(51, 0, "ReBox:0", 1);
    midi_test_add_sender(52, 0, "OrdBox:0", 0);
    assert(midi_test_sender_alive(51, 0) && midi_test_sender_alive(52, 0));
    assert(midi_test_announce(0, 99, 9, NULL) == 0);
    assert(midi_test_sender_alive(51, 0));
    assert(midi_test_announce(0, 51, 0, NULL) == 1);
    assert(!midi_test_sender_alive(51, 0) && midi_test_sender_alive(52, 0));
    assert(midi_test_announce(0, 51, 0, NULL) == 0);
    assert(midi_test_announce(1, 0, 0, "Nope") == 0);
    assert(midi_test_announce(1, 0, 0, "OrdBox") == 0);
    assert(midi_test_announce(1, 0, 0, "ReBox") == 1);
    assert(!midi_test_sender_alive(51, 0));
    assert(midi_test_announce(0, 52, 0, NULL) == 1);
    assert(!midi_test_sender_alive(52, 0));
    puts("midi reconnect: dead-marking, name gating, ordinal exclusion ok");
}

/* Keyboard commit through injected arrivals: held notes voice once with
   channel attribution, then consume so re-queries stay silent. Requires a
   live sender (see the loopback test below); skipped without one. */
static int loopback_sender_ready;
static snd_seq_t *loopback_sender;
static void test_midi_keys_commit(void) {
    if (!loopback_sender_ready) {
        puts("SKIP midi keys commit: no ALSA sequencer");
        return;
    }
    event_queue_clear(); audio_time=0; audio_toggle_play(1);
    midi_test_inject(0,3,69,110);
    assert(track_load_from_code("const kb = await midikeys('test-none-xyz'); kb().s('saw')"));
    schedule_loaded(1);
    int voices=0;
    for (QNode *node=qhead;node;node=node->next) {
        if (node->ev.control_only || node->ev.eviction_only) continue;
        voices++;
        assert(fabs(node->ev.freq-440*pow(2,(69-69)/12.0))<1e-9);
        assert(node->ev.midi_chan==3);
        assert(fabs(node->ev.gain-.8*110/127.0)<1e-9);
    }
    assert(voices==1);
    event_queue_clear();
    assert(track_load_from_code("const kb = await midikeys('test-none-xyz'); kb().s('saw')"));
    schedule_loaded(0);
    voices=0;
    for (QNode *node=qhead;node;node=node->next)
        if (!node->ev.control_only && !node->ev.eviction_only) voices++;
    assert(voices==0);
    event_queue_clear();
    if (loopback_sender) { snd_seq_close(loopback_sender); loopback_sender=NULL; }
    puts("midi keys commit: held notes voice once with channel attribution");
}

/* Full hardware loop for restore and reconnect: a duplex sender with a
   source port and a writable sink port. CCs cross the real worker into the
   sender file; deleting the source port marks it dead; recreating it
   resubscribes by client name and restores the CCs to the sink. Skipped
   gracefully without a sequencer. */
/* Collect controller triples for up to timeout_s; returns how many of the
   two wanted triples arrived (order-independent). */
static int drain_controllers(snd_seq_t *handle, int ch0, int cc0, int v0, int ch1, int cc1, int v1,
    double timeout_s) {
    int nfds = snd_seq_poll_descriptors_count(handle, POLLIN);
    struct pollfd fds[8];
    if (nfds > 8) nfds = 8;
    double end = monotonic() + timeout_s;
    int found0 = 0, found1 = 0;
    for (;;) {
        if (nfds > 0) {
            snd_seq_poll_descriptors(handle, fds, (unsigned)nfds, POLLIN);
            if (poll(fds, (nfds_t)nfds, 50) <= 0) {
                if ((found0 && found1) || monotonic() >= end) return found0 + found1;
                continue;
            }
        } else if (monotonic() >= end) return found0 + found1;
        snd_seq_event_t *event = NULL;
        if (snd_seq_event_input(handle, &event) < 0 || !event) {
            if ((found0 && found1) || monotonic() >= end) return found0 + found1;
            continue;
        }
        if (event->type != SND_SEQ_EVENT_CONTROLLER) continue;
        int ch = event->data.control.channel, cc = (int)event->data.control.param,
            v = event->data.control.value;
        if (ch == ch0 && cc == cc0 && v == v0) found0 = 1;
        if (ch == ch1 && cc == cc1 && v == v1) found1 = 1;
        if (found0 && found1) return 2;
    }
}

static void test_midi_restore_loopback(void) {
    snd_seq_t *sender = NULL;
    if (snd_seq_open(&sender, "default", SND_SEQ_OPEN_DUPLEX, 0) < 0 || !sender) {
        puts("SKIP midi restore loopback: no ALSA sequencer");
        return;
    }
    snd_seq_set_client_name(sender, "tompouce-midi-restore");
    int sport = snd_seq_create_simple_port(sender, "src",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    int sink = snd_seq_create_simple_port(sender, "sink",
        SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (sport < 0 || sink < 0) { snd_seq_close(sender); puts("SKIP midi restore loopback: no ports"); return; }
    assert(midi_open_input("tompouce-midi-restore", -1));
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_controller(&ev, 0, 21, 80);
    snd_seq_ev_set_source(&ev, sport);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    assert(snd_seq_event_output_direct(sender, &ev) >= 0);
    /* Back-to-back direct sends can drop sequencer events; pace them. */
    { struct timespec pause = {.tv_nsec = 100000000};
      nanosleep(&pause, NULL); }
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_controller(&ev, 1, 22, 100);
    snd_seq_ev_set_source(&ev, sport);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    assert(snd_seq_event_output_direct(sender, &ev) >= 0);
    double end = monotonic() + 5;
    while ((fabs(midi_cc_value(21, 0) - 80 / 127.0) >= 1e-12 ||
        fabs(midi_cc_value(22, 0) - 100 / 127.0) >= 1e-12) && monotonic() < end) {
        struct timespec pause = {.tv_nsec = 5000000};
        nanosleep(&pause, NULL);
    }
    assert(fabs(midi_cc_value(21, 0) - 80 / 127.0) < 1e-12);
    assert(midi_state_load("tompouce-midi-restore") == 4);
    /* Drop the source port: the announce path must mark the sender dead. */
    assert(snd_seq_delete_simple_port(sender, sport) >= 0);
    end = monotonic() + 5;
    int dead = 0;
    while (monotonic() < end) {
        if (!midi_input_live()) { dead = 1; break; }
        struct timespec pause = {.tv_nsec = 5000000};
        nanosleep(&pause, NULL);
    }
    assert(dead);
    /* Recreate under the same client name (possibly a new port number): the
       revive path resubscribes by name and restores both CCs to the sink. */
    sport = snd_seq_create_simple_port(sender, "src",
        SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    assert(sport >= 0);
    int got = drain_controllers(sender, 0, 21, 80, 1, 22, 100, 10);
    assert(got == 2);
    assert(midi_input_live());
    snd_seq_close(sender);
    puts("midi restore loopback: persist, disconnect, reconnect and CC restore ok");
}

/* Full hardware loop: a local ALSA sender feeds the real input thread and
   subscription path. Skipped gracefully without a sequencer. */
static void test_midi_loopback(void) {
    snd_seq_t *sender=NULL;
    if (snd_seq_open(&sender,"default",SND_SEQ_OPEN_OUTPUT,0)<0 || !sender) {
        puts("SKIP midi loopback: no ALSA sequencer");
        return;
    }
    snd_seq_set_client_name(sender,"tompouce-midi-loopback");
    int sport=snd_seq_create_simple_port(sender,"src",
        SND_SEQ_PORT_CAP_READ|SND_SEQ_PORT_CAP_SUBS_READ,
        SND_SEQ_PORT_TYPE_MIDI_GENERIC|SND_SEQ_PORT_TYPE_APPLICATION);
    if (sport<0) { snd_seq_close(sender); puts("SKIP midi loopback: no sender port"); return; }
    event_queue_clear(); audio_time=0; audio_toggle_play(1);
    assert(track_load_from_code("const cc = await midin('tompouce-midi-loopback'); note(60)"));
    assert(midi_open_input("tompouce-midi-loopback",-1));

    assert(track_load_from_code(
        "const cc = await midin('tompouce-midi-loopback');"
        "const kb = await midikeys('tompouce-midi-loopback');"
        "stack(note(60).s('sine').lpf(cc(20).range(0,1000)), kb().s('saw'))"));
    schedule_loaded(2);
    assert(fabs(committed_lpf()-0)<1e-9);
    snd_seq_event_t ev;
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_controller(&ev,0,20,96);
    snd_seq_ev_set_source(&ev,sport);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    assert(snd_seq_event_output_direct(sender,&ev)>=0);
    double end=monotonic()+5;
    while (fabs(midi_cc_value(20,0)-96/127.0)>=1e-12 && monotonic()<end) {
        struct timespec pause={.tv_nsec=5000000}; nanosleep(&pause,NULL);
    }
    assert(fabs(midi_cc_value(20,0)-96/127.0)<1e-12);
    assert(fabs(midi_cc_value(20,1)-96/127.0)<1e-12);
    event_queue_clear();
    assert(track_load_from_code(
        "const cc = await midin('tompouce-midi-loopback');"
        "note(60).s('sine').lpf(cc(20).range(0,1000))"));
    schedule_loaded(1);
    assert(fabs(committed_lpf()-1000*96/127.0)<1e-6);
    snd_seq_ev_clear(&ev);
    snd_seq_ev_set_noteon(&ev,1,67,100);
    snd_seq_ev_set_source(&ev,sport);
    snd_seq_ev_set_subs(&ev);
    snd_seq_ev_set_direct(&ev);
    assert(snd_seq_event_output_direct(sender,&ev)>=0);
    end=monotonic()+5;
    int keys=0;
    MidiHeldKey held[64];
    while (monotonic()<end) {
        if (midi_take_keys(held,64,1e12)>0) { keys=1; break; }
        struct timespec pause={.tv_nsec=5000000}; nanosleep(&pause,NULL);
    }
    assert(keys && held[0].note==67 && held[0].chan==2 && fabs(held[0].vel-100/127.0)<1e-12);
    /* The sender stays open so later keyboard tests have a live device. */
    loopback_sender_ready=1;
    loopback_sender=sender;
    puts("midi loopback: CC and keyboard notes cross real ALSA ports");
}

/* Dominant MIDI extras: commit parsing, web-ordered validation with
   throw-abort parity, and control-only delivery without notes. */

typedef struct {
    int midi_only, chan, note, osc_only, csound_only;
    int has_cc, cc, ccv, has_prog, prog, has_bend, bend, has_touch, touch;
    int has_sysex, id_len, data_len;
    unsigned char id[3], data[8];
    int cmd, cycle_start;
} MidiExtra;

static int collect_extras(MidiExtra *out, int capacity) {
    int count = 0;
    for (QNode *node = qhead; node && count < capacity; node = node->next) {
        if (node->ev.control_only || node->ev.eviction_only || !node->ev.midi_only) continue;
        MidiExtra e;
        memset(&e, 0, sizeof(e));
        e.midi_only = node->ev.midi_only; e.chan = node->ev.midi_chan; e.note = node->ev.midi_note;
        e.osc_only = node->ev.osc_only; e.csound_only = node->ev.csound_only;
        e.has_cc = node->ev.midi_has_cc; e.cc = node->ev.midi_cc; e.ccv = node->ev.midi_ccv;
        e.has_prog = node->ev.midi_has_prog; e.prog = node->ev.midi_prog;
        e.has_bend = node->ev.midi_has_bend; e.bend = node->ev.midi_bend;
        e.has_touch = node->ev.midi_has_touch; e.touch = node->ev.midi_touch;
        e.has_sysex = node->ev.midi_has_sysex;
        e.id_len = node->ev.midi_sysex_id_len; e.data_len = node->ev.midi_sysex_len;
        if (e.id_len > 3) e.id_len = 3;
        if (e.data_len > 8) e.data_len = 8;
        memcpy(e.id, node->ev.midi_sysex_id, (size_t)e.id_len);
        memcpy(e.data, node->ev.midi_sysex, (size_t)e.data_len);
        e.cmd = node->ev.midi_cmd; e.cycle_start = node->ev.midi_cycle_start;
        out[count++] = e;
    }
    return count;
}

static void test_midi_extras_commit(void) {
    MidiExtra extras[16];
    event_queue_clear(); audio_time = 0; audio_toggle_play(1);
    assert(prep_pool_start(0, 0));
    assert(track_load_from_code("note(60).ccn(74).ccv(0.5).midi()"));
    schedule_loaded(1);
    assert(collect_extras(extras, 16) == 1);
    assert(extras[0].note == 60 && extras[0].has_cc && extras[0].cc == 74 && extras[0].ccv == 64);
    event_queue_clear();
    /* Control-only haps carry extras without a note, like the web output. */
    assert(track_load_from_code("ccn(74).ccv(0.5).midi()"));
    schedule_loaded(1);
    assert(collect_extras(extras, 16) == 1);
    assert(extras[0].note < 0 && extras[0].has_cc && extras[0].ccv == 64);
    event_queue_clear();
    assert(track_load_from_code("note(60).progNum(10).midibend(0.25).miditouch(0.75).midi()"));
    schedule_loaded(1);
    assert(collect_extras(extras, 16) == 1);
    assert(extras[0].has_prog && extras[0].prog == 10);
    assert(extras[0].has_bend && extras[0].bend == 2047);
    assert(extras[0].has_touch && extras[0].touch == 95);
    event_queue_clear();
    assert(track_load_from_code("note(60).sysex([0x43, [0x79, 0x09]]).midi()"));
    schedule_loaded(1);
    assert(collect_extras(extras, 16) == 1);
    assert(extras[0].has_sysex && extras[0].id_len == 1 && extras[0].id[0] == 0x43);
    assert(extras[0].data_len == 2 && extras[0].data[0] == 0x79 && extras[0].data[1] == 0x09);
    event_queue_clear();
    /* String sysex forms never send on the web; the note still commits. */
    assert(track_load_from_code("note(60).sysexid(\"0x77\").sysexdata(\"0x01\").midi()"));
    schedule_loaded(1);
    assert(collect_extras(extras, 16) == 1);
    assert(extras[0].note == 60 && !extras[0].has_sysex);
    event_queue_clear();
    /* Out-of-range program aborts later extras but keeps the note. */
    assert(track_load_from_code("note(60).progNum(200).ccn(74).ccv(0.5).midi()"));
    schedule_loaded(1);
    assert(collect_extras(extras, 16) == 1);
    assert(extras[0].note == 60 && !extras[0].has_prog && !extras[0].has_cc);
    event_queue_clear();
    assert(track_load_from_code("note(60).ccn(74).ccv(64).midibend(0.5).midi()"));
    schedule_loaded(1);
    assert(collect_extras(extras, 16) == 1);
    assert(extras[0].note == 60 && !extras[0].has_cc && !extras[0].has_bend);
    event_queue_clear();
    /* A lone CC number without a value sends nothing extra. */
    assert(track_load_from_code("note(60).ccn(74).midi()"));
    schedule_loaded(1);
    assert(collect_extras(extras, 16) == 1);
    assert(extras[0].note == 60 && !extras[0].has_cc);
    event_queue_clear();
    /* Rests and drums with extras commit extras without notes. */
    assert(track_load_from_code("s('~').ccn(74).ccv(0.5).midi()"));
    schedule_loaded(1);
    assert(collect_extras(extras, 16) == 1);
    assert(extras[0].note < 0 && extras[0].has_cc);
    event_queue_clear();
    assert(track_load_from_code("s('bd').ccn(20).ccv(0.5).midi()"));
    schedule_loaded(1);
    assert(collect_extras(extras, 16) == 1);
    assert(extras[0].note < 0 && extras[0].has_cc && extras[0].cc == 20);
    event_queue_clear();
    assert(track_load_from_code("midicmd(\"stop\").midi()"));
    schedule_loaded(1);
    assert(collect_extras(extras, 16) == 1);
    assert(extras[0].note < 0 && extras[0].cmd == 4);
    event_queue_clear();
    /* Unknown transport commands are ignored like the web output. */
    assert(track_load_from_code("midicmd(\"bogus\").midi()"));
    schedule_loaded(1);
    assert(collect_extras(extras, 16) == 1);
    assert(extras[0].cmd == 0);
    event_queue_clear();
    /* Port selection broadcasts to ALSA subscribers. */
    assert(track_load_from_code("note(60).midiport(2).ccn(1).ccv(1).midi()"));
    schedule_loaded(1);
    assert(collect_extras(extras, 16) == 1);
    assert(extras[0].note == 60 && extras[0].has_cc);
    event_queue_clear();
    assert(track_load_from_code("note(60).midiport(\"no-such-port\").midi()"));
    schedule_loaded(1);
    assert(collect_extras(extras, 16) == 1);
    assert(extras[0].note == 60);
    event_queue_clear();
    /* Patterned fan-out resolves per hap with cycle-start only on begin 0. */
    assert(track_load_from_code("note(\"60 62\").ccn(\"74 75\").ccv(\"0.5 0.25\").midi()"));
    schedule_loaded(2);
    assert(collect_extras(extras, 16) == 2);
    assert(extras[0].note == 60 && extras[0].has_cc && extras[0].cc == 74 && extras[0].ccv == 64);
    assert(extras[1].note == 62 && extras[1].has_cc && extras[1].cc == 75 && extras[1].ccv == 32);
    event_queue_clear();
    /* Extras combine with OSC; a failing Csound schedule still keeps MIDI. */
    assert(track_load_from_code("note(60).ccn(74).ccv(0.5).midi().osc()"));
    schedule_loaded(1);
    assert(collect_extras(extras, 16) == 1);
    assert(extras[0].midi_only && extras[0].osc_only && extras[0].has_cc);
    event_queue_clear();
    assert(track_load_from_code("note(60).ccn(3).ccv(0.25).midi().csound('MissingInstrXYZ')"));
    schedule_loaded(1);
    assert(collect_extras(extras, 16) == 1);
    assert(extras[0].midi_only && extras[0].has_cc && extras[0].cc == 3);
    event_queue_clear();
    puts("midi extras commit: values, rejections, abort order, control-only, combos ok");
}

/* Sender-level delivery: exact captured parameters and web send order. */
static void test_midi_extras_delivery(void) {
    midi_capture_enable(1);
    midi_cc(0, 74, 64);
    midi_program(1, 10);
    midi_bend(2, 2047);
    midi_touch(3, 95);
    unsigned char id[1] = {0x43}, data[2] = {0x79, 0x09};
    midi_sysex(id, 1, data, 2);
    midi_transport(1);
    midi_transport(4);
    MidiCaptured seen[16];
    unsigned total = midi_captured_all(seen, 16);
    assert(total == 7);
    assert(seen[0].kind == MIDI_CAP_CC && seen[0].chan == 0 && seen[0].d1 == 74 && seen[0].d2 == 64);
    assert(seen[1].kind == MIDI_CAP_PROG && seen[1].chan == 1 && seen[1].d1 == 10);
    assert(seen[2].kind == MIDI_CAP_BEND && seen[2].chan == 2 && seen[2].d1 == 2047);
    assert(seen[3].kind == MIDI_CAP_TOUCH && seen[3].chan == 3 && seen[3].d1 == 95);
    assert(seen[4].kind == MIDI_CAP_SYSEX && seen[4].d1 == 1 && seen[4].d2 == 2);
    assert(seen[4].sysex_len == 5 && seen[4].sysex[0] == 0xF0 && seen[4].sysex[1] == 0x43);
    assert(seen[4].sysex[2] == 0x79 && seen[4].sysex[3] == 0x09 && seen[4].sysex[4] == 0xF7);
    assert(seen[5].kind == MIDI_CAP_TRANSPORT && seen[5].d1 == 1);
    assert(seen[6].kind == MIDI_CAP_TRANSPORT && seen[6].d1 == 4);
    /* Out-of-range sends are dropped before capture. */
    midi_cc(0, 128, 64); midi_program(0, 200); midi_bend(0, 9000); midi_touch(0, 200);
    midi_transport(0); midi_transport(9);
    assert(midi_captured_all(seen, 16) == 7);
    midi_capture_enable(0);
    puts("midi extras delivery: captured parameters, framing, order, bounds ok");
}

#ifdef __SANITIZE_ADDRESS__
/* ALSA keeps per-handle internals past snd_seq_close; suppress only those,
   never test-code allocations. */
const char *__lsan_default_suppressions(void) {
    return "leak:snd_seq_hw_open\n";
}
#endif

/* Reference mode for scripts/midi-reference-qa.mjs: stdin is a JSON array
   of {name, code}; stdout is an array of {name, values, events} where
   values are the queried hap values (up to 8) and events are the committed
   dominant MIDI decisions. */
static int midi_reference_main(void) {
    log_console_stderr(0);
    isolate_environment();
    char *input = NULL;
    size_t cap = 0, len = 0;
    int chunk;
    while ((chunk = getchar()) != EOF && len + 1 < 1024 * 1024) {
        if (len + 1 >= cap) {
            size_t grown = cap ? cap * 2 : 65536;
            char *next = realloc(input, grown);
            if (!next) break;
            input = next; cap = grown;
        }
        input[len++] = (char)chunk;
    }
    if (!input) return 1;
    input[len] = 0;
    cJSON *cases = cJSON_Parse(input);
    free(input);
    if (!cJSON_IsArray(cases)) return 1;
    event_queue_clear(); audio_time = 0; audio_toggle_play(1);
    assert(prep_pool_start(0, 0));
    cJSON *out = cJSON_CreateArray();
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, cases) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(item, "code");
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "name", cJSON_IsString(name) ? name->valuestring : "");
        event_queue_clear();
        if (!cJSON_IsString(code) || !track_load_from_code(code->valuestring)) {
            cJSON_AddStringToObject(entry, "error", "load failed");
            cJSON_AddItemToArray(out, entry);
            continue;
        }
        cJSON *haps = javascript_query_audio(tracks[0].javascript, 0, 1, 0.5);
        cJSON *values = cJSON_CreateArray();
        if (cJSON_IsArray(haps)) {
            const cJSON *hap = NULL;
            int n = 0;
            cJSON_ArrayForEach(hap, haps) {
                if (n++ >= 8) break;
                const cJSON *begin = cJSON_GetObjectItemCaseSensitive(hap, "begin");
                const cJSON *end = cJSON_GetObjectItemCaseSensitive(hap, "end");
                const cJSON *value = cJSON_GetObjectItemCaseSensitive(hap, "value");
                cJSON *dump = cJSON_CreateObject();
                if (cJSON_IsNumber(begin)) cJSON_AddNumberToObject(dump, "begin", begin->valuedouble);
                if (cJSON_IsNumber(end)) cJSON_AddNumberToObject(dump, "end", end->valuedouble);
                cJSON_AddItemToObject(dump, "value", value ? cJSON_Duplicate(value, 1) : cJSON_CreateNull());
                cJSON_AddItemToArray(values, dump);
            }
        }
        cJSON_Delete(haps);
        cJSON_AddItemToObject(entry, "values", values);
        unsigned generation = event_queue_generation();
        js_audio_schedule(tracks[0].javascript, 0, 0, generation);
        for (int i = 0; i < 2000 && event_queue_depth() == 0; i++) {
            if (!prepare_queued_audio(0)) {
                struct timespec pause = {.tv_nsec = 1000000};
                nanosleep(&pause, NULL);
            }
        }
        cJSON *events = cJSON_CreateArray();
        for (QNode *node = qhead; node; node = node->next) {
            if (node->ev.control_only || node->ev.eviction_only || !node->ev.midi_only) continue;
            cJSON *dump = cJSON_CreateObject();
            cJSON_AddNumberToObject(dump, "chan", node->ev.midi_chan);
            cJSON_AddNumberToObject(dump, "note", node->ev.midi_note);
            if (node->ev.midi_has_cc) {
                cJSON_AddNumberToObject(dump, "cc", node->ev.midi_cc);
                cJSON_AddNumberToObject(dump, "ccv", node->ev.midi_ccv);
            }
            if (node->ev.midi_has_prog) cJSON_AddNumberToObject(dump, "prog", node->ev.midi_prog);
            if (node->ev.midi_has_bend) cJSON_AddNumberToObject(dump, "bend", node->ev.midi_bend);
            if (node->ev.midi_has_touch) cJSON_AddNumberToObject(dump, "touch", node->ev.midi_touch);
            if (node->ev.midi_has_sysex) {
                cJSON *id = cJSON_CreateArray(), *data = cJSON_CreateArray();
                for (int i = 0; i < node->ev.midi_sysex_id_len; i++)
                    cJSON_AddItemToArray(id, cJSON_CreateNumber(node->ev.midi_sysex_id[i]));
                for (int i = 0; i < node->ev.midi_sysex_len; i++)
                    cJSON_AddItemToArray(data, cJSON_CreateNumber(node->ev.midi_sysex[i]));
                cJSON_AddItemToObject(dump, "sysex_id", id);
                cJSON_AddItemToObject(dump, "sysex", data);
            }
            if (node->ev.midi_cmd) cJSON_AddNumberToObject(dump, "cmd", node->ev.midi_cmd);
            if (node->ev.midi_cycle_start) cJSON_AddNumberToObject(dump, "cycle_start", 1);
            if (node->ev.osc_only) cJSON_AddNumberToObject(dump, "osc", 1);
            if (node->ev.csound_only) cJSON_AddNumberToObject(dump, "csound", 1);
            cJSON_AddItemToArray(events, dump);
        }
        cJSON_AddItemToObject(entry, "events", events);
        cJSON_AddItemToArray(out, entry);
    }
    cJSON_Delete(cases);
    char *text = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    if (text) { printf("%s\n", text); free(text); }
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--midi-reference")) return midi_reference_main();
    if (argc>1 && !strcmp(argv[1],"--csound-worker")) {
#ifdef TOMPOUCE_TEST_CSOUND_WORKER
        execl(TOMPOUCE_TEST_CSOUND_WORKER,"test_midi","--csound-worker",(char *)NULL); return 1;
#else
        return csound_worker_main();
#endif
    }
    log_console_stderr(0);
    isolate_environment();
    test_midi_input_stores();
    test_commit_routing();
    test_threaded_delivery();
    test_midi_input_js();
    test_midi_persist();
    test_midi_reconnect();
    test_midi_loopback();
    test_midi_keys_commit();
    test_midi_restore_loopback();
    test_midi_extras_commit();
    test_midi_extras_delivery();
    event_queue_clear();
    midi_shutdown();
    puts("dominant MIDI routing holds without local voices; combined outputs stay independent");
    return 0;
}
