#ifndef PREP_PROFILE_H
#define PREP_PROFILE_H
#include <pthread.h>
#include <stdint.h>

/* Preparation profile. Disabled until prep_profile_enable(1). The audio
   callback does not take these locks. Enable only around an idle profile;
   reset is not safe while a preparation call is in progress. */

enum {
    PREP_LOCK_SAMPLE = 0,
    PREP_LOCK_STRETCH,
    PREP_LOCK_CONV,
    PREP_LOCK_REVERB,
    PREP_LOCK_COUNT
};

enum {
    PREP_DECODE = 0, /* file read, decode and resample; outside the sample lock */
    PREP_STRETCH,    /* overlap-add outside the cache lock */
    PREP_FFT,        /* impulse kernel FFT */
    PREP_REVERB,     /* generated-IR noise and low-pass, before its FFT */
    PREP_WORK_COUNT
};

typedef struct {
    uint64_t calls, wait_ns, hold_ns, max_wait_ns;
} PrepLockStat;

typedef struct {
    uint64_t calls, wall_ns, cpu_ns;
} PrepWorkStat;

void prep_profile_enable(int on);
void prep_profile_reset(void);
void prep_profile_snapshot(PrepLockStat locks[PREP_LOCK_COUNT], PrepWorkStat works[PREP_WORK_COUNT]);
void prep_lock(pthread_mutex_t *mutex, int kind);
void prep_unlock(pthread_mutex_t *mutex, int kind);
void prep_work_begin(int work);
void prep_work_end(int work);

/* Producer/consumer measurements only. The callback never reads the clock or
   updates these counters. Reset only while the producer and workers are idle. */
typedef struct {
    uint64_t submitted;
    uint64_t jobs, job_latency_ns, max_job_latency_ns;
    uint64_t sources_published, controls_published, late_preparation;
    uint64_t late_publication, max_publication_late_ns;
    uint64_t sources_reserved, late_reservation, max_reservation_late_ns;
} PrepDeadlineStat;
enum { PREP_PUBLISHED_SOURCE, PREP_PUBLISHED_CONTROL, PREP_LATE_SOURCE, PREP_RESERVED_SOURCE };
uint64_t prep_profile_now_ns(void);
void prep_profile_job_completed(uint64_t started);
void prep_profile_job_submitted(void);
void prep_profile_event(int stage, double target, double now);
void prep_profile_deadlines(PrepDeadlineStat *out);

#endif
