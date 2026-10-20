/* Paced real producer/consumer/callback, owned sample store, no audio devices.
   Timing is observational: report violations rather than hiding host jitter
   behind wider budgets. Each workload starts in a fresh process/cache. */
#include "../src/audio.c"
#include "../src/scheduler.c"
#include "sound_store.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>

static double monotonic(void) {
    struct timespec now; assert(!clock_gettime(CLOCK_MONOTONIC,&now));
    return now.tv_sec+now.tv_nsec/1e9;
}

static void wait_until(double target) {
    struct timespec when={.tv_sec=(time_t)target};
    when.tv_nsec=(long)((target-when.tv_sec)*1e9);
    int error;
    do { error=clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&when,NULL); } while (error==EINTR);
    assert(!error);
}

static void fixture_store(char root[64],char names[5][69]) {
    assert(mkdtemp(root) && !setenv("XDG_CONFIG_HOME",root,1));
    const char *dirs[]={"tompouce","tompouce/sounds","tompouce/sounds/assets"};
    char path[512];
    for (int i=0;i<3;i++) {
        assert(snprintf(path,sizeof(path),"%s/%s",root,dirs[i])>0);
        assert(!mkdir(path,0700));
    }
    enum { FRAMES=8*44100, BYTES=44+FRAMES*4 };
    unsigned char *wav=calloc(BYTES,1); assert(wav);
    const unsigned char header[]={
        'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,
        1,0,2,0,68,172,0,0,16,177,2,0,4,0,16,0,'d','a','t','a',0,0,0,0};
    memcpy(wav,header,sizeof(header));
    for (int i=0;i<4;i++) {
        wav[4+i]=(unsigned char)((BYTES-8)>>(i*8));
        wav[40+i]=(unsigned char)((BYTES-44)>>(i*8));
    }
    for (int i=0;i<FRAMES;i++) {
        int16_t sample=(int16_t)(2000*sin(2*M_PI*440*i/SR));
        for (int c=0;c<2;c++) {
            wav[44+i*4+c*2]=(unsigned char)sample;
            wav[45+i*4+c*2]=(unsigned char)((uint16_t)sample>>8);
        }
    }
    for (int i=0;i<5;i++) {
        memset(names[i],'a'+i,64); memcpy(names[i]+64,".wav",5);
        size_t bytes=i==4?44+44100*2:BYTES; /* half-second stretch/IR source */
        for (int byte=0;byte<4;byte++) {
            wav[4+byte]=(unsigned char)((bytes-8)>>(byte*8));
            wav[40+byte]=(unsigned char)((bytes-44)>>(byte*8));
        }
        assert(snprintf(path,sizeof(path),"%s/tompouce/sounds/assets/%s",root,names[i])>0);
        FILE *file=fopen(path,"wb"); assert(file);
        assert(fwrite(wav,1,bytes,file)==bytes && !fclose(file));
    }
    free(wav);
    assert(snprintf(path,sizeof(path),"%s/tompouce/sounds/index.json",root)>0);
    FILE *file=fopen(path,"w"); assert(file);
    assert(fputs("{\"version\":1,\"sounds\":{\"prepdeadline\":[",file)>=0);
    for (int i=0;i<4;i++) assert(fprintf(file,"%s{\"label\":\"%d.wav\",\"file\":\"%s\"}",
        i?",":"",i,names[i])>0);
    assert(fprintf(file,"],\"prepnote\":[{\"label\":\"note.wav\",\"file\":\"%s\"}]}}",names[4])>0 && !fclose(file));
    assert(user_sounds_reload()==2);
}

static void cleanup_store(const char *root,char names[5][69]) {
    char path[512];
    for (int i=0;i<5;i++) {
        snprintf(path,sizeof(path),"%s/tompouce/sounds/assets/%s",root,names[i]);
        assert(!unlink(path));
    }
    snprintf(path,sizeof(path),"%s/tompouce/sounds/index.json",root); assert(!unlink(path));
    const char *dirs[]={"tompouce/sounds/assets","tompouce/sounds","tompouce",""};
    for (int i=0;i<4;i++) {
        snprintf(path,sizeof(path),"%s/%s",root,dirs[i]); assert(!rmdir(path));
    }
}

static void build_workload_source(char *source, size_t size, int workload) {
    size_t used=0;
    int count=workload==2?16:4;
    int n=snprintf(source,size,"stack("); assert(n>0); used=(size_t)n;
    if (workload==3) {
        /* Voice-burst overlap: twelve sine voices per cycle need no
           preparation and always publish, so the pool saturates
           deterministically, while four stretched voices with distinct
           factors load the workers. The long release keeps every accepted
           voice sounding past the run horizon. The sine ducks exercise
           control publication and timer dispatch under that load. */
        for (int i=0;i<12;i++) {
            n=snprintf(source+used,size-used,
                "%ss('sine').note(%d).release(2.5).orbit(%d).duckorbit(2).duckattack(.02).duckdepth(.5)",
                i?",":"",60+i,i%4);
            assert(n>0 && (size_t)n<size-used); used+=(size_t)n;
        }
        static const char *const factors[]={"1.25","1.5","1.75","2"};
        for (int i=0;i<4;i++) {
            n=snprintf(source+used,size-used,
                ",s('prepdeadline').n(%d).note(%d).stretch(%s).release(2.5).orbit(%d)",
                i,70+i,factors[i],i%4);
            assert(n>0 && (size_t)n<size-used); used+=(size_t)n;
        }
    } else for (int i=0;i<count;i++) {
        n=snprintf(source+used,size-used,
            "%ss('prepdeadline').n(%d).note(%d)%s.orbit(%d)",
            i?",":"",i%4,60+i,workload==2?".stretch(2)":"",i%4);
        assert(n>0 && (size_t)n<size-used); used+=(size_t)n;
    }
    n=snprintf(source+used,size-used,
        ",s('prepnote').note(60).stretch(2),"
        "s('sine').ir('prepnote').irspeed(slider(1,1,2,.25)).room(.2).roomsize(.25).orbit(2),"
        "s('sine').room(.2).roomsize(slider(.8,.8,1,.05)).roomfade(.01).orbit(3)"
        ").speed(slider(1,1,2,.25)).gain(.005).fast(%d)",
        workload==3?10:4);
    assert(n>0 && (size_t)n<size-used);
}

/* Real-backend render wrapper. Times the actual DSP path on the output
   thread; statistics use atomics because the backend owns this thread. */
static _Atomic unsigned long long backend_render_ns, backend_render_max_ns;
static _Atomic unsigned backend_render_calls, backend_render_overruns;
static void backend_render(float *out, unsigned long frames) {
    double begin=monotonic();
    render_output(out,frames);
    double elapsed=monotonic()-begin;
    uint64_t ns=elapsed>0?(uint64_t)(elapsed*1e9):0;
    atomic_fetch_add(&backend_render_ns,ns);
    atomic_fetch_add(&backend_render_calls,1);
    if (elapsed>frames/SR) atomic_fetch_add(&backend_render_overruns,1);
    uint64_t prev=atomic_load(&backend_render_max_ns);
    while (ns>prev && !atomic_compare_exchange_weak(&backend_render_max_ns,&prev,ns))
        ;
}

/* Same workload as run_workload, but the real output backend drives
   audio_time through render_output. Requires TOMPOUCE_OUTPUT_TARGET (or the
   default sink) to be an owned silent sink; verified by the QA script before
   nonzero gain reaches any device. */
static void run_backend_workload(unsigned workers, int workload) {
    char root[64]="/tmp/tompouce-deadline-XXXXXX",names[5][69];
    fixture_store(root,names);
    assert(!setenv("TOMPOUCE_BANKS",root,1) && !chdir(root));
    log_console_stderr(0);
    assert(prep_pool_start(workers,0));
    char source[8192]; build_workload_source(source,sizeof(source),workload);
    audio_time=0; audio_toggle_play(1); audio_set_reference_mode(1);
    atomic_store(&backend_render_ns,0); atomic_store(&backend_render_max_ns,0);
    atomic_store(&backend_render_calls,0); atomic_store(&backend_render_overruns,0);
    int started=audio_output_start(backend_render,256);
    const char *backend=audio_output_name();
    if (!started || !audio_output_active()) {
        printf("backend workload=%s workers=%u budget=%d status=no-backend backend=%s\n",
            workload==0?"cold":workload==1?"live":workload==2?"burst":"voiceburst",workers,prep_pool_cpu_budget(),backend);
        fflush(stdout);
        prep_pool_stop(); prep_profile_enable(0); cleanup_store(root,names);
        exit(2);
    }
    assert(track_load_from_code(source));
    prep_profile_reset();
    scheduler_init();
    double begin=monotonic();
    unsigned drops=0,polls=0;
    int was_active=1,changed=0,peak_voices=0,peak_retiring=0;
    while (monotonic()-begin<2.0) {
        struct timespec pause={.tv_nsec=5000000}; nanosleep(&pause,NULL);
        int active=audio_output_active();
        if (was_active && !active) drops++;
        was_active=active; polls++;
        int live=audio_active_voices(),retiring=audio_retiring_voices();
        if (live>peak_voices) peak_voices=live;
        if (retiring>peak_retiring) peak_retiring=retiring;
        if (workload==1 && !changed && monotonic()-begin>=.55) {
            assert(slider_set(0,1.25) && slider_set(1,.85) && slider_set(2,1.25)); changed=1;
        }
    }
    double rendered=audio_time_now();
    PrepDeadlineStat stats; prep_profile_deadlines(&stats);
    assert(stats.submitted>0);
    int remaining=event_queue_depth();
    audio_output_stop();
    scheduler_shutdown(); audio_wait_room_preparation(); stop_room_worker();
    PrepLockStat final_locks[PREP_LOCK_COUNT]; PrepWorkStat final_works[PREP_WORK_COUNT];
    prep_profile_snapshot(final_locks,final_works);
    assert(final_works[PREP_DECODE].calls>=5 && final_works[PREP_STRETCH].calls>=1);
    /* Room synthesis and kernel FFTs run at commit time. Burst overload may
       legitimately shed the room sine before its commit turn, so overload
       runs do not demand them here; test_room and test_live_sliders cover
       the commit path deterministically, and the QA matrices report per-case
       counters. Decode/stretch work runs inside pool jobs and is counted
       even when the commit later sheds. */
    if (workload!=2 && workload!=3)
        assert(final_works[PREP_REVERB].calls>0 && final_works[PREP_FFT].calls>0);
    unsigned long long calls=atomic_load(&backend_render_calls);
    printf("backend workload=%s workers=%u budget=%d status=measured backend=%s "
        "backend_drops=%u backend_polls=%u rendered_s=%.3f expected_s=2.000 "
        "render_calls=%llu render_mean_ms=%.3f render_max_ms=%.3f render_overruns=%u "
        "submitted=%llu jobs=%llu published=%llu reserved=%llu controls=%llu "
        "skipped_late=%llu published_late=%llu reserved_late=%llu "
        "reserve_late_max_ms=%.3f queued_at_stop=%d peak_voices=%d peak_retiring=%d\n",
        workload==0?"cold":workload==1?"live":workload==2?"burst":"voiceburst",workers,prep_pool_cpu_budget(),backend,
        drops,polls,rendered,calls,
        calls?atomic_load(&backend_render_ns)/1e6/calls:0,
        atomic_load(&backend_render_max_ns)/1e6,atomic_load(&backend_render_overruns),
        (unsigned long long)stats.submitted,(unsigned long long)stats.jobs,
        (unsigned long long)stats.sources_published,(unsigned long long)stats.sources_reserved,
        (unsigned long long)stats.controls_published,
        (unsigned long long)stats.late_preparation,(unsigned long long)stats.late_publication,
        (unsigned long long)stats.late_reservation,stats.max_reservation_late_ns/1e6,remaining,
        peak_voices,peak_retiring);
    fflush(stdout);
    prep_profile_enable(0); cleanup_store(root,names);
}

static void run_workload(unsigned workers, int workload, unsigned long block) {
    char root[64]="/tmp/tompouce-deadline-XXXXXX",names[5][69];
    fixture_store(root,names);
    /* Do not scan the user's installed catalogs or the checkout's bank tree.
       Cold measurements use only this fixture's source assets. */
    assert(!setenv("TOMPOUCE_BANKS",root,1) && !chdir(root));
    log_console_stderr(0);
    assert(prep_pool_start(workers,0));
    char source[8192]; build_workload_source(source,sizeof(source),workload);
    audio_time=0; audio_toggle_play(1); audio_set_reference_mode(1);
    assert(track_load_from_code(source));
    prep_profile_reset();
    scheduler_init();
    float *output=calloc(block*2,sizeof(*output)); assert(output);
    double started=monotonic(),max_callback=0,max_wake_late=0;
    unsigned callbacks=0,overruns=0,wake_misses=0;
    int changed=0,peak_voices=0,peak_retiring=0;
    unsigned long frames=0;
    /* Keep running through the initial cold miss, repeated cache hits and
       (for live) a parameter edit without reevaluation or transport reset. */
    while (frames<2*44100) {
        double target=started+frames/SR;
        wait_until(target);
        double begin=monotonic(),wake_late=begin-target;
        if (wake_late>max_wake_late) max_wake_late=wake_late;
        if (wake_late>block/SR) wake_misses++;
        if (workload==1 && !changed && frames/SR>=.55) {
            assert(slider_set(0,1.25) && slider_set(1,.85) && slider_set(2,1.25)); changed=1;
        }
        double callback_begin=monotonic();
        pa_callback(NULL,output,block,NULL,0,NULL);
        double elapsed=monotonic()-callback_begin;
        if (elapsed>max_callback) max_callback=elapsed;
        if (elapsed>block/SR) overruns++;
        for (unsigned long i=0;i<block*2;i++) assert(isfinite(output[i]));
        int live=audio_active_voices(),retiring=audio_retiring_voices();
        if (live>peak_voices) peak_voices=live;
        if (retiring>peak_retiring) peak_retiring=retiring;
        callbacks++; frames+=block;
    }
    PrepDeadlineStat stats; prep_profile_deadlines(&stats);
    PrepLockStat locks[PREP_LOCK_COUNT]; PrepWorkStat works[PREP_WORK_COUNT];
    prep_profile_snapshot(locks,works);
    assert(stats.submitted>0);
    int remaining=event_queue_depth();
    scheduler_shutdown(); audio_wait_room_preparation(); stop_room_worker();
    /* Check successful workload execution after joining, but report the
       snapshot from the paced interval, not the unpaced shutdown drain. */
    PrepLockStat final_locks[PREP_LOCK_COUNT]; PrepWorkStat final_works[PREP_WORK_COUNT];
    prep_profile_snapshot(final_locks,final_works);
    assert(final_works[PREP_DECODE].calls>=5 && final_works[PREP_STRETCH].calls>=1);
    /* Room synthesis and kernel FFTs run at commit time. Burst overload may
       legitimately shed the room sine before its commit turn, so overload
       runs do not demand them here; test_room and test_live_sliders cover
       the commit path deterministically, and the QA matrices report per-case
       counters. Decode/stretch work runs inside pool jobs and is counted
       even when the commit later sheds. */
    if (workload!=2 && workload!=3)
        assert(final_works[PREP_REVERB].calls>0 && final_works[PREP_FFT].calls>0);
    printf("deadline workload=%s workers=%u budget=%d block=%lu submitted=%llu jobs=%llu "
        "job_mean_ms=%.3f job_max_ms=%.3f published=%llu reserved=%llu controls=%llu "
        "skipped_late=%llu published_late=%llu reserved_late=%llu "
        "reserve_late_max_ms=%.3f queued_at_stop=%d callbacks=%u callback_overruns=%u "
        "callback_max_ms=%.3f wake_misses=%u wake_late_max_ms=%.3f "
        "peak_voices=%d peak_retiring=%d "
        "backend_underruns=unmeasured decode_cpu_ms=%.3f stretch_cpu_ms=%.3f\n",
        workload==0?"cold":workload==1?"live":workload==2?"burst":"voiceburst",workers,prep_pool_cpu_budget(),block,
        (unsigned long long)stats.submitted,(unsigned long long)stats.jobs,
        stats.jobs?stats.job_latency_ns/1e6/stats.jobs:0,
        stats.max_job_latency_ns/1e6,(unsigned long long)stats.sources_published,
        (unsigned long long)stats.sources_reserved,(unsigned long long)stats.controls_published,
        (unsigned long long)stats.late_preparation,
        (unsigned long long)stats.late_publication,(unsigned long long)stats.late_reservation,
        stats.max_reservation_late_ns/1e6,remaining,callbacks,overruns,max_callback*1000,
        wake_misses,max_wake_late*1000,peak_voices,peak_retiring,works[PREP_DECODE].cpu_ns/1e6,
        works[PREP_STRETCH].cpu_ns/1e6);
    fflush(stdout);
    free(output); prep_profile_enable(0); cleanup_store(root,names);
}

int main(int argc, char **argv) {
    if (argc==2 && !strcmp(argv[1],"--cpu-budget")) {
        printf("%d\n",prep_pool_cpu_budget()); return 0;
    }
    if (argc==4) {
        unsigned workers=(unsigned)strtoul(argv[1],NULL,10);
        int workload=atoi(argv[2]); unsigned long block=strtoul(argv[3],NULL,10);
        assert(workers>=1 && workers<=8 && workload>=0 && workload<=3 && block>=1 && block<=8192);
        run_workload(workers,workload,block); return 0;
    }
    if (argc==5 && !strcmp(argv[4],"backend")) {
        unsigned workers=(unsigned)strtoul(argv[1],NULL,10);
        int workload=atoi(argv[2]); unsigned long block=strtoul(argv[3],NULL,10);
        assert(workers>=1 && workers<=8 && workload>=0 && workload<=3 && block>=1 && block<=8192);
        (void)block;
        run_backend_workload(workers,workload); return 0;
    }
    assert(argc==1);
    /* Fresh child processes prevent earlier workers/workloads warming later
       cache measurements. No shell or user engine is involved. */
    for (int workload=0;workload<4;workload++) {
        pid_t child=fork(); assert(child>=0);
        if (!child) { run_workload(1,workload,128); _exit(0); }
        int status; assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    return 0;
}
