/* Repeatable preparation profile. Step 4 of TODO.md reruns these workloads.
   cold: one long decode, one stretch miss, one sample-convolution miss,
   one generated-IR miss, then the same keys again as cache hits.
   live: nearby parameter changes that miss preparation and reuse the decoded sample.
   burst: parallel decodes, a serial stretch run, contended stretch locks,
   and stretch overlapped with generated IR. */
#include "prep_profile.h"
#include "sampler.h"
#include "stretch.h"
#include "convolution.h"
#include "dsp.h"
#include "event.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>
#include <math.h>

static const char *lock_name[PREP_LOCK_COUNT] = {"sample", "stretch", "convolution", "reverb"};
static const char *work_name[PREP_WORK_COUNT] = {"decode", "stretch", "fft", "reverb-synth"};
static _Atomic int barrier_arrived, barrier_release, barrier_participants;

static double seconds_now(void) {
    struct timespec now;
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
    return (double)now.tv_sec + (double)now.tv_nsec / 1e9;
}

static void print_prep(const char *label) {
    PrepLockStat locks[PREP_LOCK_COUNT];
    PrepWorkStat works[PREP_WORK_COUNT];
    prep_profile_snapshot(locks, works);
    printf("prep %s\n", label);
    for (int i = 0; i < PREP_LOCK_COUNT; i++)
        printf("  lock %-12s calls=%llu wait_s=%.6f hold_s=%.6f max_wait_s=%.6f\n",
            lock_name[i], (unsigned long long)locks[i].calls, locks[i].wait_ns / 1e9,
            locks[i].hold_ns / 1e9, locks[i].max_wait_ns / 1e9);
    for (int i = 0; i < PREP_WORK_COUNT; i++)
        printf("  work %-12s calls=%llu wall_s=%.6f cpu_s=%.6f\n",
            work_name[i], (unsigned long long)works[i].calls, works[i].wall_ns / 1e9,
            works[i].cpu_ns / 1e9);
    fflush(stdout);
}

static void snapshot(PrepLockStat locks[PREP_LOCK_COUNT], PrepWorkStat works[PREP_WORK_COUNT]) {
    prep_profile_snapshot(locks, works);
}

static char *write_wav(int frames) {
    char *path = strdup("/tmp/tompouce-prep-XXXXXX.wav");
    assert(path);
    int fd = mkstemps(path, 4);
    assert(fd >= 0);
    size_t bytes = 44u + (size_t)frames * 4u;
    unsigned char *data = calloc(1, bytes);
    assert(data);
    unsigned char header[44] = {
        'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,
        1,0,2,0,68,172,0,0,16,177,2,0,4,0,16,0,'d','a','t','a',0,0,0,0};
    uint32_t chunk = (uint32_t)(bytes - 8u), pcm = (uint32_t)(bytes - 44u);
    for (int i = 0; i < 4; i++) {
        header[4 + i] = (unsigned char)(chunk >> (8 * i));
        header[40 + i] = (unsigned char)(pcm >> (8 * i));
    }
    memcpy(data, header, sizeof(header));
    for (int i = 0; i < frames; i++) {
        double phase = 2 * 3.141592653589793 * 440.0 * i / 44100.0;
        int left = (int)(8000 * sin(phase)), right = -left / 2;
        unsigned char *sample = data + 44 + (size_t)i * 4u;
        sample[0] = (unsigned char)left; sample[1] = (unsigned char)((unsigned)left >> 8);
        sample[2] = (unsigned char)right; sample[3] = (unsigned char)((unsigned)right >> 8);
    }
    assert(write(fd, data, bytes) == (ssize_t)bytes);
    assert(close(fd) == 0);
    free(data);
    return path;
}

static int acquire(const char *path) {
    int sample = smp_acquire_abs(path, 60);
    assert(sample >= 0);
    return sample;
}

static AudioEvent stretch_event(int sample, double factor, double freq, double duration) {
    AudioEvent event;
    memset(&event, 0, sizeof(event));
    event.smp = sample;
    event.stretch = factor;
    event.freq = freq;
    event.smp_speed = 1;
    event.smp_gate = 1;
    event.duration = duration;
    event.time = 0.25;
    event.gain = 0.2;
    event.sus = 1;
    event.rel = 0.02;
    event.att = 0.01;
    event.dec = 0.05;
    dsp_params_init(&event.dsp);
    event.dsp.enabled = 1;
    return event;
}

static void stretch_once(int sample, double factor, double freq, double duration) {
    AudioEvent event = stretch_event(sample, factor, freq, duration);
    const StretchBuffer *buffer = stretch_prepare(&event);
    assert(buffer);
    stretch_release(buffer);
}

static void barrier_ready(int participants) {
    atomic_store(&barrier_arrived, 0);
    atomic_store(&barrier_release, 0);
    atomic_store(&barrier_participants, participants);
}

static void barrier_wait(void) {
    int arrived = atomic_fetch_add(&barrier_arrived, 1) + 1;
    if (arrived == atomic_load(&barrier_participants)) atomic_store(&barrier_release, 1);
    while (!atomic_load(&barrier_release))
        ;
}

typedef struct { int sample; double freq, duration, factor; } StretchJob;
typedef struct { double size; } ReverbJob;
typedef struct { char *path; } DecodeJob;

static void *stretch_thread(void *arg) {
    StretchJob *job = arg;
    barrier_wait();
    stretch_once(job->sample, job->factor, job->freq, job->duration);
    return NULL;
}

static void *reverb_thread(void *arg) {
    ReverbJob *job = arg;
    barrier_wait();
    assert(convolution_prepare_reverb(job->size, 0.01, 4000, 700));
    return NULL;
}

static void *decode_thread(void *arg) {
    DecodeJob *job = arg;
    barrier_wait();
    acquire(job->path);
    return NULL;
}

static void run_stretch_threads(int n, int sample, double duration, double factor_base) {
    pthread_t threads[8];
    StretchJob jobs[8];
    assert(n <= 8);
    barrier_ready(n);
    for (int i = 0; i < n; i++) {
        jobs[i] = (StretchJob){sample, 180.0 + 40 * i, duration, factor_base + 0.05 * i};
        assert(pthread_create(&threads[i], NULL, stretch_thread, &jobs[i]) == 0);
    }
    for (int i = 0; i < n; i++) assert(pthread_join(threads[i], NULL) == 0);
}

int main(void) {
    char *note_path = write_wav(44100);
    char *long_paths[4];
    for (int i = 0; i < 4; i++) long_paths[i] = write_wav(44100 * 8);
    int note = acquire(note_path);
    assert(unlink(note_path) == 0);
    free(note_path);

    prep_profile_reset();
    acquire(long_paths[0]);
    print_prep("cold-decode");
    PrepLockStat locks[PREP_LOCK_COUNT];
    PrepWorkStat works[PREP_WORK_COUNT];
    snapshot(locks, works);
    assert(works[PREP_DECODE].calls >= 1);
    assert(works[PREP_DECODE].wall_ns > locks[PREP_LOCK_SAMPLE].hold_ns * 4);
    assert(works[PREP_DECODE].cpu_ns > 0);

    prep_profile_reset();
    stretch_once(note, 2, 440, 0.5);
    print_prep("cold-stretch");
    snapshot(locks, works);
    assert(works[PREP_STRETCH].calls == 1);
    assert(works[PREP_STRETCH].wall_ns > 100000);
    assert(locks[PREP_LOCK_STRETCH].hold_ns + 1000000 >= works[PREP_STRETCH].wall_ns);
    assert(locks[PREP_LOCK_STRETCH].hold_ns < works[PREP_STRETCH].wall_ns * 2 + 1000000);
    assert(locks[PREP_LOCK_STRETCH].wait_ns * 20 < locks[PREP_LOCK_STRETCH].hold_ns);

    prep_profile_reset();
    assert(convolution_prepare(note, 0.25, 1, 0));
    print_prep("cold-convolution");
    snapshot(locks, works);
    assert(works[PREP_FFT].calls == 2);
    assert(locks[PREP_LOCK_CONV].hold_ns + 1000000 >= works[PREP_FFT].wall_ns);
    assert(locks[PREP_LOCK_CONV].wait_ns * 20 < locks[PREP_LOCK_CONV].hold_ns + 1000000ull);

    prep_profile_reset();
    assert(convolution_prepare_reverb(0.8, 0.01, 5000, 800));
    print_prep("cold-reverb");
    snapshot(locks, works);
    assert(works[PREP_REVERB].calls == 1);
    assert(works[PREP_FFT].calls == 2);
    assert(locks[PREP_LOCK_REVERB].hold_ns + 1000000 >=
        works[PREP_REVERB].wall_ns + works[PREP_FFT].wall_ns);
    assert(locks[PREP_LOCK_REVERB].wait_ns * 20 < locks[PREP_LOCK_REVERB].hold_ns + 1000000ull);

    prep_profile_reset();
    stretch_once(note, 2, 440, 0.5);
    assert(convolution_prepare(note, 0.25, 1, 0) != NULL);
    assert(convolution_prepare_reverb(0.8, 0.01, 5000, 800) != NULL);
    print_prep("cold-hit");
    snapshot(locks, works);
    assert(works[PREP_DECODE].calls == 0);
    assert(works[PREP_STRETCH].calls == 0);
    assert(works[PREP_FFT].calls == 0);
    assert(works[PREP_REVERB].calls == 0);
    assert(locks[PREP_LOCK_STRETCH].calls >= 1);
    assert(locks[PREP_LOCK_CONV].calls >= 1);
    assert(locks[PREP_LOCK_REVERB].calls >= 1);

    const char *mp3 = "assets/banks/dough-samples/piano/C4v8.mp3";
    char mp3_path[PATH_MAX];
    /* smp_acquire_abs resolves already-resolved absolute files only. */
    if (realpath(mp3, mp3_path) && access(mp3_path, R_OK) == 0) {
        prep_profile_reset();
        assert(acquire(mp3_path) >= 0);
        print_prep("cold-mp3");
        snapshot(locks, works);
        assert(works[PREP_DECODE].calls == 1);
        assert(works[PREP_DECODE].wall_ns > locks[PREP_LOCK_SAMPLE].hold_ns);
    } else printf("prep cold-mp3 skipped\n");

    prep_profile_reset();
    stretch_once(note, 2, 466, 0.5);
    assert(convolution_prepare(note, 0.25, 1.25, 0));
    assert(convolution_prepare_reverb(0.85, 0.01, 5000, 800));
    print_prep("live-edit");
    snapshot(locks, works);
    assert(works[PREP_DECODE].calls == 0);
    assert(works[PREP_STRETCH].calls == 1);
    assert(works[PREP_FFT].calls == 4); /* changed convolution plus changed reverb, two channels each */
    assert(works[PREP_REVERB].calls == 1);
    assert(locks[PREP_LOCK_STRETCH].hold_ns + 1000000 >= works[PREP_STRETCH].wall_ns);

    prep_profile_reset();
    pthread_t decoders[4];
    DecodeJob decode_jobs[4];
    barrier_ready(4);
    for (int i = 0; i < 4; i++) {
        /* long_paths[0] was decoded during cold. The other three, plus a repeat
           of the first, still enter the sample lock; only misses decode. */
        decode_jobs[i].path = long_paths[i];
        assert(pthread_create(&decoders[i], NULL, decode_thread, &decode_jobs[i]) == 0);
    }
    for (int i = 0; i < 4; i++) assert(pthread_join(decoders[i], NULL) == 0);
    print_prep("burst-decode");
    snapshot(locks, works);
    assert(works[PREP_DECODE].calls == 3);
    assert(works[PREP_DECODE].wall_ns > locks[PREP_LOCK_SAMPLE].wait_ns * 4);

    prep_profile_reset();
    for (int i = 0; i < 8; i++) stretch_once(note, 1.2, 200 + 15 * i, 0.12);
    print_prep("burst-serial");
    snapshot(locks, works);
    assert(works[PREP_STRETCH].calls == 8);
    assert(locks[PREP_LOCK_STRETCH].wait_ns * 20 < locks[PREP_LOCK_STRETCH].hold_ns);

    prep_profile_reset();
    run_stretch_threads(4, note, 0.35, 1.5);
    print_prep("burst-stretch");
    snapshot(locks, works);
    assert(works[PREP_STRETCH].calls == 4);
    assert(locks[PREP_LOCK_STRETCH].wait_ns * 5 > locks[PREP_LOCK_STRETCH].hold_ns);

    prep_profile_reset();
    pthread_t overlap[2];
    StretchJob stretch_job = {note, 523, 0.5, 2.2};
    ReverbJob reverb_job = {1.0};
    barrier_ready(2);
    double started = seconds_now();
    assert(pthread_create(&overlap[0], NULL, stretch_thread, &stretch_job) == 0);
    assert(pthread_create(&overlap[1], NULL, reverb_thread, &reverb_job) == 0);
    assert(pthread_join(overlap[0], NULL) == 0);
    assert(pthread_join(overlap[1], NULL) == 0);
    double elapsed = seconds_now() - started;
    snapshot(locks, works);
    printf("prep burst-overlap wall_s=%.6f parts_sum_s=%.6f\n", elapsed,
        (works[PREP_STRETCH].wall_ns + works[PREP_REVERB].wall_ns + works[PREP_FFT].wall_ns) / 1e9);
    print_prep("burst-overlap");
    assert(works[PREP_STRETCH].calls == 1 && works[PREP_REVERB].calls == 1);
    assert(locks[PREP_LOCK_STRETCH].wait_ns * 10 < locks[PREP_LOCK_STRETCH].hold_ns + 1000000ull);
    assert(locks[PREP_LOCK_REVERB].wait_ns * 10 < locks[PREP_LOCK_REVERB].hold_ns + 1000000ull);

    for (int i = 0; i < 4; i++) {
        assert(unlink(long_paths[i]) == 0);
        free(long_paths[i]);
    }
    prep_profile_enable(0);
    return 0;
}
