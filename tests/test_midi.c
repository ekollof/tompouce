/* Dominant MIDI routing: commit flags plus threaded scheduler-to-ALSA
   delivery. The capture ring observes scheduling intent with or without
   ALSA hardware, so this runs anywhere. */
#include "../src/audio.c"
#include "../src/scheduler.c"
#include "midi.h"
#include <assert.h>
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
#ifdef HAVE_CSOUND
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
    midi_shutdown();
    puts("midi delivery: ordered notes with velocity, no local voices, legacy path intact");
}

int main(int argc, char **argv) {
    if (argc>1 && !strcmp(argv[1],"--csound-worker")) return csound_worker_main();
    log_console_stderr(0);
    isolate_environment();
    test_commit_routing();
    test_threaded_delivery();
    event_queue_clear();
    puts("dominant MIDI routing holds without local voices; combined outputs stay independent");
    return 0;
}
