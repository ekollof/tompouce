/* Jam-mode room eval path (M2 slice): slot table, composite build order,
   last-good substitution, staged activation with slot tags, and the
   consent allowlist. Drives schedule_live_javascript directly with a
   silent clock: no audio device, no UI, no threads, no network. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/audio.c"
#include "../src/scheduler.c"
#include "collab.h"
#include "track.h"
static void run_gen(double now) {
    pthread_mutex_lock(&tracks_lock);
    assert(schedule_live_javascript(now));
    pthread_mutex_unlock(&tracks_lock);
}
static void run_ticks(double first, int n) {
    int i;
    for (i=0;i<n;i++) run_gen(first+0.1*i);
}
static void reset(const char *code) {
    audio_set_bpm(120.0);
    audio_time=100.0;
    audio_toggle_play(0);
    event_queue_clear();
    collab_room_reset();
    collab_disconnect();
    assert(track_load_from_code(code));
    audio_time=100.0;
    audio_toggle_play(1);
}
typedef struct {
    double time, freq;
    int slot;
} TaggedKey;
static int tagged_cmp(const void *a, const void *b) {
    const TaggedKey *x=a, *y=b;
    if (x->time<y->time) return -1;
    if (x->time>y->time) return 1;
    if (x->freq<y->freq) return -1;
    if (x->freq>y->freq) return 1;
    return x->slot-y->slot;
}
/* Copy the accepted queue in cycle order with slot tags, then clear. */
static int drain_tagged(TaggedKey *out, int cap) {
    QNode *node;
    int n=0;
    pthread_mutex_lock(&tracks_lock);
    for (node=qhead;node && n<cap;node=node->next) {
        out[n].time=node->ev.time;
        out[n].freq=node->ev.freq;
        out[n].slot=node->ev.slot;
        n++;
    }
    pthread_mutex_unlock(&tracks_lock);
    event_queue_clear();
    qsort(out,(size_t)n,sizeof(*out),tagged_cmp);
    return n;
}
static double music_cursor_now(void) {
    double cursor;
    pthread_mutex_lock(&tracks_lock);
    cursor=tracks[0].js_cursor;
    pthread_mutex_unlock(&tracks_lock);
    return cursor;
}
static void test_slot_from_id(void) {
    assert(collab_slot_from_id(NULL)==0);
    assert(collab_slot_from_id("")==0);
    assert(collab_slot_from_id("abc")==0);
    assert(collab_slot_from_id("0/$0")==0);
    assert(collab_slot_from_id("3/$0")==3);
    assert(collab_slot_from_id("7")==7);
    assert(collab_slot_from_id("1023/x")==1023);
    assert(collab_slot_from_id("1024/x")==0);
    assert(collab_slot_from_id("-1/x")==0);
    assert(collab_slot_from_id("3x")==0);
    assert(collab_slot_from_id("/x")==0);
    puts("room slot ids ok");
}
static void test_consent_store(void) {
    char path[1100], *text=NULL;
    FILE *file=NULL;
    long length;
    /* Consent is room-scoped: nothing applies without the room tag, and
       the gate consults the connected room ("" here), so an entry for
       another room must not open this one. */
    assert(collab_allow(COLLAB_CAP_OSC,"test-room-osc","127.0.0.1:57120"));
    assert(!collab_capability_allowed(1,COLLAB_CAP_OSC,"127.0.0.1:57120"));
    assert(!collab_capability_allowed(1,COLLAB_CAP_OSC,"127.0.0.1:57121"));
    assert(!collab_capability_allowed(1,COLLAB_CAP_MIDI_OUT,"hw:1,0"));
    /* Exact targets only: no prefixes, no wildcards, no empty target. */
    assert(!collab_capability_allowed(1,COLLAB_CAP_OSC,NULL));
    assert(collab_allow(COLLAB_CAP_OSC,"test-room-osc","127.0.0.1:57120"));
    assert(!collab_allow((CollabCap)99,"test-room-osc","x"));
    assert(!collab_allow(COLLAB_CAP_OSC,"","x"));
    assert(!collab_allow(COLLAB_CAP_OSC,"test-room-osc",""));
    /* Persistence roundtrip through collab.json. */
    assert(collab_deny(COLLAB_CAP_MIDI_OUT,"test-room-osc","hw:1,0"));
    assert(collab_allow(COLLAB_CAP_MIDI_OUT,"test-room-midi","hw:1,0"));
    snprintf(path,sizeof(path),"%s/tompouce/collab.json",getenv("XDG_CONFIG_HOME"));
    file=fopen(path,"rb");
    assert(file);
    assert(!fseek(file,0,SEEK_END));
    length=ftell(file);
    assert(length>0 && length<65536);
    rewind(file);
    text=malloc((size_t)length+1);
    assert(text && fread(text,1,(size_t)length,file)==(size_t)length);
    fclose(file);
    text[length]=0;
    assert(strstr(text,"\"test-room-midi\"") && strstr(text,"hw:1,0"));
    free(text);
    collab_consent_reload();
    file=fopen(path,"rb");
    assert(file);
    fclose(file);
    /* Deny removes the entry again. */
    assert(collab_deny(COLLAB_CAP_MIDI_OUT,"test-room-midi","hw:1,0"));
    assert(collab_deny(COLLAB_CAP_OSC,"test-room-osc","127.0.0.1:57120"));
    assert(!collab_capability_allowed(1,COLLAB_CAP_OSC,"127.0.0.1:57120"));
    /* Slot 0 is never gated. */
    assert(collab_capability_allowed(0,COLLAB_CAP_OSC,NULL));
    puts("room consent ok");
}
static void test_room_apply(void) {
    char err[256]="";
    TaggedKey keys[64];
    int n, i, saw0, saw1;
    double at;
    reset("$: note(60).s('sine')");
    run_ticks(100.0,5);
    /* Local slot 0 plus a remote slot 1: ascending order, remote profile. */
    assert(collab_room_set_slot(0,"$: note(62).s('sine')",strlen("$: note(62).s('sine')"),0));
    assert(collab_room_set_slot(1,"$: note(67).s('sine')",strlen("$: note(67).s('sine')"),1));
    at=music_cursor_now()+2.0;
    assert(collab_room_apply(at,1,err,sizeof(err)));
    /* Slot errors are recorded per slot for the author. */
    assert(!collab_room_slot_error(0) && !collab_room_slot_error(1));
    run_ticks(100.5,60);
    n=drain_tagged(keys,64);
    saw0=saw1=0;
    for (i=0;i<n;i++) {
        if (keys[i].freq>293 && keys[i].freq<294 && keys[i].slot==0) saw0=1;
        if (keys[i].freq>391 && keys[i].freq<393 && keys[i].slot==1) saw1=1;
    }
    if (!saw0 || !saw1) {
        fprintf(stderr,"room apply: %d events, saw0=%d saw1=%d\n",n,saw0,saw1);
        for (i=0;i<n && i<16;i++)
            fprintf(stderr," (%.3f,%.1f,s%d)",keys[i].time,keys[i].freq,keys[i].slot);
        fprintf(stderr,"\n");
    }
    assert(saw0 && saw1);
    puts("room apply ok");
}
static void test_room_last_good(void) {
    char err[256]="";
    TaggedKey keys[64];
    int n, i, saw0, saw1;
    double at;
    reset("$: note(60).s('sine')");
    run_ticks(100.0,5);
    assert(collab_room_set_slot(0,"$: note(62).s('sine')",strlen("$: note(62).s('sine')"),0));
    assert(collab_room_set_slot(1,"$: note(67).s('sine')",strlen("$: note(67).s('sine')"),1));
    at=music_cursor_now()+2.0;
    assert(collab_room_apply(at,1,err,sizeof(err)));
    run_ticks(100.5,30);
    drain_tagged(keys,64);
    /* Break the remote slot: the local slot's new body still lands and
       the remote slot keeps its last good notes. */
    assert(collab_room_set_slot(0,"$: note(64).s('sine')",strlen("$: note(64).s('sine')"),0));
    assert(collab_room_set_slot(1,"throw new Error('nope')",strlen("throw new Error('nope')"),1));
    at=music_cursor_now()+2.0;
    assert(collab_room_apply(at,2,err,sizeof(err)));
    assert(!collab_room_slot_error(0) && collab_room_slot_error(1));
    assert(strstr(collab_room_slot_error(1),"nope"));
    run_ticks(103.5,60);
    n=drain_tagged(keys,64);
    saw0=saw1=0;
    for (i=0;i<n;i++) {
        if (keys[i].freq>329 && keys[i].freq<330 && keys[i].slot==0) saw0=1;
        if (keys[i].freq>391 && keys[i].freq<393 && keys[i].slot==1) saw1=1;
    }
    if (!saw0 || !saw1) {
        fprintf(stderr,"room last-good: %d events, saw0=%d saw1=%d\n",n,saw0,saw1);
        for (i=0;i<n && i<16;i++)
            fprintf(stderr," (%.3f,%.1f,s%d)",keys[i].time,keys[i].freq,keys[i].slot);
        fprintf(stderr,"\n");
    }
    assert(saw0 && saw1);
    puts("room last-good ok");
}
static void test_room_reject(void) {
    char err[256]="";
    reset("$: note(60).s('sine')");
    /* No slots, bad cycles, oversized bodies and bad ids fail closed. */
    assert(!collab_room_apply(101.0,1,err,sizeof(err)));
    assert(!collab_room_set_slot(-1,"$: note(60)",strlen("$: note(60)"),0));
    assert(!collab_room_set_slot(16,"$: note(60)",strlen("$: note(60)"),0));
    assert(!collab_room_set_slot(1024,"$: note(60)",strlen("$: note(60)"),0));
    assert(!collab_room_set_slot(0,"$: note(60)",strlen("$: note(60)")+1,0));
    assert(!collab_room_set_slot(0,NULL,0,0));
    assert(!collab_room_apply(HUGE_VAL,1,err,sizeof(err)));
    assert(collab_room_set_slot(0,"$: note(60)",strlen("$: note(60)"),0));
    assert(collab_room_apply(music_cursor_now()+1.0,1,err,sizeof(err)));
    puts("room reject ok");
}
int main(void) {
    test_slot_from_id();
    test_consent_store();
    test_room_apply();
    test_room_last_good();
    test_room_reject();
    puts("collab room ok");
    return 0;
}
