/* Staged activation: window splitting, late-path equivalence and queued-note
   survival. Drives schedule_live_javascript directly with a silent clock:
   no audio device, no UI, no threads. */
#include <assert.h>
#include <math.h>
#include <string.h>
#include "../src/audio.c"
#include "../src/scheduler.c"
#include "track.h"
#define PROG_A "$: note(\"c4*8\").s(\"sine\")"
#define PROG_B "$: note(\"e4*8\").s(\"sine\")"
typedef struct {
    double time, freq;
} EvKey;
static int evkey_cmp(const void *a, const void *b) {
    const EvKey *x=a, *y=b;
    if (x->time<y->time) return -1;
    if (x->time>y->time) return 1;
    if (x->freq<y->freq) return -1;
    if (x->freq>y->freq) return 1;
    return 0;
}
static void run_gen(double now) {
    pthread_mutex_lock(&tracks_lock);
    assert(schedule_live_javascript(now));
    pthread_mutex_unlock(&tracks_lock);
}
/* Tick like the 100 ms driver: each call re-queries once it is due. */
static void run_ticks(double first, int n) {
    int i;
    for (i=0;i<n;i++) run_gen(first+0.1*i);
}
/* Identical starting conditions for every run: stopped, cleared, loaded
   fresh at audio time 100 with 120 bpm, then playing. The clear must come
   before the load: clearing after would bump the queue generation past
   the program's compile generation and the generator would (correctly)
   treat every window as stale. */
static void reset(const char *code) {
    audio_set_bpm(120.0);
    audio_time=100.0;
    audio_toggle_play(0);
    event_queue_clear();
    assert(track_load_from_code(code));
    audio_time=100.0;
    audio_toggle_play(1);
}
/* Compile exactly like track_load_from_code does, for staging. */
static int compile_slot(const char *code, JsProgram **prog, cJSON **meta) {
    char error[512];
    JsProgram *previous=NULL;
    pthread_mutex_lock(&tracks_lock);
    if (track_count==1 && tracks[0].javascript) {
        previous=tracks[0].javascript;
        javascript_retain(previous);
    }
    pthread_mutex_unlock(&tracks_lock);
    *prog=javascript_compile_from(code,audio_get_bpm(),previous,meta,error,
        sizeof(error));
    if (*prog && !javascript_prepare_csound_from(*prog,*meta,previous,error,
            sizeof(error))) {
        javascript_free(*prog);
        *prog=NULL;
        cJSON_Delete(*meta);
        *meta=NULL;
    }
    javascript_free(previous);
    return *prog!=NULL;
}
/* Copy the accepted queue in cycle order. Asserts the collab slot tag
   stays local-only until attribution arrives (Phase 1). */
static int drain(EvKey *out, int cap) {
    QNode *node;
    int n=0;
    pthread_mutex_lock(&tracks_lock);
    for (node=qhead;node && n<cap;node=node->next) {
        assert(node->ev.slot==0);
        out[n].time=node->ev.time;
        out[n].freq=node->ev.freq;
        n++;
    }
    pthread_mutex_unlock(&tracks_lock);
    event_queue_clear();
    qsort(out,(size_t)n,sizeof(*out),evkey_cmp);
    return n;
}
/* Same copy without clearing: lets the test read the baseline pitch. */
static int peek(EvKey *out, int cap) {
    QNode *node;
    int n=0;
    pthread_mutex_lock(&tracks_lock);
    for (node=qhead;node && n<cap;node=node->next) {
        out[n].time=node->ev.time;
        out[n].freq=node->ev.freq;
        n++;
    }
    pthread_mutex_unlock(&tracks_lock);
    qsort(out,(size_t)n,sizeof(*out),evkey_cmp);
    return n;
}
static void check_same(const EvKey *a, int na, const EvKey *b, int nb,
    const char *what) {
    int i;
    fprintf(stderr,"run A (%d):",na);
    for (i=0;i<na && i<16;i++) fprintf(stderr," (%.17g,%.17g)",a[i].time,a[i].freq);
    fprintf(stderr,"\nrun B (%d):",nb);
    for (i=0;i<nb && i<16;i++) fprintf(stderr," (%.17g,%.17g)",b[i].time,b[i].freq);
    fprintf(stderr,"\n");
    assert(na==nb);
    /* Onset times may differ by 1 ulp of float association: the staged
       path anchors the new program as anchor + (pending - anchor) * s
       while an immediate load anchors it as js_phase + .1. Same shared
       instant, inaudible difference (a sample is ~2e-5 s). */
    for (i=0;i<na;i++) {
        assert(fabs(a[i].time-b[i].time)<=1e-9);
        assert(a[i].freq==b[i].freq);
    }
    (void)what;
}
int main(void) {
    EvKey first[4096], second[4096];
    int n1, n2, i;
    JsProgram *staged=NULL;
    cJSON *meta=NULL;
    /* Late path matches an immediate load event for event. */
    reset(PROG_A);
    run_ticks(100.0,1);
    assert(compile_slot(PROG_B,&staged,&meta));
    pthread_mutex_lock(&tracks_lock);
    assert(tracks[0].js_cursor>0);
    pthread_mutex_unlock(&tracks_lock);
    /* Stage at the current cycle: behind the query cursor on next run. */
    pthread_mutex_lock(&tracks_lock);
    {
        double at=tracks[0].js_cursor;
        pthread_mutex_unlock(&tracks_lock);
        assert(track_stage_program(staged,meta,at,7));
    }
    run_ticks(100.1,4);
    n1=drain(first,4096);
    assert(n1>0);
    /* Same run through an immediate load instead of a stage. */
    reset(PROG_A);
    run_ticks(100.0,1);
    assert(track_load_from_code(PROG_B));
    run_ticks(100.1,4);
    n2=drain(second,4096);
    check_same(first,n1,second,n2,"late path");
    /* Split path: A before the staged cycle, B at and after it; an
       accepted A note that spans the split survives activation. */
    reset(PROG_A);
    run_ticks(100.0,1);
    {
        EvKey base[4096];
        double afreq;
        int nb;
        nb=peek(base,4096);
        assert(nb>0);
        afreq=base[0].freq;
        for (i=1;i<nb;i++) assert(base[i].freq==afreq);
        assert(compile_slot(PROG_B,&staged,&meta));
        assert(track_stage_program(staged,meta,0.21,8));
        run_ticks(100.1,4);
        n1=drain(first,4096);
        {
            double amax=-1, bmin=1e300;
            int bcount=0;
            for (i=0;i<n1;i++) {
                if (first[i].freq==afreq) {
                    if (first[i].time>amax) amax=first[i].time;
                } else {
                    bcount++;
                    if (first[i].time<bmin) bmin=first[i].time;
                }
            }
            /* B plays only at/after the split; the earliest accepted A
               note survives activation. */
            assert(bcount>0);
            assert(amax<bmin);
            assert(first[0].freq==afreq);
        }
    }
    /* A superseding stage wins; clearing drops the stage silently. */
    reset(PROG_A);
    assert(compile_slot(PROG_B,&staged,&meta));
    assert(track_stage_program(staged,meta,5.0,9));
    assert(compile_slot(PROG_A,&staged,&meta));
    assert(track_stage_program(staged,meta,6.0,10));
    pthread_mutex_lock(&tracks_lock);
    assert(tracks[0].pending!=NULL && tracks[0].pending_cycle==6.0 &&
        tracks[0].pending_seq==10);
    pthread_mutex_unlock(&tracks_lock);
    track_clear_pending();
    run_gen(100.0);
    n1=drain(first,4096);
    assert(n1>0);
    for (i=0;i<n1;i++) assert(fabs(first[i].freq-261.63)<1.0);
    puts("scheduler collab ok");
    return 0;
}
