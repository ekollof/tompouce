/* Owned, offline scheduler/callback: never opens an audio device or UI. */
#include "../src/audio.c"
#include "../src/scheduler.c"
#include <assert.h>
#include "sound_store.h"
#include <sys/stat.h>
#include <pthread.h>
#include <time.h>

static pthread_mutex_t prep_gate_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t prep_gate_wake=PTHREAD_COND_INITIALIZER;
static int prep_gate_entered, prep_gate_released;

static void block_saw_preparation(const AudioEvent *event) {
    if (strcmp(event->token,"saw")) return;
    pthread_mutex_lock(&prep_gate_lock);
    prep_gate_entered=1;
    pthread_cond_broadcast(&prep_gate_wake);
    while (!prep_gate_released) pthread_cond_wait(&prep_gate_wake,&prep_gate_lock);
    pthread_mutex_unlock(&prep_gate_lock);
}

typedef struct { int visible; } PrefixProbe;

static void *prefix_probe_thread(void *argument) {
    PrefixProbe *probe=argument;
    pthread_mutex_lock(&prep_gate_lock);
    while (!prep_gate_entered) pthread_cond_wait(&prep_gate_wake,&prep_gate_lock);
    pthread_mutex_unlock(&prep_gate_lock);
    struct timespec started;
    clock_gettime(CLOCK_MONOTONIC,&started);
    for (;;) {
        if (event_queue_depth()>=1) { probe->visible=1; break; }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC,&now);
        double elapsed=(double)(now.tv_sec-started.tv_sec)+
            (double)(now.tv_nsec-started.tv_nsec)/1e9;
        if (elapsed>=1.0) break;
        struct timespec pause={.tv_nsec=1000000};
        nanosleep(&pause,NULL);
    }
    pthread_mutex_lock(&prep_gate_lock);
    prep_gate_released=1;
    pthread_cond_broadcast(&prep_gate_wake);
    pthread_mutex_unlock(&prep_gate_lock);
    return NULL;
}

/* A later blocked source must not hide an earlier ready note. */
static void test_ready_prefix_publication(void) {
    audio_time=0; audio_toggle_play(1); event_queue_clear();
    assert(prep_pool_start(0,0));
    assert(track_load_from_code("stack(note('c4').s('triangle'), note('e4').s('saw'))"));
    unsigned generation=event_queue_generation();
    prep_gate_entered=0; prep_gate_released=0;
    PrefixProbe probe={0};
    js_audio_set_prep_observer(block_saw_preparation);
    pthread_t probe_thread;
    assert(pthread_create(&probe_thread,NULL,prefix_probe_thread,&probe)==0);
    assert(js_audio_schedule(tracks[0].javascript,0,0,generation));
    assert(pthread_join(probe_thread,NULL)==0);
    js_audio_set_prep_observer(NULL);
    assert(probe.visible);
    assert(event_queue_depth()==2);
    assert(strcmp(qhead->ev.token,"triangle")==0);
    event_queue_clear();
    audio_time=0;
}

typedef struct { int replace,visible; } DuckPrepProbe;

static void *duck_prep_probe(void *argument) {
    DuckPrepProbe *probe=argument;
    pthread_mutex_lock(&prep_gate_lock);
    while (!prep_gate_entered) pthread_cond_wait(&prep_gate_wake,&prep_gate_lock);
    pthread_mutex_unlock(&prep_gate_lock);
    /* Wait for publication, not source completion. A broken implementation
       times out and releases the source so the test fails without hanging. */
    for (int i=0;i<1000 && !event_queue_depth();i++) {
        struct timespec pause={.tv_nsec=1000000}; nanosleep(&pause,NULL);
    }
    if (event_queue_depth()) {
        assert(prepare_queued_audio(0));
        assert(pending_duck_count==1 && !audio_active_voices());
        probe->visible=1;
        if (probe->replace) event_queue_new_program(HUGE_VAL);
        /* Advance an owned callback while the source remains blocked. The
           accepted duck survives a generation change and reaches its target. */
        float output[256];
        while (audio_time_now()<.15) pa_callback(NULL,output,128,NULL,0,NULL);
        assert(!pending_duck_count && orbit_duck_gain(2)<1);
    }
    pthread_mutex_lock(&prep_gate_lock);
    prep_gate_released=1; pthread_cond_broadcast(&prep_gate_wake);
    pthread_mutex_unlock(&prep_gate_lock);
    return NULL;
}

static void test_duck_before_source_preparation(void) {
    assert(prep_pool_start(0,0));
    for (int replace=0;replace<=1;replace++) {
        event_queue_clear(); audio_time=0; audio_toggle_play(1);
        audio_register_orbit(2);
        assert(track_load_from_code("note('c4').s('saw').duckorbit(2).duckdepth(.5).duckattack(.2)"));
        prep_gate_entered=0; prep_gate_released=0;
        DuckPrepProbe probe={.replace=replace};
        js_audio_set_prep_observer(block_saw_preparation);
        pthread_t thread;
        assert(!pthread_create(&thread,NULL,duck_prep_probe,&probe));
        (void)js_audio_schedule(tracks[0].javascript,.1,0,event_queue_generation());
        assert(!pthread_join(thread,NULL));
        js_audio_set_prep_observer(NULL);
        assert(probe.visible);
        for (QNode *node=qhead;node;node=node->next)
            assert(node->ev.control_only && node->ev.audio_prepared);
        memset(web_duck,0,sizeof(web_duck));
    }
    event_queue_clear(); audio_time=0;
}

static void test_refresh_cutoff(void) {
    audio_time=10.0; event_queue_clear();
    unsigned generation=event_queue_generation();
    AudioEvent original={.time=10.2,.gain=.8,.generation=generation,
        .smp=-1,.wt=-1,.midi_chan=-1,.midi_note=-1};
    event_queue_push(&original);
    assert(event_queue_depth()==1);
    event_queue_refresh(10.0);
    AudioEvent replacement=original;
    replacement.gain=.3;
    event_queue_push(&replacement);
    assert(qstaging.count==1);
    audio_time=10.3;
    assert(event_queue_refresh_finish(1));
    /* The original is within the final cutoff; its replacement is not
       reinserted, so a consumed/due onset cannot retrigger. */
    assert(event_queue_depth()==1 && qhead->ev.gain==.8);

    event_queue_clear(); audio_time=10.0;
    generation=event_queue_generation();
    event_queue_refresh(10.0);
    AudioEvent expired={.time=10.2,.gain=.3,.generation=generation,
        .smp=-1,.wt=-1,.midi_chan=-1,.midi_note=-1};
    event_queue_push(&expired);
    audio_time=10.3;
    assert(event_queue_refresh_finish(1));
    assert(event_queue_depth()==0);

    event_queue_clear(); audio_time=10.0;
    generation=event_queue_generation();
    event_queue_refresh(10.0);
    AudioEvent future={.time=10.5,.gain=.3,.generation=generation,
        .smp=-1,.wt=-1,.midi_chan=-1,.midi_note=-1};
    event_queue_push(&future);
    audio_time=10.3;
    assert(event_queue_refresh_finish(1));
    assert(event_queue_depth()==1 && qhead->ev.time==10.5);
    event_queue_clear(); audio_time=0;
}

static void test_custom_room_schedule(void) {
    char root[]="/tmp/tompouce-room-XXXXXX",path[512],names[2][69];
    const char *previous=getenv("XDG_CONFIG_HOME");
    char *saved=previous?strdup(previous):NULL; assert(!previous || saved);
    assert(mkdtemp(root) && !setenv("XDG_CONFIG_HOME",root,1));
    const char *directories[]={"tompouce","tompouce/sounds","tompouce/sounds/assets"};
    for (int i=0;i<3;i++) {
        snprintf(path,sizeof(path),"%s/%s",root,directories[i]); assert(!mkdir(path,0700));
    }
    for (int i=0;i<2;i++) {
        memset(names[i],'0'+i,64); memcpy(names[i]+64,".wav",5);
        snprintf(path,sizeof(path),"%s/tompouce/sounds/assets/%s",root,names[i]);
        unsigned char wav[]={'R','I','F','F',44,0,0,0,'W','A','V','E','f','m','t',' ',
            16,0,0,0,1,0,1,0,68,172,0,0,136,88,1,0,2,0,16,0,
            'd','a','t','a',8,0,0,0,0,32,0,192,0,16,0,48};
        wav[45]+=(unsigned char)i;
        FILE *file=fopen(path,"wb"); assert(file);
        assert(fwrite(wav,1,sizeof(wav),file)==sizeof(wav) && !fclose(file));
    }
    snprintf(path,sizeof(path),"%s/tompouce/sounds/index.json",root);
    FILE *file=fopen(path,"w"); assert(file);
    assert(fprintf(file,"{\"version\":1,\"sounds\":{\"roomfixture\":["
        "{\"label\":\"a.wav\",\"file\":\"%s\"},{\"label\":\"b.wav\",\"file\":\"%s\"}],"
        "\"roomfixtureb\":[{\"label\":\"b.wav\",\"file\":\"%s\"}]}}",names[0],names[1],names[1])>0);
    assert(!fclose(file) && user_sounds_reload()==2);
    audio_time=0; audio_toggle_play(1); event_queue_clear();
    assert(track_load_from_code("s('sine').gain(0).room(1).orbit(11).ir('roomfixture').set({i:0}).roomsize(.3).irspeed(2).irbegin(.25)"));
    js_audio_schedule(tracks[0].javascript,1,0,event_queue_generation());
    assert(qhead && qhead->ev.ir_kernel && qhead->ev.room_params_valid);
    const ConvKernel *kernel=qhead->ev.ir_kernel;
    assert(qhead->ev.room_speed==2 && qhead->ev.room_begin==.25);
    event_queue_clear();
    assert(track_load_from_code("s('sine').gain(0).room(1).orbit(11).ir('roomfixture').set({i:0})"));
    js_audio_schedule(tracks[0].javascript,2,0,event_queue_generation());
    assert(qhead && qhead->ev.ir_kernel==kernel && qhead->ev.dsp.p[DSP_SIZE]==.3);
    assert(qhead->ev.room_speed==2 && qhead->ev.room_begin==.25);
    event_queue_clear();
    assert(track_load_from_code("s('sine').gain(0).room(1).orbit(11).ir('roomfixtureb')"));
    js_audio_schedule(tracks[0].javascript,3,0,event_queue_generation());
    assert(qhead && qhead->ev.ir_kernel && qhead->ev.ir_kernel!=kernel);
    assert(qhead->ev.room_speed==1 && qhead->ev.room_begin==0 && qhead->ev.dsp.p[DSP_SIZE]==2);
    const ConvKernel *second=qhead->ev.ir_kernel;
    event_queue_clear();
    assert(track_load_from_code("s('sine').gain(0).room(1).orbit(11).ir('roomfixture').set({i:1})"));
    js_audio_schedule(tracks[0].javascript,4,0,event_queue_generation());
    assert(qhead && qhead->ev.ir_kernel==second);
    assert(!qhead->ev.room_rebuild);
    VoiceMix seed={.room_l=1,.room_r=-1}; native_reverb(11,&seed);
    assert(convolvers[11].remaining>0);
    unsigned long long revision=atomic_load(&ir_revision[11]);
    event_queue_clear();
    assert(track_load_from_code("s('sine').gain(0).room(1).orbit(11).ir('roomfixture').set({i:1}).irspeed(1)"));
    js_audio_schedule(tracks[0].javascript,5,0,event_queue_generation());
    assert(qhead && qhead->ev.room_rebuild && qhead->ev.ir_kernel==second);
    assert(atomic_load(&ir_revision[11])==revision+2);
    VoiceMix reset={0}; native_reverb(11,&reset);
    assert(reset.left==0 && reset.right==0 && !convolvers[11].remaining);
    seed=(VoiceMix){.room_l=1,.room_r=-1}; native_reverb(11,&seed);
    revision=atomic_load(&ir_revision[11]);
    event_queue_clear();
    assert(track_load_from_code("s('sine').gain(0).room(1).orbit(11).ir('roomfixture').set({i:1})"));
    js_audio_schedule(tracks[0].javascript,6,0,event_queue_generation());
    assert(qhead && !qhead->ev.room_rebuild && atomic_load(&ir_revision[11])==revision);
    VoiceMix retained={0}; native_reverb(11,&retained);
    assert(retained.left!=0 && retained.right!=0);
    /* Pause publication after the pointer write but before its final
       revision. The callback must render the previous history without
       waiting, then reset exactly once when publication completes. */
    ConvState *during=malloc(sizeof(*during)); assert(during);
    memcpy(during,&convolvers[11],sizeof(*during));
    unsigned long long rendered=rendered_ir_revision[11];
    atomic_fetch_add(&ir_revision[11],1);
    atomic_store(&requested_ir[11],kernel);
    VoiceMix interrupted={0}; double expected_left,expected_right;
    native_reverb(11,&interrupted);
    convolution_process(during,0,0,&expected_left,&expected_right);
    assert(interrupted.left==expected_left && interrupted.right==expected_right);
    assert(interrupted.left!=0 && rendered_ir_revision[11]==rendered);
    atomic_fetch_add(&ir_revision[11],1);
    VoiceMix completed={0}; native_reverb(11,&completed);
    assert(completed.left==0 && completed.right==0);
    assert(convolvers[11].kernel==kernel && !convolvers[11].remaining);
    assert(rendered_ir_revision[11]==atomic_load(&ir_revision[11]));
    free(during);
    event_queue_clear(); assert(!unlink(path));
    for (int i=0;i<2;i++) {
        snprintf(path,sizeof(path),"%s/tompouce/sounds/assets/%s",root,names[i]); assert(!unlink(path));
    }
    for (int i=2;i>=0;i--) {
        snprintf(path,sizeof(path),"%s/%s",root,directories[i]); assert(!rmdir(path));
    }
    assert(!rmdir(root));
    assert(saved?!setenv("XDG_CONFIG_HOME",saved,1):!unsetenv("XDG_CONFIG_HOME"));
    free(saved); user_sounds_reload();
}

static void live_tick(double now) {
    audio_time=now;
    pthread_mutex_lock(&tracks_lock);
    assert(schedule_live_javascript(now));
    pthread_mutex_unlock(&tracks_lock);
}

static void reference_clock(int print) {
    event_queue_clear(); audio_toggle_play(0); audio_set_bpm(120); audio_time=0;
    assert(track_load_from_code("note(\"60 64 67 72\").fast(10).gain(slider(.5))"));
    audio_toggle_play(1);
    live_tick(0);
    if (event_queue_depth()!=4 || !qhead || fabs(qhead->ev.time-.11)>1e-9)
        fprintf(stderr,"initial clock: queue=%d time=%.12g cursor=%.12g origin=%.12g seconds=%.12g\n",event_queue_depth(),qhead?qhead->ev.time:-1,tracks[0].js_cursor,tracks[0].js_origin,tracks[0].js_seconds);
    assert(event_queue_depth()==4 && fabs(qhead->ev.time-.11)<1e-9);
    AudioEvent held=qhead->ev; held.duration=10;
    audio_trigger_event(&held);
    int voice=-1;
    for (int i=0;i<MAX_VOICES;i++) if (voices[i].active && voices[i].freq==held.freq) { voice=i; break; }
    assert(voice>=0);
    double held_start=voices[voice].start_time;
    double held_amplitude=voices[voice].amp;
    assert(slider_set(0,.75)); live_tick(.1);
    assert(event_queue_depth()==6);
    unsigned generation=event_queue_generation();
    audio_set_bpm(240); live_tick(.2);
    assert(event_queue_depth()==10 && event_queue_generation()==generation);
    double boundary=tracks[0].js_origin+tracks[0].js_cursor*tracks[0].js_seconds;
    assert(fabs(boundary-.51)<1e-9);
    audio_time=.25;
    assert(track_load_from_code("note(\"72 76 79 84\").fast(10).gain(slider(.75))"));
    assert(fabs(tracks[0].js_cursor-.25)<1e-9 && event_queue_depth()==10);
    live_tick(.3);
    assert(event_queue_depth()==14);
    assert(voices[voice].active && voices[voice].start_time==held_start && voices[voice].amp==held_amplitude);
    float pcm[512]; pa_callback(NULL,pcm,256,NULL,0,NULL);
    double energy=0;
    for (int i=0;i<512;i++) { assert(isfinite(pcm[i])); energy+=pcm[i]*pcm[i]; }
    assert(energy>1e-6);
    Track stable; memcpy(&stable,&tracks[0],sizeof(stable)); QNode *committed=qhead;
    assert(!track_load_from_code("note(99).gain(slider(.1)); throw Error('performance eval failure')"));
    assert(!memcmp(&stable,&tracks[0],sizeof(stable)) && qhead==committed);
    double age=voices[voice].age;
    scheduler_transport_set(0);
    pa_callback(NULL,pcm,256,NULL,0,NULL);
    energy=0; for (int i=0;i<512;i++) energy+=pcm[i]*pcm[i];
    assert(!audio_is_playing() && energy>1e-6 && voices[voice].age>age);
    assert(voices[voice].start_time==held_start);
    double previous=-1;
    if (print) printf("[");
    for (QNode *node=qhead;node;node=node->next) {
        assert(node->ev.time>previous); previous=node->ev.time;
        assert(fabs(node->ev.gain-(node->ev.time<.3?.5:.75))<1e-9);
        if (print) printf("%s{\"time\":%.12g,\"duration\":%.12g,\"freq\":%.12g,\"gain\":%.12g}",node==qhead?"":",",node->ev.time,node->ev.duration,node->ev.freq,node->ev.gain);
    }
    if (print) puts("]");
    audio_toggle_play(0); event_queue_clear();
    assert(track_load_from_code("note(60).gain(0)"));
}

static void test_performance_tail(void) {
    event_queue_clear(); audio_toggle_play(0);
    assert(track_load_from_code("note(60).gain(slider(.5)).delay(.4).gain(0)"));
    cJSON *hap=cJSON_Parse("{\"begin\":0,\"end\":1,\"value\":{\"note\":60,\"delay\":0.5}}");
    AudioEvent timing;
    assert(hap && js_audio_event(hap,0,2,&timing) && timing.delay_time==.375 && timing.delay_fb==.5);
    cJSON *value=cJSON_GetObjectItemCaseSensitive(hap,"value");
    assert(cJSON_AddNumberToObject(value,"delaytime",.1) && cJSON_AddNumberToObject(value,"delaysync",.5));
    assert(js_audio_event(hap,0,2,&timing) && timing.delay_time==.1);
    assert(cJSON_AddNumberToObject(value,"delayfeedback",0));
    assert(js_audio_event(hap,0,2,&timing) && timing.delay_send==0);
    assert(cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(value,"delayfeedback"),.5)==.5);
    assert(cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(value,"delaytime"),0)==0);
    assert(js_audio_event(hap,0,2,&timing) && timing.delay_send==0);
    assert(cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(value,"delaytime"),2)==2);
    assert(js_audio_event(hap,0,2,&timing) && timing.delay_time==1 && timing.delay_send==.5);
    cJSON_DeleteItemFromObjectCaseSensitive(value,"delaytime");
    cJSON_DeleteItemFromObjectCaseSensitive(value,"delaysync");
    assert(js_audio_event(hap,0,8,&timing) && timing.delay_time==1);
    cJSON_Delete(hap);
    audio_toggle_play(1);
    for (int i=0;i<MAX_VOICES;i++) voices[i].active=0;
    orbits_ensure(); orbit_dt[N_ORBITS-1]=32/SR; orbit_fb[N_ORBITS-1]=0;
    float impulse[2]={1,1};
    Voice *voice=&voices[0]; memset(voice,0,sizeof(*voice));
    voice->active=1; voice->orbit=N_ORBITS-1; voice->smpbuf=impulse; voice->smplen=1;
    voice->smp_channels=1; voice->smpstep=1; voice->smp_end=1;
    voice->amp=.5; voice->delay_send=1; voice->note_off_time=voice->gate_end=10;
    float before[16]; pa_callback(NULL,before,8,NULL,0,NULL);
    assert(fabs(before[0]-.5)<1e-5);
    assert(slider_set(0,.8));
    assert(track_load_from_code("note(64).gain(slider(.8)).delay(0).gain(0)"));
    scheduler_transport_set(0);
    float after[80]; pa_callback(NULL,after,40,NULL,0,NULL);
    assert(fabs(after[24*2]-.5)<1e-4); /* Submitted echo survives both changes. */
    assert(fabs(after[24*2+1]-.5)<1e-4);
    audio_toggle_play(0); event_queue_clear();
}

static void test_disabled_delay_keeps_tail(void) {
    audio_time=0; audio_toggle_play(1); event_queue_clear();
    for (int i=0;i<MAX_VOICES;i++) voices[i].active=0;
    orbits_ensure(); orbit_dt[10]=32/SR; orbit_fb[10]=.5;
    float impulse[2]={1,1},output[256];
    Voice *voice=&voices[0]; memset(voice,0,sizeof(*voice));
    voice->active=1; voice->orbit=10; voice->smpbuf=impulse; voice->smplen=1;
    voice->smp_channels=1; voice->smpstep=1; voice->smp_end=1;
    voice->amp=.5; voice->delay_send=1; voice->note_off_time=voice->gate_end=10;
    pa_callback(NULL,output,8,NULL,0,NULL);
    cJSON *hap=cJSON_Parse("{\"begin\":0,\"end\":1,\"value\":{\"note\":60,\"gain\":0.5,\"dry\":0,\"delay\":1,\"delaytime\":0.01,\"delayfeedback\":0,\"orbit\":10}}");
    AudioEvent event; assert(hap && js_audio_event(hap,audio_time+4/SR,2,&event));
    assert(event.delay_send==0); audio_trigger_event(&event); cJSON_Delete(hap);
    pa_callback(NULL,output,128,NULL,0,NULL);
    assert(orbit_dt[10]==32/SR && orbit_fb[10]==.5);
    assert(fabs(output[24*2]-.5)<1e-5 && fabs(output[56*2]-.25)<1e-5);
    assert(fabs(output[88*2]-.125)<1e-5);
    for (int i=0;i<MAX_VOICES;i++) voices[i].active=0;
    orbit_fb[10]=0; pa_callback(NULL,output,64,NULL,0,NULL);
    audio_toggle_play(0); event_queue_clear();
}

static void reference_cps(int print) {
    event_queue_clear(); audio_toggle_play(0); audio_set_bpm(120); audio_time=0;
    assert(track_load_from_code("note(fastcat(60,64,67,72)).fast(10).gain(.5).cps(slider(.5,0,2))"));
    audio_toggle_play(1); live_tick(0);
    assert(slider_set(0,1)); live_tick(.1); live_tick(.2);
    assert(slider_set(0,.25)); live_tick(.3); live_tick(.4); live_tick(.5);
    assert(slider_set(0,0)); live_tick(.6); live_tick(.7); live_tick(.8);
    assert(audio_get_bpm()==0);
    assert(slider_set(0,.5)); audio_set_bpm(120); live_tick(.9); live_tick(1);
    if (print) {
        printf("[");
        for (QNode *node=qhead;node;node=node->next)
            printf("%s{\"time\":%.12g,\"duration\":%.12g,\"freq\":%.12g,\"gain\":%.12g}",node==qhead?"":",",node->ev.time,node->ev.duration,node->ev.freq,node->ev.gain);
        puts("]");
    }
    audio_toggle_play(0); event_queue_clear();
    assert(track_load_from_code("note(60).gain(0)"));
}

static struct { double time,cycle,cps,phase; int started; } performance_states[16];
static int performance_state_count;
static void performance_state(void) {
    assert(performance_state_count<16);
    int i=performance_state_count++;
    performance_states[i].time=audio_time;
    performance_states[i].cycle=tracks[0].js_cursor;
    performance_states[i].cps=audio_get_bpm()/240;
    performance_states[i].phase=tracks[0].js_phase;
    performance_states[i].started=audio_is_playing();
}
static void reference_performance(int print) {
    event_queue_clear(); audio_toggle_play(0); audio_set_bpm(120); audio_time=0;
    assert(track_load_from_code("stack(note(fastcat(60,64)).fast(10),note(fastcat(72,76)).fast(10).cps(slider(.5,0,2))).gain(.5)"));
    performance_state_count=0;
    scheduler_transport_set(1); live_tick(0); performance_state();
    assert(slider_set(0,.75)); live_tick(.1); performance_state();
    audio_set_bpm(480); live_tick(.2); performance_state();
    assert(slider_set(0,.25)); live_tick(.3); performance_state(); live_tick(.4); performance_state();
    assert(slider_set(0,0)); live_tick(.5); performance_state(); live_tick(.6); performance_state();
    assert(slider_set(0,1)); audio_set_bpm(240); live_tick(.7); performance_state();
    audio_time=.75; scheduler_transport_set(0); performance_state();
    int pending=event_queue_depth();
    live_tick(1); assert(event_queue_depth()==pending && tracks[0].js_cursor==0);
    audio_time=2; scheduler_transport_set(1); live_tick(2); performance_state();
    live_tick(2.1); performance_state();
    if (print) {
        printf("{\"events\":[");
        for (QNode *node=qhead;node;node=node->next)
            printf("%s{\"time\":%.12g,\"duration\":%.12g,\"note\":%d}",node==qhead?"":",",node->ev.time,node->ev.duration,ev_display_midi(&node->ev));
        printf("],\"states\":[");
        for (int i=0;i<performance_state_count;i++)
            printf("%s{\"time\":%.12g,\"cycle\":%.12g,\"cps\":%.12g,\"phase\":%.12g,\"started\":%s}",i?",":"",performance_states[i].time,performance_states[i].cycle,performance_states[i].cps,performance_states[i].phase,performance_states[i].started?"true":"false");
        puts("]}");
    }
    scheduler_transport_set(0); event_queue_clear();
    assert(track_load_from_code("note(60).gain(0)"));
}

static void reference_query_failure(int print) {
    event_queue_clear(); audio_toggle_play(0); audio_set_bpm(120); audio_time=0;
    assert(track_load_from_code("const originalQuery=globalThis.__query; globalThis.__query=(begin,...args)=>{if(begin<.025)throw Error('query failure');return originalQuery(begin,...args)}; note(fastcat(60,64,67,72)).fast(10).gain(.5)"));
    audio_toggle_play(1); live_tick(0);
    assert(event_queue_depth()==3 && fabs(tracks[0].js_cursor-.1)<1e-9);
    if (print) {
        printf("[");
        for (QNode *node=qhead;node;node=node->next)
            printf("%s{\"time\":%.12g,\"duration\":%.12g,\"freq\":%.12g,\"gain\":%.12g}",node==qhead?"":",",node->ev.time,node->ev.duration,node->ev.freq,node->ev.gain);
        puts("]");
    }
    audio_toggle_play(0); event_queue_clear();
    assert(track_load_from_code("note(60).gain(0)"));
}

/* External tempo pivot anchoring against the deployed web scheduler: steady
   0.5 cps, virtual ticks every 100 ms, external 0.5 -> 1 cps pivot after the
   tick at 1.0 s. The web anchors the pivot at its scheduling frontier
   (last queried end, next callback phase); the native anchor must produce
   the same onset times (web-measured 0.11/1.71/2.71). */
static void test_external_tempo_anchor(void) {
    event_queue_clear(); audio_toggle_play(0); audio_set_bpm(120); audio_time=0;
    assert(track_load_from_code("note(60).gain(.5)"));
    audio_toggle_play(1);
    for (int i = 0; i <= 10; i++) live_tick(i * .1);
    assert(fabs(tracks[0].js_cursor - .6) < 1e-9);
    assert(qhead && fabs(qhead->ev.time - .11) < 1e-9 && !qhead->next);
    audio_set_bpm(240);
    for (int i = 11; i <= 26; i++) live_tick(i * .1);
    double times[3] = {-1, -1, -1};
    int n = 0;
    for (QNode *node = qhead; node && n < 3; node = node->next) times[n++] = node->ev.time;
    assert(n == 3);
    assert(fabs(times[0] - .11) < 1e-9 && fabs(times[1] - 1.71) < 1e-9 && fabs(times[2] - 2.71) < 1e-9);
    audio_toggle_play(0); event_queue_clear();
    assert(track_load_from_code("note(60).gain(0)"));
}

static void test_timestamped_onsets(void) {
    event_queue_clear(); audio_toggle_play(0); audio_time=0;
    assert(track_load_from_code("note(69).gain(0)"));
    audio_toggle_play(1);
    for (int i=0;i<MAX_VOICES;i++) voices[i].active=0;
    next_voice_onset=HUGE_VAL;
    AudioEvent event={.generation=event_queue_generation(),.time=2048/SR,
        .inst=INST_SQUARE,.smp=-1,.wt=-1,.midi_chan=-1,.midi_note=-1,
        .freq=440,.gain=.1,.duration=1,.sus=1,.rel=.02,.delay_send=.1,
        .delay_time=.137,.delay_fb=.23,.orbit=3,.scope_mask=1u<<31,
        .duck_mask=1u<<4,.duck_depth=.7,.duck_attack=.2};
    dsp_params_init(&event.dsp); event.dsp.enabled=1;
    orbits_ensure(); web_delay_created[3]=1;
    orbit_dt[3]=(float).3; orbit_fb[3]=(float).4; pattern_scope_generation[31]=0;
    duck_depth[4]=0;
    event_queue_push(&event);
    assert(prepare_queued_audio(0) && !prepare_queued_audio(0));
    assert(qhead && qhead->ev.audio_prepared && voices[0].active);
    float source[8192];
    for (int i=0;i<8192;i++) source[i]=1;
    voices[0].smpbuf=source; voices[0].smplen=4096;
    voices[0].smp_channels=2; voices[0].smpstep=1;
    assert(voices[0].onset_pending && voices[0].age==0 && voices[0].phase==0);
    assert(orbit_dt[3]==(float).3 && orbit_fb[3]==(float).4 && pattern_scope_generation[31]==0);
    assert(duck_depth[4]==0);
    float output[8192];
    pa_callback(NULL,output,2048,NULL,0,NULL);
    for (int i=0;i<4096;i++) assert(output[i]==0);
    assert(voices[0].onset_pending && voices[0].age==0 && voices[0].phase==0);
    assert(orbit_dt[3]==(float).3 && orbit_fb[3]==(float).4 && pattern_scope_generation[31]==0);
    assert(duck_depth[4]==.7); /* Duck automation begins 10 ms before audio. */
    pa_callback(NULL,output+4096,2048,NULL,0,NULL);
    assert(!voices[0].onset_pending && fabs(voices[0].age-2048/SR)<1e-10);
    assert(orbit_dt[3]==(float).137 && orbit_fb[3]==(float).23);
    assert(pattern_scope_generation[31]==event.generation);
    assert(duck_depth[4]==.7 && duck_atk[4]==.2);
    assert(fabs(output[4096])>1e-4); /* First audible sample is the exact timestamp. */
    event_queue_clear();
    for (int i=0;i<MAX_VOICES;i++) voices[i].active=0;
    /* Chronological cut ordering must not depend on the selected pool slot. */
    audio_time=0; next_voice_onset=HUGE_VAL;
    event.delay_send=0; event.scope_mask=0; event.dsp.p[DSP_CUT]=7;
    event.time=3072/SR; audio_trigger_event(&event);
    event.time=1024/SR; audio_trigger_event(&event);
    assert(voices[0].active && voices[1].active);
    pa_callback(NULL,output,2048,NULL,0,NULL);
    assert(voices[0].onset_pending && voices[1].active && !voices[1].onset_pending);
    pa_callback(NULL,output,2048,NULL,0,NULL);
    assert(voices[0].active && voices[1].active); /* Oscillator cut is ignored. */
    for (int i=0;i<MAX_VOICES;i++) voices[i].active=0;
    /* Drain this test's orbit history before the independent echo fixture. */
    for (int i=0;i<40;i++) pa_callback(NULL,output,4096,NULL,0,NULL);
    audio_toggle_play(0);
}

static void test_silent_sample_cut(void) {
    char path[]="/tmp/tompouce-live-cut-XXXXXX.wav";
    int fd=mkstemps(path,4); assert(fd>=0);
    enum { frames=4096, bytes=frames*4 };
    unsigned char wav[44+bytes]={ 'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,
        1,0,2,0,68,172,0,0,16,177,2,0,4,0,16,0,'d','a','t','a',0,0,0,0 };
    for (int i=0;i<4;i++) {wav[4+i]=(unsigned char)((bytes+36)>>(8*i)); wav[40+i]=(unsigned char)(bytes>>(8*i));}
    for (int i=0;i<frames*2;i++) wav[44+i*2+1]=32; /* Stereo .25 PCM. */
    assert(write(fd,wav,sizeof(wav))==(ssize_t)sizeof(wav)); assert(close(fd)==0);
    int sample=smp_acquire_abs(path,60); assert(sample>=0); assert(unlink(path)==0);
    audio_toggle_play(1); event_queue_clear();
    for (int i=0;i<MAX_VOICES;i++) voices[i].active=0;
    audio_time=0; next_voice_onset=HUGE_VAL;
    cJSON *hap=cJSON_Parse("{\"begin\":0,\"end\":1,\"value\":{\"note\":60,\"gain\":0,\"cut\":37,\"attack\":0,\"decay\":0,\"sustain\":1,\"release\":0.02}}");
    AudioEvent silent; assert(hap && js_audio_event(hap,0,1,&silent)); cJSON_Delete(hap);
    assert(silent.gain==0 && silent.dsp.enabled);
    silent.smp=sample; silent.smp_speed=1; silent.smp_gate=1;
    silent.orbit=9; silent.delay_send=1; silent.delay_time=.017; silent.delay_fb=.25;
    orbits_ensure(); orbit_dt[9]=.3; orbit_fb[9]=.75;
    AudioEvent held=silent; held.gain=.4; held.delay_send=0;
    audio_trigger_event(&held);
    silent.time=1024/SR; audio_trigger_event(&silent);
    assert(voices[0].sampled_source && voices[1].sampled_source && voices[1].amp==0);
    assert(voices[0].cut_pending && voices[0].cut_time==silent.time && audio_time==0);
    float output[8192]; pa_callback(NULL,output,2048,NULL,0,NULL);
    for (int i=0;i<2048;i++) {
        double expected=.1*fmin(1,i/(SR*.001))*(i<1024?1:fmax(0,1-(i-1024)/(SR*.01)));
        if (fabs(output[i*2]-expected)>=1e-5 || fabs(output[i*2+1]-expected)>=1e-5)
            fprintf(stderr,"cut frame %d: %.9g %.9g expected %.9g\n",i,output[i*2],output[i*2+1],expected);
        assert(fabs(output[i*2]-expected)<1e-5 && fabs(output[i*2+1]-expected)<1e-5);
    }
    assert(voices[0].active && voices[0].cut_pending); /* Original graph lifetime. */
    assert(voices[1].active && voices[1].amp==0);
    assert(orbit_dt[9]==(float).017 && orbit_fb[9]==.25); /* Silent control automation. */
    for (int trial=0;trial<4;trial++) {
        for (int i=0;i<MAX_VOICES;i++) voices[i].active=0;
        audio_time=0; next_voice_onset=HUGE_VAL;
        web_duck[9].enabled=0; duck_depth[9]=0;
        held.dsp.p[DSP_CUT]=0; held.delay_send=0;
        silent=held; silent.gain=0; silent.time=.03;
        silent.duck_mask=1u<<9; silent.duck_depth=trial>=2?1:.25;
        silent.duck_attack=.02; silent.duck_onset=trial==1?.01:0;
        audio_trigger_event(&held); audio_trigger_event(&silent);
        if (trial==3) {
            AudioEvent cancel=silent; cancel.time=.05; cancel.duck_depth=0;
            audio_trigger_event(&cancel);
        }
        pa_callback(NULL,output,4096,NULL,0,NULL);
        double minimum=.03+silent.duck_onset,target=trial>=2?.01:.5;
        for (int i=0;i<4096;i++) {
            double time=i/SR,gain=1;
            if (time>=.02 && time<minimum) gain=pow(target,(time-.02)/(minimum-.02));
            else if (time>=minimum && time<minimum+.02) gain=pow(target,1-(time-minimum)/.02);
            if (trial==3 && time>=.04) {
                /* Depth-zero cancellation reads the previous rendered frame,
                   including when that frame lies on the old return ramp. */
                double anchor=(float)pow(.01,(.05-(.04-1/SR))/.02);
                gain=time<.05?anchor:time<.07?pow(anchor,1-(time-.05)/.02):1;
            }
            double expected=.1*fmin(1,time/.001)*gain;
            assert(fabs(output[i*2]-expected)<2e-6 && fabs(output[i*2+1]-expected)<2e-6);
        }
    }
    web_duck[9].enabled=0; duck_depth[9]=0;
    for (int i=0;i<MAX_VOICES;i++) voices[i].active=0;
    audio_toggle_play(0);
}

static void test_duck_control_lifetime(void) {
    event_queue_clear(); audio_time=0; audio_toggle_play(1);
    for (int i=0;i<MAX_VOICES;i++) voices[i].active=0;
    pending_duck_count=0; next_voice_onset=HUGE_VAL;
    web_duck[9].enabled=0; duck_depth[9]=0;
    AudioEvent event={.time=.04,.duration=1,.inst=INST_SINE,.freq=440,
        .smp=-1,.wt=-1,.midi_chan=-1,.midi_note=-1,.orbit=9,
        .duck_mask=1u<<9,.duck_depth=.25,.duck_attack=.02,.sus=1,.rel=.01};
    dsp_params_init(&event.dsp); event.dsp.enabled=1;
    audio_trigger_event(&event);
    assert(pending_duck_count==1 && voices[0].start_time==.04);
    event.time=.06; event.duck_mask=0;
    for (int i=0;i<MAX_VOICES;i++) audio_trigger_event(&event);
    assert(voices[0].start_time==.06); /* Original zero-gain voice was stolen. */
    assert(pending_duck_count==1);
    float output[8192]; pa_callback(NULL,output,2048,NULL,0,NULL);
    assert(pending_duck_count==0 && web_duck[9].enabled);
    assert(fabs(web_duck[9].minimum-.04)<1e-10 && fabs(web_duck[9].end-.06)<1e-10);
    assert(fabs(orbit_duck_gain(9)-pow(.5,1-(audio_time-.04)/.02))<1e-8);
    /* Admission is bounded and does not overwrite already accepted callbacks. */
    event.duck_mask=1u<<9;
    for (int i=0;i<MAX_PENDING_DUCKS;i++) {
        event.time=1+(MAX_PENDING_DUCKS-i)*.001;
        event.duck_depth=i==0?.25:1;
        event.duck_attack=i==0?.07:.02;
        assert(schedule_duck_locked(&event));
    }
    assert(!schedule_duck_locked(&event) && pending_duck_count==MAX_PENDING_DUCKS);
    audio_time=10; dispatch_ducks_locked();
    assert(pending_duck_count==0 && web_duck[9].minimum==10 && fabs(web_duck[9].end-10.07)<1e-10);
    for (int i=0;i<MAX_VOICES;i++) voices[i].active=0;
    audio_time=0; next_voice_onset=HUGE_VAL;
    assert(track_load_from_code("globalThis.__query=()=>JSON.stringify(["
        "{begin:0,end:.1,value:{s:'~',duckorbit:9,duckdepth:.25}},"
        "{begin:.025,end:.1,value:{s:'__missing_duck_qa__',duckorbit:9,duckdepth:1}}]); note(60)"));
    assert(js_audio_schedule_arc(tracks[0].javascript,.04,2,0,.05,event_queue_generation()));
    assert(event_queue_depth()==4 && qhead->ev.control_only && qhead->next->ev.eviction_only);
    for(int i=0;i<4;i++) assert(prepare_queued_audio(0));
    assert(!prepare_queued_audio(0));
    assert(pending_duck_count==2);
    event_queue_clear(); /* Accepted timer callbacks survive producer invalidation. */
    pa_callback(NULL,output,4096,NULL,0,NULL);
    assert(pending_duck_count==0 && fabs(web_duck[9].minimum-.09)<1e-10);
    for (int i=0;i<MAX_VOICES;i++) assert(!voices[i].active);
    web_duck[9].enabled=0; duck_depth[9]=0;
    for (int i=0;i<MAX_VOICES;i++) voices[i].active=0;
    audio_toggle_play(0);
}

static void test_source_eviction_schedule(void) {
    event_queue_clear(); audio_time=0; audio_toggle_play(1);
    memset(voices,0,sizeof(voices)); next_voice_onset=HUGE_VAL; voice_onset_serial=0;
    assert(track_load_from_code("globalThis.__query=()=>JSON.stringify(["
        "...Array.from({length:128},(_,i)=>({begin:.1,end:1,value:{s:'sine',freq:100+i,gain:0}})),"
        "{begin:.2,end:1,value:{s:'~'}},"
        "{begin:.3,end:1,value:{s:'sine',freq:1000,gain:0}},"
        "{begin:.4,end:1,value:{s:'__missing_eviction_qa__'}},"
        "{begin:.5,end:1,value:{s:'sine',freq:1001,gain:0}},"
        "{begin:.6,end:1,value:{s:'sine',freq:1002,gain:0,stretch:1}}]); note(60)"));
    assert(js_audio_schedule_arc(tracks[0].javascript,1,1,0,1,event_queue_generation()));
    assert(event_queue_depth()==133);
    for(QNode *node=qhead;node;node=node->next) if(node->ev.eviction_only) {
        assert(node->ev.control_only && node->ev.midi_chan==-1 && node->ev.midi_note==-1);
    }
    while(prepare_queued_audio(2)) {}
    assert(audio_active_voices()==128);
    assert(voices[MAX_VOICES].freq==100 && fabs(voices[MAX_VOICES].retire_end-1.45)<1e-12);
    assert(voices[MAX_VOICES+1].freq==101 && fabs(voices[MAX_VOICES+1].retire_end-1.65)<1e-12);
    assert(voices[MAX_VOICES+2].freq==102 && fabs(voices[MAX_VOICES+2].retire_end-1.81)<1e-12);
    assert(!voices[MAX_VOICES+3].active);
    event_queue_clear(); memset(voices,0,sizeof(voices)); next_voice_onset=HUGE_VAL;
    audio_toggle_play(0);
}

static void test_duck_target_existence(void) {
    event_queue_clear(); audio_time=0; audio_toggle_play(1);
    unsigned previous=atomic_exchange(&created_orbits,0);
    assert(track_load_from_code("globalThis.__query=()=>JSON.stringify(["
        "{begin:0,end:.1,value:{s:'sine',gain:0,orbit:1,duckorbit:9}},"
        "{begin:.1,end:.2,value:{s:'~',orbit:9}},"
        "{begin:.2,end:.3,value:{s:'~',orbit:1,duckorbit:9}},"
        "{begin:.3,end:.4,value:{s:'__missing_duck_qa__',orbit:7,duckorbit:7}}]); note(60)"));
    assert(js_audio_schedule_arc(tracks[0].javascript,1,1,0,.5,event_queue_generation()));
    assert(event_queue_depth()==6 && qhead && !qhead->ev.control_only);
    assert(qhead->ev.duck_mask==0); /* Later creation cannot revive a skipped duck. */
    assert(qhead->next->ev.eviction_only);
    assert(qhead->next->next->ev.duck_mask==(1u<<9));
    assert(qhead->next->next->next->next->ev.duck_mask==(1u<<7));
    assert(atomic_load(&created_orbits)==((1u<<1)|(1u<<7)|(1u<<9)));
    event_queue_clear(); audio_toggle_play(0);
    assert(track_load_from_code("s('~').duckorbit(9)")); audio_toggle_play(1);
    js_audio_schedule(tracks[0].javascript,2,0,event_queue_generation());
    assert(event_queue_depth()==2 && qhead->ev.control_only && qhead->ev.duck_mask==(1u<<9));
    event_queue_clear(); atomic_fetch_or(&created_orbits,previous);
}

static void test_duck_timer_failure(void) {
    event_queue_clear(); audio_wait_room_preparation(); audio_report_errors();
    audio_time=0; audio_toggle_play(1); pending_duck_count=0;
    unsigned previous=atomic_exchange(&created_orbits,(1u<<1)|(1u<<9));
    for(int i=0;i<VOICE_SLOTS;i++) {
        native_synth_state_free(voices[i].native_state); native_synth_state_free(voices[i].native_right);
        stretch_release(voices[i].stretch_buffer);
    }
    memset(voices,0,sizeof(voices)); next_voice_onset=HUGE_VAL;
    AudioEvent held={.time=.5,.duration=1,.inst=INST_SINE,.freq=440,.smp=-1,
        .midi_chan=-1,.midi_note=-1,.orbit=1,.room_prepared=1,.delay_prepared=1};
    dsp_params_init(&held.dsp); held.dsp.enabled=1;
    for(int i=0;i<MAX_VOICES;i++) audio_trigger_event(&held);
    unsigned long long serial=voices[0].onset_serial,revision=atomic_load(&ir_revision[7]);
    double delay=orbit_dt[7],feedback=orbit_fb[7];
    int had_room=room_created[7],had_delay=web_delay_created[7];
    web_duck[9].enabled=web_duck[9].hold=1; web_duck[9].initial=web_duck[9].target=.8;
    AudioEvent accepted=held; accepted.control_only=1; accepted.time=.04;
    accepted.duck_mask=1u<<9; accepted.duck_depth=.25; accepted.duck_attack=.1;
    audio_trigger_event(&accepted); assert(pending_duck_count==1);
    AudioEvent rejected=accepted; rejected.control_only=0; rejected.time=.005; rejected.orbit=7;
    rejected.room_prepared=rejected.delay_prepared=0; rejected.delay_send=1;
    rejected.delay_time=.123; rejected.delay_fb=.5; rejected.dsp.p[DSP_ROOM]=1;
    rejected.dsp.p[DSP_SIZE]=.05; rejected.room_rebuild=rejected.room_params_valid=1; rejected.room_sample=-1;
    audio_trigger_event(&rejected);
    assert(duck_error_count==1 && duck_errors[0].timer && duck_errors[0].orbit==9 && duck_errors[0].time==0);
    assert(pending_duck_count==1 && orbit_duck_gain(9)==.8 && (atomic_load(&created_orbits)&(1u<<7)));
    assert(voices[0].onset_serial==serial);
    for(int i=0;i<MAX_VOICES;i++) assert(voices[i].active && voices[i].start_time==.5);
    for(int i=MAX_VOICES;i<VOICE_SLOTS;i++) assert(!voices[i].active);
    assert(atomic_load(&ir_revision[7])==revision && room_created[7]==had_room && web_delay_created[7]==had_delay);
    assert(orbit_dt[7]==delay && orbit_fb[7]==feedback);
    audio_report_errors();
    /* Failure aborts this hap, while later haps and repeated valid targets
       still commit their independent callbacks. */
    assert(track_load_from_code("globalThis.__query=()=>JSON.stringify(["
        "{begin:0,end:.1,value:{s:'sine',orbit:7,duckorbit:[9,9],room:1}},"
        "{begin:.015,end:.1,value:{s:'~',orbit:7,duckorbit:[9,9]}}]); note(60)"));
    assert(js_audio_schedule_arc(tracks[0].javascript,.005,1,0,.1,event_queue_generation()));
    assert(duck_error_count==1 && duck_errors[0].timer && event_queue_depth()==3);
    for(QNode *node=qhead;node;node=node->next) assert(node->ev.control_only && fabs(node->ev.time-.02)<1e-12);
    assert(prepare_queued_audio(0) && prepare_queued_audio(0) && pending_duck_count==3);
    assert(prepare_queued_audio(0) && !prepare_queued_audio(0));
    audio_report_errors(); audio_time=.2; audio_trigger_event(&rejected);
    assert(duck_error_count==1 && duck_errors[0].timer && duck_errors[0].time==.2 && pending_duck_count==3);
    audio_report_errors(); rejected.orbit=6;
    rejected.osc_only=1; audio_trigger_event(&rejected);
    rejected.osc_only=0; rejected.csound_only=1; audio_trigger_event(&rejected);
    assert(!duck_error_count && !(atomic_load(&created_orbits)&(1u<<6)));
    event_queue_clear(); pending_duck_count=0; memset(voices,0,sizeof(voices)); next_voice_onset=HUGE_VAL;
    web_duck[9].enabled=0; duck_depth[9]=0; atomic_store(&created_orbits,previous);
    audio_toggle_play(0); assert(track_load_from_code("note(60).gain(0)"));
}

static void test_duck_target_coercion(void) {
    const char *targets[]={"1","\"1\"","\"01\"","\"1:2\"","[1,2]",
        "[[1],[2]]","[[1,2]]","[null,1]","[\"2\",\"1\"]"};
    const unsigned expected[]={2,2,0,0,6,6,0,2,6};
    for (size_t i=0;i<sizeof(expected)/sizeof(expected[0]);i++) {
        char json[512];
        snprintf(json,sizeof(json),"{\"begin\":0,\"end\":1,\"value\":{\"s\":\"sine\",\"duckorbit\":%s}}",targets[i]);
        cJSON *hap=cJSON_Parse(json); assert(hap); AudioEvent event;
        assert(js_audio_event(hap,0,1,&event));
        assert(event.duck_mask==expected[i]);
        cJSON_Delete(hap);
    }
}

static void test_ordered_duck_targets(void) {
    event_queue_clear(); pending_duck_count=0; audio_time=0; audio_toggle_play(1);
    audio_register_orbit(2);
    assert(track_load_from_code("s('~').duckorbit([2,1,2]).duckonset([.02,.04,.06]).duckattack([.1,.2,.3]).duckdepth([.25,.5,1])"));
    js_audio_schedule(tracks[0].javascript,1,0,event_queue_generation());
    assert(event_queue_depth()==4);
    const int targets[]={2,1,2}; const double onset[]={.02,.04,.06};
    const double attack[]={.1,.2,.3},depth[]={.25,.5,1};
    QNode *node=qhead;
    for (int i=0;i<3;i++,node=node->next) {
        assert(node && node->ev.control_only && node->ev.duck_mask==(1u<<targets[i]));
        DuckControl control=node->ev.ducks[targets[i]];
        assert(control.onset==onset[i] && control.attack==attack[i] && control.depth==depth[i]);
        assert(prepare_queued_audio(.95));
    }
    assert(pending_duck_count==3);
    audio_time=1; dispatch_ducks_locked();
    assert(pending_duck_count==0 && fabs(web_duck[2].minimum-1.06)<1e-10);
    assert(fabs(web_duck[2].end-1.36)<1e-10);
    assert(fabs(web_duck[1].minimum-1.04)<1e-10);
    event_queue_clear();
}

static void test_duck_numeric_controls(void) {
    const char *values[]={"0","0.25","1","\"0.25\"","null","false","true","[0.25]","[[0.25]]","[null]"};
    const double numbers[]={0,.25,1,.25,0,0,1,.25,.25,0};
    for (size_t i=0;i<sizeof(numbers)/sizeof(numbers[0]);i++) {
        char json[512]; snprintf(json,sizeof(json),
            "{\"begin\":0,\"end\":1,\"value\":{\"s\":\"sine\",\"duckorbit\":1,\"duckattack\":%s,\"duckdepth\":%s}}",values[i],values[i]);
        cJSON *hap=cJSON_Parse(json); assert(hap); AudioEvent event;
        assert(js_audio_event(hap,.05,1,&event)); cJSON_Delete(hap);
        assert(event.ducks[1].depth==numbers[i]);
        assert(event.ducks[1].attack==fmax(.002,numbers[i]));
        audio_time=0; pending_duck_count=0;
        web_duck[1].enabled=0; duck_depth[1]=0;
        assert(schedule_duck_locked(&event));
        audio_time=.04; dispatch_ducks_locked();
        assert(pending_duck_count==0 && web_duck[1].target==fmax(.01,1-sqrt(numbers[i])));
        assert(fabs(web_duck[1].end-(.05+fmax(.002,numbers[i])))<1e-10);
    }
}

static void test_invalid_duck_controls(void) {
    audio_report_errors();
    const char *controls[]={"\"duckdepth\":-1","\"duckdepth\":\"bad\"",
        "\"duckdepth\":[[true]]","\"duckdepth\":0.25,\"duckattack\":\"bad\"",
        "\"duckdepth\":0.25,\"duckattack\":\"Infinity\""};
    for (size_t i=0;i<sizeof(controls)/sizeof(controls[0]);i++) {
        char json[512]; snprintf(json,sizeof(json),
            "{\"begin\":0,\"end\":1,\"value\":{\"s\":\"sine\",\"duckorbit\":1,%s}}",controls[i]);
        cJSON *hap=cJSON_Parse(json); assert(hap); AudioEvent event;
        assert(js_audio_event(hap,.3,1,&event)); cJSON_Delete(hap);
        memset(&web_duck[1],0,sizeof(web_duck[1]));
        web_duck[1].enabled=1; web_duck[1].hold=1;
        web_duck[1].initial=web_duck[1].target=.8;
        pending_duck_count=0; audio_time=.28;
        int before=console_head_get();
        assert(schedule_duck_locked(&event)); dispatch_ducks_locked();
        assert(!duck_error_count && pending_duck_count==1);
        audio_report_errors(); assert(console_head_get()==before);
        audio_time=.29; dispatch_ducks_locked();
        assert(console_head_get()==before); /* No callback-side logging. */
        assert(duck_error_count==1 && duck_errors[0].orbit==1);
        assert(duck_errors[0].returning==(i>=3));
        assert(fabs(duck_errors[0].time-.29)<1e-12);
        audio_report_errors();
        assert(duck_error_count==0 && console_head_get()!=before);
        assert(strstr(console_line(console_count_get()-1),i>=3?"return ramp":"downward ramp"));
        before=console_head_get(); audio_report_errors();
        assert(console_head_get()==before); /* Each report is consumed once. */
        audio_time=.45;
        double gain=orbit_duck_gain(1);
        assert(isfinite(gain) && fabs(gain-(i<3?(double)(float).8:.5))<1e-8);
        assert(web_duck[1].hold);
    }
    /* A burst cannot allocate or overwrite the fixed callback mailbox. */
    for (int i=0;i<DUCK_ERROR_CAPACITY+2;i++) record_duck_error(1,0);
    assert(duck_error_count==DUCK_ERROR_CAPACITY && duck_error_overflow);
    audio_report_errors();
    assert(!duck_error_count && !duck_error_overflow);
    assert(strstr(console_line(console_count_get()-1),"additional automation errors"));
    PendingDuck late={.time=.1,.source_time=.11,.mask=1u<<1};
    late.controls[1]=(DuckControl){.depth=-1,.attack=.1};
    audio_time=2; apply_duck(&late);
    assert(duck_error_count==1 && duck_errors[0].time==2);
    audio_report_errors();
}

static void test_boolean_duck_onset(void) {
    const char *values[]={"true","false","null","[true]"};
    const double onset[]={1,0,0,1};
    for (size_t i=0;i<sizeof(onset)/sizeof(onset[0]);i++) {
        char json[512]; snprintf(json,sizeof(json),
            "{\"begin\":0,\"end\":1,\"value\":{\"s\":\"sine\",\"duckorbit\":1,\"duckonset\":%s}}",values[i]);
        cJSON *hap=cJSON_Parse(json); assert(hap); AudioEvent event;
        assert(js_audio_event(hap,.3,1,&event)); cJSON_Delete(hap);
        assert(event.ducks[1].onset==onset[i]);
        audio_time=.29; pending_duck_count=0; web_duck[1].enabled=0; duck_depth[1]=0;
        assert(schedule_duck_locked(&event)); dispatch_ducks_locked();
        assert(fabs(web_duck[1].minimum-(.3+onset[i]))<1e-10);
        assert(fabs(web_duck[1].end-(.4+onset[i]))<1e-10);
    }
}

static void test_nonfinite_duck_numbers(void) {
    audio_report_errors();
    const char *values[]={"NaN","Infinity","-Infinity","[NaN]","[[Infinity]]"};
    const double expected[]={NAN,INFINITY,-INFINITY,NAN,INFINITY};
    for (unsigned i=0;i<sizeof(expected)/sizeof(expected[0]);i++) {
        char source[256]; snprintf(source,sizeof(source),
            "s('~').duckorbit(1).duckdepth(%s).duckattack(%s).duckonset(%s)",
            values[i],values[i],values[i]);
        cJSON *metadata=NULL; char error[512];
        JsProgram *program=javascript_compile(source,120,&metadata,error,sizeof(error));
        assert(program); cJSON_Delete(metadata);
        cJSON *haps=javascript_query(program,0,1); assert(haps);
        AudioEvent event; assert(js_audio_event(cJSON_GetArrayItem(haps,0),0,1,&event));
        DuckControl control=event.ducks[1];
        assert(isnan(expected[i])?isnan(control.depth):control.depth==expected[i]);
        double attack=isnan(expected[i])?NAN:fmax(.002,expected[i]);
        assert(isnan(attack)?isnan(control.attack):control.attack==attack);
        web_duck[1].enabled=0; duck_depth[1]=0;
        PendingDuck pending={.time=0,.source_time=.1,.mask=1u<<1};
        pending.controls[1]=control; audio_time=.09; apply_duck(&pending);
        assert(web_duck[1].hold && orbit_duck_gain(1)==1);
        assert(duck_error_count==1 && !duck_errors[0].returning);
        audio_report_errors();
        cJSON_Delete(haps); javascript_free(program);
    }
    const char *sources[]={"s('~').duckorbit(1).duckdepth(Infinity)",
        "s('~').duckorbit(1).duckattack(-Infinity)"};
    for (unsigned i=0;i<2;i++) {
        cJSON *metadata=NULL; char error[512];
        JsProgram *program=javascript_compile(sources[i],120,&metadata,error,sizeof(error));
        assert(program); cJSON_Delete(metadata);
        cJSON *haps=javascript_query(program,0,1); assert(haps);
        AudioEvent event; assert(js_audio_event(cJSON_GetArrayItem(haps,0),0,1,&event));
        web_duck[1].enabled=0; duck_depth[1]=0;
        PendingDuck pending={.time=0,.source_time=.1,.mask=1u<<1};
        pending.controls[1]=event.ducks[1]; audio_time=.09; apply_duck(&pending);
        assert(!web_duck[1].hold && !duck_error_count);
        assert(web_duck[1].target==.01);
        assert(fabs(web_duck[1].end-(i?.102:.2))<1e-12);
        cJSON_Delete(haps); javascript_free(program);
    }
}

static void test_string_duck_onset(void) {
    const char *values[]={"\"1\"","\"\"","[[0.02]]","[[null]]","\"1\""};
    const double attacks[]={.1,.1,.1,.1,1},expected[]={.5,.5,.8,.5,1};
    for (size_t i=0;i<sizeof(expected)/sizeof(expected[0]);i++) {
        char json[512]; snprintf(json,sizeof(json),
            "{\"begin\":0,\"end\":1,\"value\":{\"s\":\"sine\",\"duckorbit\":1,\"duckdepth\":0.25,\"duckattack\":%.1f,\"duckonset\":%s}}",attacks[i],values[i]);
        cJSON *hap=cJSON_Parse(json); assert(hap); AudioEvent event;
        assert(js_audio_event(hap,.3,1,&event)); cJSON_Delete(hap);
        assert(event.ducks[1].string_onset);
        memset(&web_duck[1],0,sizeof(web_duck[1]));
        web_duck[1].enabled=1; web_duck[1].hold=1;
        web_duck[1].initial=web_duck[1].target=.8;
        pending_duck_count=0; audio_time=.29;
        assert(schedule_duck_locked(&event)); dispatch_ducks_locked();
        audio_time=.45;
        assert(fabs(orbit_duck_gain(1)-expected[i])<2e-8);
        if (i==4) assert(web_duck[1].minimum==.31 && web_duck[1].end==.311);
    }
}

static void test_interrupted_duck_anchor(void) {
    for (int falling=0;falling<2;falling++) for (int failure=0;failure<3;failure++) {
        audio_report_errors();
        memset(&web_duck[1],0,sizeof(web_duck[1]));
        web_duck[1].enabled=web_duck[1].hold=1;
        web_duck[1].initial=web_duck[1].target=.8;
        double begin=falling?.28:.09,minimum=falling?.3:.1;
        PendingDuck prior={.source_time=minimum,.mask=2};
        prior.controls[1]=(DuckControl){.attack=.6,.depth=.75};
        audio_time=begin; apply_duck(&prior);
        /* Independent exponential automation at the last completed frame. */
        double last=.29-1/SR,target=1-sqrt(.75);
        double expected=falling?(float).8*pow(target/(float).8,(last-begin)/(minimum-begin)):
            target*pow(1/target,(last-minimum)/.6);
        PendingDuck next={.source_time=.3,.mask=2};
        next.controls[1]=(DuckControl){.attack=failure==2?NAN:.2,.depth=failure==1?-1:.25};
        audio_time=.29; apply_duck(&next);
        assert(web_duck[1].initial==(double)(float)expected);
        assert(duck_error_count==(unsigned)(failure!=0));
        if(failure) {
            assert(duck_errors[0].returning==(failure==2));
            assert(web_duck[1].hold);
            audio_time=.9;
            assert(orbit_duck_gain(1)==fmin((double)(float)expected,.5));
        }
    }
    audio_report_errors();
}

static void test_long_duck_onset(void) {
    typedef struct { double onset,attack,depth; int string_onset; char text[128]; } InlineControl;
    _Static_assert(sizeof(DuckControl)==sizeof(InlineControl),"Long onsets must not enlarge queued controls");
    char text[10001]; memset(text,'0',sizeof(text)-1); text[sizeof(text)-1]=0;
    cJSON *hap=cJSON_CreateObject(),*value=cJSON_CreateObject(); assert(hap && value);
    cJSON_AddNumberToObject(hap,"begin",0); cJSON_AddNumberToObject(hap,"end",1);
    cJSON_AddItemToObject(hap,"value",value); cJSON_AddStringToObject(value,"s","~");
    cJSON_AddNumberToObject(value,"duckorbit",1); cJSON_AddNumberToObject(value,"duckdepth",.25);
    cJSON_AddStringToObject(value,"duckonset",text);
    AudioEvent event; assert(js_audio_event(hap,.3,1,&event));
    assert(event.ducks[1].string_onset==DUCK_ONSET_SUMMARY);
    cJSON_Delete(hap); /* No borrowed JSON/string pointers may reach the callback. */
    audio_report_errors(); pending_duck_count=0; audio_time=0;
    memset(&web_duck[1],0,sizeof(web_duck[1]));
    web_duck[1].enabled=web_duck[1].hold=1;
    web_duck[1].initial=web_duck[1].target=.8;
    assert(schedule_duck_locked(&event)); assert(pending_duck_count==1);
    memset(&event,0,sizeof(event)); audio_time=.29; dispatch_ducks_locked();
    assert(!pending_duck_count && web_duck[1].minimum==.3);
    assert(duck_error_count==1 && duck_errors[0].returning);
    audio_time=.45; assert(orbit_duck_gain(1)==.5); audio_report_errors();
}

static void test_nul_duck_controls(void) {
    /* Construct strings at query time; literal strings belong to upstream
       mini-notation, whose grammar correctly rejects embedded NULs. */
    const char *values[]={"String.fromCharCode(49,0,116)","[String.fromCharCode(49,0,116)]",
        "[[String.fromCharCode(49,0,116)]]"};
    for (unsigned i=0;i<sizeof(values)/sizeof(values[0]);i++) {
        char source[512],error[512]; cJSON *metadata=NULL;
        snprintf(source,sizeof(source),"const v=%s; s('~').fmap(()=>({s:'~',duckorbit:1,duckonset:v,duckattack:v,duckdepth:v}))",values[i]);
        JsProgram *program=javascript_compile(source,120,&metadata,error,sizeof(error));
        if (!program) fprintf(stderr,"NUL fixture: %s\n",error);
        assert(program);
        cJSON_Delete(metadata); cJSON *haps=javascript_query(program,0,1); assert(haps);
        AudioEvent event; assert(js_audio_event(cJSON_GetArrayItem(haps,0),.3,1,&event));
        assert(isnan(event.ducks[1].depth) && isnan(event.ducks[1].attack));
        assert(event.ducks[1].string_onset==DUCK_ONSET_STRING && !strcmp(event.ducks[1].onset_text,"NaN"));
        cJSON_Delete(haps); javascript_free(program);
    }
}

static void test_nested_duck_controls(void) {
    const char *inputs[]={"undefined","[]","[undefined]","[null]","[.25,null]","[null,.25]",
        "[[null]]","[[]]","[[[]]]","[[false]]","[[.25]]","[[[.25]]]","[[1,2]]","{}","[[{}]]",
        "Array(2)","[undefined,.25]"};
    const int stages[][2]={{0,0},{2,2},{2,2},{0,0},{0,0},{0,0},{2,2},{2,2},{2,2},
        {1,1},{1,1},{1,1},{1,1},{1,1},{1,1},{2,2},{2,0}};
    const double attacks[]={.1,NAN,NAN,.002,.25,.002,.002,.002,.002,NAN,.25,.25,NAN,NAN,NAN,NAN,NAN};
    audio_report_errors();
    for (unsigned i=0;i<sizeof(inputs)/sizeof(inputs[0]);i++) {
        char source[512]; snprintf(source,sizeof(source),
            "const v=%s; s('~').set({duckorbit:[1,2],duckonset:v,duckattack:v,duckdepth:v})",inputs[i]);
        cJSON *metadata=NULL; char error[512];
        JsProgram *program=javascript_compile(source,120,&metadata,error,sizeof(error));
        assert(program); cJSON_Delete(metadata);
        cJSON *haps=javascript_query(program,0,1); assert(haps);
        AudioEvent event; assert(js_audio_event(cJSON_GetArrayItem(haps,0),0,1,&event));
        assert(event.duck_mask==6);
        for (int o=1;o<=2;o++) {
            double attack=o==2 && (i==5 || i==16)?.25:attacks[i];
            assert(isnan(attack)?isnan(event.ducks[o].attack):event.ducks[o].attack==attack);
            web_duck[o].enabled=1; web_duck[o].hold=1;
            web_duck[o].initial=web_duck[o].target=.8;
            PendingDuck pending={.source_time=.3,.mask=1u<<o};
            pending.controls[o]=event.ducks[o]; audio_time=0; apply_duck(&pending);
            int stage=stages[i][o-1];
            assert(duck_error_count==(unsigned)(stage!=0));
            if (stage) {
                assert(duck_errors[0].returning==(stage==2));
                assert(web_duck[o].hold);
            } else assert(!web_duck[o].hold);
            audio_report_errors();
        }
        cJSON_Delete(haps); javascript_free(program);
    }
    /* Raw serialized empty arrays must also retain the controller's NaN
       attack, independently of the embedded runtime's undefined encoding. */
    cJSON *hap=cJSON_Parse("{\"begin\":0,\"end\":1,\"value\":{\"s\":\"~\",\"duckorbit\":1,\"duckattack\":[],\"duckdepth\":[]}}");
    AudioEvent event; assert(hap && js_audio_event(hap,0,1,&event));
    assert(isnan(event.ducks[1].attack) && event.ducks[1].depth==1);
    cJSON_Delete(hap);
}

static void test_duck_output_routing(void) {
    const char *sources[]={
        "note(60).osc().orbit(7).duckorbit(1)",
        "note(60).csound('Fixture').orbit(7).duckorbit(1)",
        "s('~').orbit(7).duckorbit(1)",
        "s('~').midichan(1).orbit(7).duckorbit(1)"};
    unsigned previous=atomic_load(&created_orbits);
    event_queue_clear(); audio_time=0; audio_toggle_play(1);
    for (unsigned i=0;i<sizeof(sources)/sizeof(sources[0]);i++) {
        atomic_store(&created_orbits,1u<<1);
        cJSON *metadata=NULL; char error[512];
        JsProgram *program=javascript_compile(sources[i],120,&metadata,error,sizeof(error));
        assert(program); cJSON_Delete(metadata);
        assert(js_audio_schedule(program,1,0,event_queue_generation()));
        if (i<2) {
            assert(atomic_load(&created_orbits)==(1u<<1));
            assert(event_queue_depth()==(i==0?1:0));
            if (i==0) assert(qhead->ev.osc_only && !qhead->ev.control_only);
        } else {
            assert(atomic_load(&created_orbits)==((1u<<1)|(1u<<7)));
            assert(event_queue_depth()==2 && qhead->ev.control_only && qhead->ev.duck_mask==(1u<<1));
        }
        event_queue_clear(); javascript_free(program);
    }
#ifdef HAVE_CSOUND
    /* A valid Csound event with a literal local rest still reaches its own
       renderer. Neither renderer failure nor success creates local orbits. */
    const char *source="await loadCsound('instr Fixture\\na1 init 0\\nouts a1,a1\\nendin');"
        "note(60).s('~').csound('Fixture').midichan(1).orbit(7).duckorbit(1)";
    cJSON *metadata=NULL; char error[512];
    JsProgram *program=javascript_compile(source,120,&metadata,error,sizeof(error));
    assert(program && javascript_prepare_csound(program,metadata,error,sizeof(error)));
    cJSON_Delete(metadata); atomic_store(&created_orbits,1u<<1);
    assert(js_audio_schedule(program,1,0,event_queue_generation()));
    assert(atomic_load(&created_orbits)==(1u<<1));
    assert(event_queue_depth()==1 && qhead->ev.csound_only && !qhead->ev.duck_mask);
    assert(qhead->ev.midi_chan==-1 && qhead->ev.midi_note==-1);
    event_queue_clear(); javascript_free(program);
#endif
    atomic_store(&created_orbits,previous); audio_toggle_play(0);
}

static void test_javascript_numeric_strings(void) {
    const char *text[]={"\xc2\xa0.25\xef\xbb\xbf","\xe2\x80\xa8.25\xe2\x80\xa9",
        "\xe2\x80\x83","\xc2\x85.25","0b10","0o10","0x10","+0x10","-0b1",
        "0x1.8","0x1p2","Infinity","-Infinity","1_0",""," \t.25\r\n"};
    const double expected[]={.25,.25,0,NAN,2,8,16,NAN,NAN,NAN,NAN,INFINITY,-INFINITY,NAN,0,.25};
    for (size_t i=0;i<sizeof(expected)/sizeof(expected[0]);i++) {
        double actual=js_number_parse(text[i]);
        assert(isnan(expected[i])?isnan(actual):actual==expected[i]);
    }
}

static void test_room_preparation(void) {
    event_queue_clear(); audio_time=0; audio_toggle_play(1);
    assert(track_load_from_code("note(60).gain(0).room(1).roomsize(.1).roomfade(.02).roomlp(9000).roomdim(500).orbit(10)"));
    js_audio_schedule(tracks[0].javascript,1,0,event_queue_generation());
    assert(qhead && qhead->ev.room_prepared && qhead->ev.time>=1);
    audio_wait_room_preparation();
    const ConvKernel *first=atomic_load(&requested_ir[10]);
    assert(first && atomic_load(&requested_ir[10])==first);
    double old_energy=0;
    for (int frame=0;frame<1500;frame++) {
        VoiceMix mix={.room_l=frame==0?1:0,.room_r=frame==0?-1:0};
        native_reverb(10,&mix);
        if (frame>1000) old_energy+=mix.left*mix.left+mix.right*mix.right;
    }
    assert(old_energy>0 && convolvers[10].remaining>0);
    ConvState *pending_history=malloc(sizeof(*pending_history)); assert(pending_history);
    memcpy(pending_history,&convolvers[10],sizeof(*pending_history));
    const ConvKernel *pending=convolution_prepare_reverb(.15,.01,9000,500);
    assert(pending && pending!=first && atomic_load(&requested_ir[10])==first);
    double pending_energy=0;
    for (int frame=0;frame<512;frame++) {
        VoiceMix mix={0}; double left,right;
        native_reverb(10,&mix);
        convolution_process(pending_history,0,0,&left,&right);
        assert(mix.left==left && mix.right==right);
        pending_energy+=left*left+right*right;
    }
    assert(pending_energy>0); free(pending_history);
    AudioEvent future=qhead->ev;
    event_queue_clear();
    assert(track_load_from_code("note(60).gain(0).room(1).roomsize(.1).orbit(10)"));
    js_audio_schedule(tracks[0].javascript,1.5,0,event_queue_generation());
    assert(qhead && !qhead->ev.room_rebuild && atomic_load(&requested_ir[10])==first);
    assert(qhead->ev.dsp.p[DSP_ROOMFADE]==.02 && qhead->ev.dsp.p[DSP_ROOMDIM]==500);
    event_queue_clear();
    assert(track_load_from_code("note(60).gain(0).room(1).roomsize(.2).orbit(10)"));
    js_audio_schedule(tracks[0].javascript,2,0,event_queue_generation());
    assert(qhead && qhead->ev.room_prepared);
    audio_wait_room_preparation();
    const ConvKernel *second=atomic_load(&requested_ir[10]);
    assert(second && second!=first && atomic_load(&requested_ir[10])==second);
    assert(qhead->ev.dsp.p[DSP_ROOMFADE]==.1 && qhead->ev.dsp.p[DSP_ROOMLP]==15000);
    assert(qhead->ev.dsp.p[DSP_ROOMDIM]==1000);
    VoiceMix changed={0}; native_reverb(10,&changed);
    assert(convolvers[10].kernel==second && !convolvers[10].remaining);
    assert(changed.left==0 && changed.right==0);
    for (int frame=0;frame<1500;frame++) {
        VoiceMix mix={.room_l=frame==0?1:0,.room_r=frame==0?-1:0};
        native_reverb(10,&mix);
    }
    ConvState *unchanged=malloc(sizeof(*unchanged)); assert(unchanged);
    memcpy(unchanged,&convolvers[10],sizeof(*unchanged));
    event_queue_clear();
    audio_toggle_play(0);
    assert(track_load_from_code("note(64).gain(0).room(1).orbit(10)"));
    audio_toggle_play(1);
    js_audio_schedule(tracks[0].javascript,3,0,event_queue_generation());
    assert(qhead && !qhead->ev.room_rebuild && atomic_load(&requested_ir[10])==second && qhead->ev.dsp.p[DSP_SIZE]==.2);
    assert(qhead->ev.dsp.p[DSP_ROOMFADE]==.1 && qhead->ev.dsp.p[DSP_ROOMDIM]==1000);
    double retained_energy=0;
    for (int frame=0;frame<512;frame++) {
        VoiceMix mix={0}; double left,right;
        native_reverb(10,&mix);
        convolution_process(unchanged,0,0,&left,&right);
        assert(mix.left==left && mix.right==right);
        retained_energy+=left*left+right*right;
    }
    assert(retained_energy>0); free(unchanged);
    event_queue_clear();
    /* The earlier accepted voice must not restore its room at onset. */
    audio_trigger_event(&future);
    for (int i=0;i<MAX_VOICES;i++) if (voices[i].active && voices[i].orbit==10) {
        assert(!voices[i].onset_room);
        voice_onset(&voices[i]);
    }
    assert(atomic_load(&requested_ir[10])==second);
    /* Independent custom-IR equality guards: matching sample identity and
       omitted controls reuse all parameters; replacing the sample resets
       omitted controls, including the effective speed/offset defaults. */
    AudioEvent custom={.orbit=11,.room_params_valid=1,.room_sample=123,
        .room_speed=2,.room_begin=.25};
    dsp_params_init(&custom.dsp); custom.dsp.enabled=1;
    custom.dsp.p[DSP_ROOM]=1; custom.dsp.p[DSP_SIZE]=.3;
    custom.dsp.p[DSP_ROOMFADE]=.02; custom.dsp.p[DSP_ROOMLP]=9000;
    custom.dsp.p[DSP_ROOMDIM]=500;
    audio_prepare_room(&custom);
    AudioEvent same={.orbit=11,.room_params_valid=1,.room_sample=123,.room_speed=1};
    dsp_params_init(&same.dsp); same.dsp.enabled=1; same.dsp.p[DSP_ROOM]=1;
    AudioEvent different=same; different.room_sample=124;
    audio_inherit_room(&same,0);
    assert(same.room_speed==2 && same.room_begin==.25);
    assert(same.dsp.p[DSP_SIZE]==.3 && same.dsp.p[DSP_ROOMFADE]==.02);
    audio_inherit_room(&different,0);
    assert(different.room_speed==1 && different.room_begin==0);
    assert(different.dsp.p[DSP_SIZE]==2 && different.dsp.p[DSP_ROOMFADE]==.1);
}

static void test_delay_automation(void) {
    audio_time=0; audio_toggle_play(1); event_queue_clear();
    for (int i=0;i<MAX_VOICES;i++) voices[i].active=0;
    pending_delay_count=0; web_delay_created[8]=0; next_voice_onset=HUGE_VAL;
    assert(track_load_from_code("globalThis.__query=()=>JSON.stringify(["
        "{begin:0,end:.1,value:{s:'sine',gain:0,orbit:8,delay:1,delaytime:.25,delayfeedback:.5}},"
        "{begin:.25,end:.35,value:{s:'sine',gain:0,orbit:8,delay:1,delaytime:.5,delayfeedback:.75}},"
        "{begin:.5,end:.6,value:{s:'sine',gain:0,orbit:8,delay:1,delaytime:.25,delayfeedback:.5}}]); note(60)"));
    assert(js_audio_schedule_arc(tracks[0].javascript,.1,2,0,1,event_queue_generation()));
    assert(event_queue_depth()==3 && pending_delay_count==1);
    assert(orbit_dt[8]==.25 && orbit_fb[8]==.5);
    for (QNode *node=qhead;node;node=node->next) assert(node->ev.delay_prepared);
    event_queue_clear(); /* Accepted automation survives removal of its sources. */
    float output[512];
    while (audio_time<1.2) pa_callback(NULL,output,256,NULL,0,NULL);
    assert(pending_delay_count==0 && orbit_dt[8]==.5 && orbit_fb[8]==.75);
    /* Inexact decimal time values compare unequal to their Float32 getter,
       unlike feedback .5: only time gets a future return automation. */
    audio_time=0; web_delay_created[8]=0;
    AudioEvent event={.time=.1,.orbit=8,.delay_send=1,.delay_time=.1,.delay_fb=.5};
    dsp_params_init(&event.dsp); event.dsp.enabled=1;
    assert(audio_prepare_delay(&event));
    event.delay_prepared=0; event.time=.2; event.delay_time=.2; event.delay_fb=.75;
    assert(audio_prepare_delay(&event));
    event.delay_prepared=0; event.time=.3; event.delay_time=.1; event.delay_fb=.5;
    assert(audio_prepare_delay(&event) && pending_delay_count==3);
    audio_time=.4; dispatch_delays_locked();
    assert(pending_delay_count==0 && orbit_dt[8]==(float).1 && orbit_fb[8]==.75);
    audio_toggle_play(0);
}

static void test_generated_room_race(void) {
    /* Ordered publication with the pool: an older generated completion
       replaces an intervening custom IR, matching the web handoff. The decoy
       request serializes the lane first, so the custom commit structurally
       precedes the racing completion; the intermediate custom publication is
       observable before the replacement. Sizes avoid every cached IR in this
       process so the serial jobs perform real synthesis. */
    event_queue_clear(); audio_time=0; audio_toggle_play(1);
    assert(!room_created[2] && !room_created[0] && !atomic_load(&requested_ir[0]));
    assert(prep_pool_start(0,0));
    /* Own sample store: the custom-room fixture is removed by its test. */
    char root[]="/tmp/tompouce-room-race-XXXXXX",path[512],names[2][69];
    const char *previous=getenv("XDG_CONFIG_HOME");
    char *saved=previous?strdup(previous):NULL; assert(!previous || saved);
    assert(mkdtemp(root) && !setenv("XDG_CONFIG_HOME",root,1));
    const char *directories[]={"tompouce","tompouce/sounds","tompouce/sounds/assets"};
    for (int i=0;i<3;i++) {
        snprintf(path,sizeof(path),"%s/%s",root,directories[i]); assert(!mkdir(path,0700));
    }
    for (int i=0;i<2;i++) {
        memset(names[i],'a'+i,64); memcpy(names[i]+64,".wav",5);
        snprintf(path,sizeof(path),"%s/tompouce/sounds/assets/%s",root,names[i]);
        unsigned char wav[]={'R','I','F','F',44,0,0,0,'W','A','V','E','f','m','t',' ',
            16,0,0,0,1,0,1,0,68,172,0,0,136,88,1,0,2,0,16,0,
            'd','a','t','a',8,0,0,0,0,32,0,192,0,16,0,48};
        FILE *file=fopen(path,"wb"); assert(file);
        assert(fwrite(wav,1,sizeof(wav),file)==sizeof(wav) && !fclose(file));
    }
    snprintf(path,sizeof(path),"%s/tompouce/sounds/index.json",root);
    FILE *index=fopen(path,"w"); assert(index);
    assert(fprintf(index,"{\"version\":1,\"sounds\":{\"racefixture\":["
        "{\"label\":\"a.wav\",\"file\":\"%s\"},{\"label\":\"b.wav\",\"file\":\"%s\"}]}}",names[0],names[1])>0);
    assert(!fclose(index) && user_sounds_reload()==1);
    assert(track_load_from_code("stack(s('sine').gain(0).room(.2).roomsize(1.5).orbit(2),"
        "s('sine').gain(0).room(.2).roomsize(.8).roomfade(.05).roomlp(6000).roomdim(200).orbit(0).late(.25),"
        "s('sine').gain(0).room(.2).roomsize(.3).orbit(0).ir('racefixture').set({i:0}).late(.5))"));
    assert(js_audio_schedule(tracks[0].javascript,1,0,event_queue_generation()));
    assert(event_queue_depth()==3);
    const AudioEvent *decoy=&qhead->ev,*first=&qhead->next->ev,*second=&qhead->next->next->ev;
    assert(decoy->time<first->time && first->time<second->time);
    assert(decoy->orbit==2 && decoy->room_rebuild && !decoy->ir_kernel);
    assert(first->orbit==0 && first->room_rebuild && !first->ir_kernel);
    assert(second->orbit==0 && second->ir_kernel && second->room_params_valid);
    const ConvKernel *custom=second->ir_kernel;
    /* The custom commit is synchronous while the racing completion waits
       behind the decoy on the serial lane. A different first value means
       the race inverted and must be investigated, not ignored. */
    int waited=0;
    while (!atomic_load(&requested_ir[0]) && waited<5000) {
        struct timespec pause={.tv_nsec=1000000}; nanosleep(&pause,NULL); waited++;
    }
    assert(atomic_load(&requested_ir[0])==custom);
    audio_wait_room_preparation();
    const ConvKernel *generated=atomic_load(&requested_ir[0]);
    assert(generated && generated!=custom);
    VoiceMix adopted={0}; native_reverb(0,&adopted);
    assert(convolvers[0].kernel==generated && !convolvers[0].remaining);
    assert(atomic_load(&requested_ir[2]) && atomic_load(&requested_ir[2])!=generated);
    /* Failed generation keeps the voice on the previous room and retains
       its equality guards, matching the web failure handoff. Size 3 exceeds
       the kernel frame bound, so the failure is deterministic. */
    event_queue_clear();
    assert(!room_created[1] && !atomic_load(&requested_ir[1]));
    unsigned failures=room_failures;
    assert(track_load_from_code("s('sine').gain(0).room(.2).roomsize(3).orbit(1)"));
    assert(js_audio_schedule(tracks[0].javascript,1,0,event_queue_generation()));
    assert(qhead && qhead->ev.room_prepared);
    audio_wait_room_preparation();
    assert(room_failures==failures+1 && !atomic_load(&requested_ir[1]));
    AudioEvent failed=qhead->ev;
    audio_trigger_event(&failed);
    float output[256];
    pa_callback(NULL,output,128,NULL,0,NULL);
    pa_callback(NULL,output,128,NULL,0,NULL);
    for (int i=0;i<256;i++) assert(isfinite(output[i]));
    assert(!atomic_load(&requested_ir[1]));
    event_queue_clear();
    assert(track_load_from_code("s('sine').gain(0).room(.2).roomsize(3).orbit(1)"));
    assert(js_audio_schedule(tracks[0].javascript,1,0,event_queue_generation()));
    assert(qhead && !qhead->ev.room_rebuild);
    audio_wait_room_preparation();
    assert(room_failures==failures+1);
    event_queue_clear(); assert(!unlink(path));
    for (int i=0;i<2;i++) {
        snprintf(path,sizeof(path),"%s/tompouce/sounds/assets/%s",root,names[i]); assert(!unlink(path));
    }
    for (int i=2;i>=0;i--) {
        snprintf(path,sizeof(path),"%s/%s",root,directories[i]); assert(!rmdir(path));
    }
    assert(!rmdir(root));
    assert(saved?!setenv("XDG_CONFIG_HOME",saved,1):!unsetenv("XDG_CONFIG_HOME"));
    free(saved); user_sounds_reload();
    event_queue_clear(); audio_time=0;
    for (int i=0;i<MAX_VOICES;i++) voices[i].active=0;
}

/* Deterministic live-engine driver for the engine-op comparison harness.
   Virtual time advances in exact 128-frame blocks; scripted transport,
   source and tempo operations fire on integer sample boundaries, so the
   web live driver can mirror them sample-exactly. The fixture bank comes
   from TOMPOUCE_LIVE_BANKS (an XDG layout written by the QA script). */
static void reference_live_onset(const char *scenario, const char *pcm_path) {
    const char *banks = getenv("TOMPOUCE_LIVE_BANKS");
    assert(banks && scenario && pcm_path);
    assert(!setenv("XDG_CONFIG_HOME", banks, 1));
    assert(user_sounds_reload() == 1);
    int transport = !strcmp(scenario, "transport");
    int source = !strcmp(scenario, "source");
    int tempo = !strcmp(scenario, "tempo");
    int restart = !strcmp(scenario, "restart");
    int exttempo = !strcmp(scenario, "exttempo");
    assert(transport || source || tempo || restart || exttempo);
    double end = transport ? 2.0 : restart ? 3.0 : 2.5;
    const char *first = tempo ? "s('livefixture').fast(4).clip(.25).late(.137).cps(fastcat(0.5,0.5,1,1))"
        : "s('livefixture').fast(4).clip(.25).late(.137)";
    const char *second = "s('livefixture').fast(2).clip(.25).late(.311)";
    log_console_stderr(0);
    assert(prep_pool_start(0,0));
    event_queue_clear();
    audio_time = 0; audio_toggle_play(1); audio_set_reference_mode(1); audio_set_bpm(120);
    assert(track_load_from_code(first));
    enum { MAX_OPS = 2 };
    double op_times[MAX_OPS]; int op_kinds[MAX_OPS]; int nops = 0;
    if (transport) { op_times[nops] = 1.0; op_kinds[nops++] = 0; }
    if (source) { op_times[nops] = 1.0; op_kinds[nops++] = 2; }
    if (restart) {
        op_times[nops] = 1.0; op_kinds[nops++] = 0;
        op_times[nops] = 1.5; op_kinds[nops++] = 1;
    }
    if (exttempo) { op_times[nops] = 1.0; op_kinds[nops++] = 3; }
    long total = lround(end*SR);
    float *pcm = calloc((size_t)total*2, sizeof(*pcm)); assert(pcm);
    long done = 0;
    int fired[MAX_OPS] = {0};
    unsigned long long first_serial = voice_onset_serial;
    while (done < total) {
        long boundary = total;
        for (int i = 0; i < nops; i++) if (!fired[i]) {
            long at = lround(op_times[i]*SR);
            if (at < boundary) boundary = at;
        }
        long chunk = boundary-done;
        if (chunk > 128) chunk = 128;
        if (chunk < 1) chunk = 1;
        for (int i = 0; i < nops; i++) if (!fired[i] && done >= lround(op_times[i]*SR)) {
            fired[i] = 1;
            if (op_kinds[i] == 0) scheduler_transport_set(0);
            else if (op_kinds[i] == 1) scheduler_transport_set(1);
            else if (op_kinds[i] == 2) assert(track_load_from_code(second));
            else if (op_kinds[i] == 3) audio_set_bpm(240);
        }
        double now = (double)done/SR;
        live_tick(now);
        while (prepare_queued_audio(now)) ;
        pa_callback(NULL, pcm+(size_t)done*2, (unsigned long)chunk, NULL, 0, NULL);
        done += chunk;
    }
    for (long i = 0; i < total*2; i++) assert(isfinite(pcm[i]));
    FILE *out = fopen(pcm_path, "wb"); assert(out);
    assert(fwrite(pcm, sizeof(*pcm), (size_t)total*2, out) == (size_t)total*2 && !fclose(out));
    printf("{\"scenario\":\"%s\",\"frames\":%ld,\"voices\":%llu,\"queue\":%d}\n",
        scenario, total, voice_onset_serial-first_serial, event_queue_depth());
    free(pcm);
    event_queue_clear(); audio_time = 0; audio_toggle_play(0);
    for (int i = 0; i < MAX_VOICES; i++) voices[i].active = 0;
    stop_room_worker(); prep_pool_stop();
}

static void test_web_delay_pcm(void) {
    const double frames[]={.25,.5,32,128,128.5,2205.25};
    const int onsets[][6]={{0,1,128,129,130,256},{0,1,128,129,130,256},
        {32,192,352,512,672,832},{128,384,640,896,1152,1408},
        {128,129,384,385,386,640},{2205,2206,4538,4539,4540,6871}};
    const double values[][6]={{.375,.125,.140625,.09375,.015625,.052734375},
        {.25,.25,.0625,.125,.0625,.015625},{.5,.25,.125,.0625,.03125,.015625},
        {.5,.25,.125,.0625,.03125,.015625},{.25,.25,.0625,.125,.0625,.015625},
        {.375,.125,.140625,.09375,.015625,.052734375}};
    for (int trial=0;trial<6;trial++) {
        int orbit=4+trial,found=0;
        web_delay_created[orbit]=1; orbit_dt[orbit]=(float)(frames[trial]/SR); orbit_fb[orbit]=.5;
        for (int frame=0;frame<9000 && found<6;frame++) {
            VoiceMix mix={.delay_l=frame==0?.5:0,.delay_r=frame==0?-.5:0};
            orbit_delay(orbit,&mix);
            if (fabs(mix.left)>1e-7) {
                assert(frame==onsets[trial][found]);
                assert(fabs(mix.left-values[trial][found])<1e-7 && fabs(mix.right+mix.left)<1e-7);
                found++;
            }
        }
        assert(found==6);
    }
}

static void test_stopped_evaluation_commits(void) {
    event_queue_clear(); audio_toggle_play(0); audio_set_bpm(120); audio_time=0;
    assert(track_load_from_code("note(60).fast(40).gain(.2)"));
    scheduler_transport_set(1); live_tick(0);
    assert(event_queue_depth()==4);
    QNode *accepted=qhead;
    AudioEvent old=accepted->ev;
    unsigned generation=event_queue_generation();
    audio_time=.04; scheduler_transport_set(0);
    assert(track_load_from_code("note(72).fast(40).gain(.7)"));
    assert(!audio_is_playing() && qhead==accepted && event_queue_depth()==4);
    assert(event_queue_generation()!=generation);
    assert(!memcmp(&qhead->ev,&old,sizeof(old)));
    event_queue_push(&old); /* Superseded producers cannot add new notes. */
    assert(event_queue_depth()==4);
    scheduler_transport_set(1); live_tick(.04);
    assert(event_queue_depth()==8 && fabs(tracks[0].js_cursor-.1)<1e-9);
    int prior=0,current=0;
    for (QNode *node=qhead;node;node=node->next) {
        if (node->ev.generation==generation) { prior++; assert(node->ev.gain==.2); }
        else { current++; assert(node->ev.gain==.7 && node->ev.time>=.15-1e-9); }
    }
    assert(prior==4 && current==4);
    event_queue_clear(); scheduler_transport_set(0);
    assert(track_load_from_code("note(60).gain(0)"));
}

static void test_stop_pcm(void) {
    event_queue_clear(); audio_toggle_play(0); audio_set_bpm(120); audio_time=.1;
    assert(track_load_from_code("note(69).gain(0)"));
    scheduler_transport_set(1);
    for (int i=0;i<MAX_VOICES;i++) voices[i].active=0;
    AudioEvent event={.inst=INST_SINE,.smp=-1,.wt=-1,.midi_chan=-1,.midi_note=-1,
        .freq=440,.gain=.5,.duration=2,.att=.001,.dec=0,.sus=1,.rel=.02};
    dsp_params_init(&event.dsp); event.dsp.enabled=1;
    audio_trigger_event(&event);
    float before[8820],after[8820];
    pa_callback(NULL,before,4410,NULL,0,NULL);
    scheduler_transport_set(0);
    pa_callback(NULL,after,4410,NULL,0,NULL);
    double aa=0,bb=0,ab=0;
    for (int i=4410;i<8820;i++) {
        assert(isfinite(before[i]) && isfinite(after[i]));
        aa+=before[i]*before[i]; bb+=after[i]*after[i]; ab+=before[i]*after[i];
    }
    assert(aa>1e-4 && bb>1e-4 && fabs(bb/aa-1)<.001);
    assert(ab/sqrt(aa*bb)>.999999); /* 100 ms = 44 exact oscillator cycles. */
    assert(!audio_is_playing() && tracks[0].js_cursor==0);
    voices[0].active=0;
}

#ifdef HAVE_CSOUND
static void csound_callback_level(void) {
    float output[128];
    for (int block=0;block<8;block++) {
        struct timespec pace={0,2000000}; nanosleep(&pace,NULL);
        pa_callback(NULL,output,64,NULL,0,NULL);
        for (int i=0;i<128;i++) assert(fabs(output[i]-.1)<1e-6);
    }
}
static void test_csound_callback_continuity(void) {
    audio_time=0; event_queue_clear(); audio_toggle_play(1);
    assert(track_load_from_code("await loadCsound('instr HeldCallback\\na1 = p5\\nout a1,a1\\nendin'); note(60).csound('HeldCallback')"));
    CsoundProgram *renderer=javascript_csound(tracks[0].javascript); assert(renderer);
    cJSON *value=cJSON_Parse("{\"csound\":\"HeldCallback\",\"gain\":0.5}"); assert(value);
    assert(csound_program_schedule(renderer,value,.01,1,440)); cJSON_Delete(value);
    float warmup[128];
    for (int block=0;block<32;block++) {
        struct timespec pace={0,2000000}; nanosleep(&pace,NULL);
        pa_callback(NULL,warmup,64,NULL,0,NULL);
    }
    csound_callback_level();
    assert(track_load_from_code("await loadCsound('instr AddedCallback\\na1 oscili p5,p4\\nout a1,a1\\nendin'); note(60).csound('AddedCallback')"));
    assert(javascript_csound(tracks[0].javascript)==renderer); csound_callback_level();
    assert(track_load_from_code("note(60).gain(0)"));
    assert(javascript_csound(tracks[0].javascript)==renderer); csound_callback_level();
    scheduler_transport_set(0); csound_callback_level();
    assert(track_load_from_code("note(60).csound('HeldCallback')"));
    assert(javascript_csound(tracks[0].javascript)==renderer); csound_callback_level();
    assert(track_load_from_code("await loadCsound('invalid orchestra'); note(72).csound('HeldCallback')"));
    assert(javascript_csound(tracks[0].javascript)==renderer); csound_callback_level();
    assert(!track_load_from_code("throw Error('failed musical edit')")); csound_callback_level();
    assert(track_load_from_code("note(60).gain(0)"));
    assert(javascript_csound(tracks[0].javascript)==renderer);
    /* End this owned fixture explicitly; JS evaluations retain definitions. */
    audio_set_csound(NULL);
    pthread_mutex_lock(&tracks_lock);
    javascript_free(tracks[0].javascript); memset(&tracks[0],0,sizeof(tracks[0])); track_count=0;
    pthread_mutex_unlock(&tracks_lock);
    audio_toggle_play(0); event_queue_clear();
}
#endif

static void duck_reference_apply(const cJSON *test) {
    cJSON *hap=cJSON_CreateObject(),*value=cJSON_CreateObject();
    assert(hap && value);
    cJSON_AddNumberToObject(hap,"begin",0); cJSON_AddNumberToObject(hap,"end",1);
    cJSON_AddItemToObject(hap,"value",value);
    cJSON_AddStringToObject(value,"s","~"); cJSON_AddNumberToObject(value,"duckorbit",1);
    const cJSON *controls=cJSON_GetObjectItemCaseSensitive(test,"controls"),*item;
    cJSON_ArrayForEach(item,controls) {
        char name[64]; snprintf(name,sizeof(name),"duck%s",item->string);
        assert(cJSON_AddItemToObject(value,name,cJSON_Duplicate(item,1)));
    }
    AudioEvent event;
    const cJSON *code=cJSON_GetObjectItemCaseSensitive(test,"code");
    JsProgram *program=NULL; cJSON *haps=NULL;
    if (cJSON_IsString(code)) {
        cJSON *metadata=NULL; char error[512];
        program=javascript_compile(code->valuestring,120,&metadata,error,sizeof(error));
        if (!program) fprintf(stderr,"Duck reference: %s\n",error);
        assert(program);
        cJSON_Delete(metadata); haps=javascript_query(program,0,1); assert(haps);
    }
    assert(js_audio_event(haps?cJSON_GetArrayItem(haps,0):hap,
        cJSON_GetObjectItemCaseSensitive(test,"sourceTime")->valuedouble,1,&event));
    PendingDuck pending={.source_time=event.time,.mask=1u<<1};
    pending.controls[1]=event.ducks[1];
    audio_time=cJSON_GetObjectItemCaseSensitive(test,"callbackAt")->valuedouble;
    apply_duck(&pending); cJSON_Delete(hap); cJSON_Delete(haps); javascript_free(program);
}

static void duck_reference(void) {
    char input[65536]; size_t length=fread(input,1,sizeof(input)-1,stdin);
    assert(!ferror(stdin) && feof(stdin)); input[length]=0;
    cJSON *tests=cJSON_Parse(input),*results=cJSON_CreateArray(); assert(tests && results);
    const cJSON *test;
    cJSON_ArrayForEach(test,tests) {
        memset(&web_duck[1],0,sizeof(web_duck[1]));
        web_duck[1].enabled=web_duck[1].hold=1;
        web_duck[1].initial=web_duck[1].target=.8;
        const cJSON *prior=cJSON_GetObjectItemCaseSensitive(test,"prior");
        if(cJSON_IsObject(prior)) duck_reference_apply(prior);
        duck_error_count=0; duck_reference_apply(test);
        cJSON *result=cJSON_CreateObject(),*gains=cJSON_CreateArray(); assert(result && gains);
        cJSON_AddNumberToObject(result,"rejected",duck_error_count?(duck_errors[0].returning?2:1):0);
        const cJSON *point;
        cJSON_ArrayForEach(point,cJSON_GetObjectItemCaseSensitive(test,"points")) {
            audio_time=cJSON_GetObjectItemCaseSensitive(point,"time")->valuedouble;
            cJSON_AddItemToArray(gains,cJSON_CreateNumber(orbit_duck_gain(1)));
        }
        cJSON_AddItemToObject(result,"gains",gains); cJSON_AddItemToArray(results,result);
    }
    char *json=cJSON_PrintUnformatted(results); assert(json); puts(json); free(json);
    cJSON_Delete(tests); cJSON_Delete(results);
}

static void duck_timer_reference(void) {
    char input[65536]; size_t length=fread(input,1,sizeof(input)-1,stdin);
    assert(!ferror(stdin) && feof(stdin)); input[length]=0;
    cJSON *tests=cJSON_Parse(input),*results=cJSON_CreateArray(); assert(tests && results);
    const cJSON *test;
    cJSON_ArrayForEach(test,tests) {
        event_queue_clear(); audio_wait_room_preparation(); audio_time=0;
        atomic_store(&created_orbits,(1u<<1)|(1u<<9));
        web_delay_created[7]=room_created[7]=0; pending_delay_count=0;
        duck_error_count=duck_error_overflow=0; pending_duck_count=0;
        web_duck[9].enabled=web_duck[9].hold=1;
        web_duck[9].initial=web_duck[9].target=.8;
        cJSON *hap=cJSON_CreateObject(),*haps=cJSON_CreateArray(),*value=cJSON_CreateObject();
        assert(hap && haps && value);
        cJSON_AddNumberToObject(hap,"begin",0); cJSON_AddNumberToObject(hap,"end",.1);
        cJSON_AddItemToObject(hap,"value",value); cJSON_AddItemToArray(haps,hap);
        cJSON_AddStringToObject(value,"s",cJSON_GetObjectItemCaseSensitive(test,"sound")->valuestring);
        cJSON_AddNumberToObject(value,"orbit",7); cJSON_AddNumberToObject(value,"gain",.1);
        cJSON_AddItemToObject(value,"duckorbit",cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(test,"targets"),1));
        cJSON_AddNumberToObject(value,"duckdepth",.25); cJSON_AddNumberToObject(value,"delay",1);
        cJSON_AddNumberToObject(value,"delaytime",.123); cJSON_AddNumberToObject(value,"delayfeedback",.5);
        cJSON_AddNumberToObject(value,"room",1); cJSON_AddNumberToObject(value,"roomsize",.05);
        char *json=cJSON_PrintUnformatted(haps); assert(json);
        char source[4096],error[512]; cJSON *metadata=NULL;
        /* Keep fixture JSON out of upstream literal-string mini notation. */
        int n=snprintf(source,sizeof(source),"globalThis.__query=()=>String.fromCharCode(");
        assert(n>0 && (size_t)n<sizeof(source));
        for (const unsigned char *p=(const unsigned char *)json;*p;p++) {
            int written=snprintf(source+n,sizeof(source)-(size_t)n,"%s%u",p==(const unsigned char *)json?"":",",*p);
            assert(written>0 && (size_t)written<sizeof(source)-(size_t)n); n+=written;
        }
        int written=snprintf(source+n,sizeof(source)-(size_t)n,"); note(60)");
        assert(written>0 && (size_t)written<sizeof(source)-(size_t)n);
        JsProgram *program=javascript_compile(source,120,&metadata,error,sizeof(error));
        if (!program) fprintf(stderr,"Duck timer reference: %s\n%s\n",error,source);
        assert(program);
        cJSON_Delete(metadata); cJSON_Delete(haps); free(json);
        audio_toggle_play(1);
        assert(js_audio_schedule_arc(program,cJSON_GetObjectItemCaseSensitive(test,"time")->valuedouble,
            1,0,.1,event_queue_generation()));
        int notes=0,controls=0;
        for(QNode *node=qhead;node;node=node->next) {
            if(node->ev.eviction_only) continue;
            if(node->ev.control_only)controls++;else notes++;
        }
        cJSON *result=cJSON_CreateObject(); assert(result);
        cJSON_AddNumberToObject(result,"sources",notes); cJSON_AddNumberToObject(result,"controls",controls);
        cJSON_AddNumberToObject(result,"errors",duck_error_count);
        cJSON_AddBoolToObject(result,"timer",duck_error_count && duck_errors[0].timer);
        cJSON_AddNumberToObject(result,"orbit",duck_error_count?duck_errors[0].orbit:-1);
        cJSON_AddNumberToObject(result,"created",atomic_load(&created_orbits));
        cJSON_AddBoolToObject(result,"delay",web_delay_created[7]);
        cJSON_AddBoolToObject(result,"room",room_created[7]);
        cJSON_AddNumberToObject(result,"gain",orbit_duck_gain(9));
        cJSON_AddItemToArray(results,result);
        event_queue_clear(); javascript_free(program);
    }
    audio_wait_room_preparation(); stop_room_worker();
    char *json=cJSON_PrintUnformatted(results); assert(json); puts(json); free(json);
    cJSON_Delete(tests); cJSON_Delete(results);
}

static void duck_mapping_format_number(cJSON *call, const char *name, double value) {
    if (isnan(value)) { cJSON_AddStringToObject(call, name, "NaN"); return; }
    if (value == 0) value = 0; /* Normalize -0: JSON has a single zero. */
    uint64_t bits; memcpy(&bits, &value, sizeof(bits));
    char hex[17];
    for (int i = 7; i >= 0; i--) {
        hex[(7 - i) * 2] = "0123456789abcdef"[(bits >> (i * 8 + 4)) & 15];
        hex[(7 - i) * 2 + 1] = "0123456789abcdef"[(bits >> (i * 8)) & 15];
    }
    hex[16] = 0; cJSON_AddStringToObject(call, name, hex);
}

static void duck_mapping_reference(void) {
    char input[65536]; size_t length = fread(input, 1, sizeof(input) - 1, stdin);
    assert(!ferror(stdin) && feof(stdin)); input[length] = 0;
    cJSON *tests = cJSON_Parse(input), *results = cJSON_CreateArray(); assert(tests && results);
    const cJSON *test;
    cJSON_ArrayForEach(test, tests) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(test, "value");
        int orbits[256]; DuckControl controls[256];
        int count = js_audio_duck_entries(value, orbits, controls, 256);
        assert(count >= 0 && count <= 256);
        cJSON *result = cJSON_CreateObject(), *calls = cJSON_CreateArray(); assert(result && calls);
        for (int i = 0; i < count; i++) {
            const DuckControl *control = &controls[i];
            cJSON *call = cJSON_CreateObject(); assert(call);
            cJSON_AddNumberToObject(call, "orbit", orbits[i]);
            if (control->string_onset == DUCK_ONSET_SUMMARY) {
                cJSON_AddStringToObject(call, "onset", "SUMMARY");
            } else if (control->string_onset) {
                char text[160]; snprintf(text, sizeof(text), "s:%s", control->onset_text);
                cJSON_AddStringToObject(call, "onset", text);
            } else {
                duck_mapping_format_number(call, "onset", control->onset);
            }
            duck_mapping_format_number(call, "attack", control->attack);
            duck_mapping_format_number(call, "depth", control->depth);
            cJSON_AddItemToArray(calls, call);
        }
        cJSON_AddItemToObject(result, "calls", calls); cJSON_AddItemToArray(results, result);
    }
    char *json = cJSON_PrintUnformatted(results); assert(json); puts(json); free(json);
    cJSON_Delete(tests); cJSON_Delete(results);
}

int main(int argc, char **argv) {
    if (argc>1 && !strcmp(argv[1],"--csound-worker")) {
#ifdef TOMPOUCE_TEST_CSOUND_WORKER
        /* The production worker's address-space cap cannot host ASan's shadow
           mapping. Keep that confined child ordinary; instrument the parent. */
        execl(TOMPOUCE_TEST_CSOUND_WORKER,"test_live_sliders","--csound-worker",(char *)NULL);
        return 1;
#else
        return csound_worker_main();
#endif
    }
    log_console_stderr(0);
    if (argc>1 && !strcmp(argv[1],"--duck-preparation-reference")) {
        test_duck_before_source_preparation();
        stop_room_worker(); prep_pool_stop();
        puts("{\"duckBeforeSource\":true,\"survivesReplacement\":true}");
        return 0;
    }
    if (argc>1 && !strcmp(argv[1],"--duck-timer-reference")) { duck_timer_reference(); return 0; }
    if (argc>1 && !strcmp(argv[1],"--duck-reference")) { duck_reference(); return 0; }
    if (argc>1 && !strcmp(argv[1],"--duck-mapping-reference")) { duck_mapping_reference(); return 0; }
    if (argc>1 && !strcmp(argv[1],"--clock-reference")) { reference_clock(1); return 0; }
    if (argc>1 && !strcmp(argv[1],"--live-onset-reference")) {
        assert(argc > 3);
        reference_live_onset(argv[2], argv[3]); return 0;
    }
    if (argc>1 && !strcmp(argv[1],"--cps-reference")) { reference_cps(1); return 0; }
    if (argc>1 && !strcmp(argv[1],"--performance-reference")) { reference_performance(1); return 0; }
    if (argc>1 && !strcmp(argv[1],"--query-failure-reference")) { reference_query_failure(1); return 0; }
#ifdef HAVE_CSOUND
    test_csound_callback_continuity();
#endif
    test_timestamped_onsets();
    test_refresh_cutoff();
    test_ready_prefix_publication();
    test_duck_before_source_preparation();
    audio_time=10.25; audio_toggle_play(1);
    const char *source="globalThis.evaluations=(globalThis.evaluations??0)+1; "
        "if(evaluations!==1)throw Error('slider reevaluated music'); "
        "note(\"<[c4*4] [d4*4] [e4*4] [f4*4]>\").s('sine')"
        ".gain(slider(.4,0,1,.1)).lpf(slider(800,200,2000,100))";
    assert(track_load_from_code(source));
    JsProgram *program=tracks[0].javascript;
    unsigned generation=event_queue_generation();
    double duration=audio_sec_per_cycle(), origin=.25;
    /* Playback is partway through cycle 5; two more cycles are queued. */
    tracks[0].cycle_index=7; tracks[0].next_cycle=origin+7*duration;
    tracks[0].mute=0; tracks[0].solo=1;
    song_advance_at=123; song_hold=1;
    js_audio_schedule(program,origin+5*duration,5,generation);
    js_audio_schedule(program,origin+6*duration,6,generation);
    assert(event_queue_depth()==8);
    cJSON *haps=javascript_query_cps(program,5,6,1/duration);
    AudioEvent event; assert(js_audio_event(cJSON_GetArrayItem(haps,0),origin,duration,&event));
    event.generation=generation;
    audio_trigger_event(&event); cJSON_Delete(haps);
    int voice=-1;
    for (int i=0;i<MAX_VOICES;i++) if (voices[i].active) { voice=i; break; }
    assert(voice>=0);
    Voice before=voices[voice];
    assert(slider_nudge(0,1) && slider_set(1,1500));
    event_queue_refresh(audio_time+.002);
    tracks[0].cycle_index=5; tracks[0].next_cycle=origin+5*duration;
    assert(tracks[0].javascript==program && tracks[0].solo==1);
    assert(event_queue_generation()==generation && playing && audio_time==10.25);
    assert(song_advance_at==123 && song_hold==1);
    assert(!strcmp(current_code,source));
    assert(!memcmp(&before,&voices[voice],sizeof(before)));
    assert(tracks[0].cycle_index==5 && tracks[0].next_cycle==origin+5*duration);
    /* The already-due onset survives. Refill may not duplicate it. */
    assert(event_queue_depth()==8); /* Existing lookahead stays live during preparation. */
    js_audio_schedule(program,tracks[0].next_cycle,tracks[0].cycle_index,generation);
    assert(event_queue_depth()==8 && qstaging.count==3);
    float preparing[512]; pa_callback(NULL,preparing,256,NULL,0,NULL);
    double preparing_energy=0;
    for (int i=0;i<512;i++) { assert(isfinite(preparing[i])); preparing_energy+=preparing[i]*preparing[i]; }
    assert(preparing_energy>1e-6 && voices[voice].active && event_queue_depth()==8);
    event_queue_refresh_finish(1);
    assert(event_queue_depth()==4);
    int count=0;
    for (QNode *node=qhead;node;node=node->next) {
        assert(node->ev.time==origin+5*duration+count*duration/4);
        assert(fabs(node->ev.freq-midi_to_freq(62))<.001); /* D cycle, not C restart */
        assert(fabs(node->ev.gain-(count?.5:.4))<1e-9);
        assert(node->ev.dsp.p[DSP_LPF]==(count?1500:800));
        count++;
    }
    assert(count==4);
    QNode *original_queue=qhead;
    event_queue_refresh(audio_time+.002);
    js_audio_schedule(program,origin+5*duration,5,generation);
    event_queue_refresh_finish(0);
    assert(qhead==original_queue && event_queue_depth()==4); /* Failed preparation is harmless. */
    float output[512]; pa_callback(NULL,output,256,NULL,0,NULL);
    double energy=0;
    for (int i=0;i<512;i++) { assert(isfinite(output[i])); energy+=output[i]*output[i]; }
    assert(energy>1e-6 && voices[voice].active && audio_time>10.25);
    assert(voices[voice].start_time==before.start_time);
    /* Failed/unpublished compiles cannot steal the active control bindings. */
    assert(!track_load_from_code("note(60).gain(slider(.9,0,1,.01)); throw Error('reject')"));
    double value;
    assert(tracks[0].javascript==program && event_queue_generation()==generation);
    assert(slider_info(0,&value,NULL,NULL,NULL) && fabs(value-.5)<1e-9);
    assert(slider_count()==2);
    assert(slider_nudge(0,1));
    haps=javascript_query_cps(program,5,6,1/duration);
    assert(js_audio_event(cJSON_GetArrayItem(haps,0),origin,duration,&event));
    assert(fabs(event.gain-.6)<1e-9 && event.dsp.p[DSP_LPF]==1500);
    cJSON_Delete(haps);
    /* Repeated drags preserve the same origin and compile generation. */
    for (int i=0;i<5;i++) {
        assert(slider_nudge(1,i%2?-1:1));
        tracks[0].cycle_index=7; tracks[0].next_cycle=origin+7*duration;
        event_queue_refresh(audio_time+.002);
        tracks[0].cycle_index=5; tracks[0].next_cycle=origin+5*duration;
        js_audio_schedule(program,tracks[0].next_cycle,tracks[0].cycle_index,generation);
        event_queue_refresh_finish(1);
        assert(tracks[0].javascript==program && event_queue_generation()==generation);
        assert(fabs(tracks[0].next_cycle-tracks[0].cycle_index*duration-origin)<1e-9);
    }
    audio_toggle_play(0); event_queue_clear();
#ifdef HAVE_CSOUND
    assert(track_load_from_code("await loadCsound`instr Tone\na1 oscili p5,p4\nout a1,a1\nendin`; "
                               "note(\"c4*4\").csound('Tone').gain(slider(.25,0,1,.05))"));
    program=tracks[0].javascript;
    CsoundProgram *renderer=javascript_csound(program); assert(renderer);
    generation=event_queue_generation();
    tracks[0].cycle_index=7; tracks[0].next_cycle=origin+7*duration;
    js_audio_schedule(program,origin+6*duration,6,generation);
    int queued=event_queue_depth(); assert(queued==4);
    assert(slider_nudge(0,1)); live_tick(audio_time);
    assert(tracks[0].javascript==program && javascript_csound(program)==renderer);
    assert(tracks[0].cycle_index==7 && tracks[0].next_cycle==origin+7*duration);
    assert(event_queue_depth()==queued && event_queue_generation()==generation && !playing);
    haps=javascript_query_cps(program,7,8,1/duration);
    assert(js_audio_event(cJSON_GetArrayItem(haps,0),origin,duration,&event));
    assert(fabs(event.gain-.3)<1e-9); cJSON_Delete(haps);
    assert(track_load_from_code(current_code));
    assert(javascript_csound(tracks[0].javascript)==renderer);
#endif
    event_queue_clear();
    assert(track_load_from_code("note(60).gain(slider(.5))"));
    double default_step;
    assert(slider_info(0,NULL,NULL,NULL,&default_step) && fabs(default_step-.001)<1e-12);
    assert(slider_nudge(0,1));
    assert(slider_info(0,&value,NULL,NULL,NULL) && fabs(value-.501)<1e-12);
    assert(track_load_from_code("note(60).gain(slider(2,0,1))"));
    assert(slider_info(0,&value,NULL,NULL,NULL) && value==2);
    assert(slider_nudge(0,-1));
    assert(slider_info(0,&value,NULL,NULL,NULL) && fabs(value-.999)<1e-12);
    assert(track_load_from_code("note(60).gain(slider(5e-9,0,1e-8))"));
    assert(slider_nudge(0,1));
    assert(slider_info(0,&value,NULL,NULL,NULL) && fabs(value-5.01e-9)<1e-20);
    assert(track_load_from_code("note(60).gain(0)"));
    reference_clock(0);
    test_external_tempo_anchor();
    test_performance_tail();
    test_disabled_delay_keeps_tail();
    reference_cps(0);
    reference_performance(0);
    reference_query_failure(0);
    test_stop_pcm();
    test_silent_sample_cut();
    test_duck_control_lifetime();
    test_duck_timer_failure();
    test_source_eviction_schedule();
    test_duck_target_existence();
    test_duck_target_coercion();
    test_ordered_duck_targets();
    test_duck_numeric_controls();
    test_invalid_duck_controls();
    test_boolean_duck_onset();
    test_nonfinite_duck_numbers();
    test_string_duck_onset();
    test_interrupted_duck_anchor();
    test_long_duck_onset();
    test_nul_duck_controls();
    test_nested_duck_controls();
    test_duck_output_routing();
    test_javascript_numeric_strings();
    test_delay_automation();
    test_room_preparation();
    test_custom_room_schedule();
    test_generated_room_race();
    test_web_delay_pcm();
    test_stopped_evaluation_commits();
    assert(track_load_from_code("globalThis.__query=()=>JSON.stringify([{value:{note:60}},null]); note(60)"));
    assert(js_audio_schedule_arc(tracks[0].javascript,audio_time+.1,2,0,.025,event_queue_generation()));
    assert(event_queue_depth()==0); /* Malformed user haps may not crash sorting. */
    assert(track_load_from_code("note(60).gain(0)"));
    puts("Live sliders preserve program, cycle phase, due events, voices, transport and song state; future parameters update without duplicate onsets");
    stop_room_worker();
    prep_pool_stop();
    return 0;
}
