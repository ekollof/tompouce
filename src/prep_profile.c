#include "prep_profile.h"
#include <stdatomic.h>
#include <string.h>
#include <time.h>

static _Atomic int enabled;
static _Atomic uint64_t lock_calls[PREP_LOCK_COUNT];
static _Atomic uint64_t lock_wait_ns[PREP_LOCK_COUNT];
static _Atomic uint64_t lock_hold_ns[PREP_LOCK_COUNT];
static _Atomic uint64_t lock_max_wait_ns[PREP_LOCK_COUNT];
static _Atomic uint64_t work_calls[PREP_WORK_COUNT];
static _Atomic uint64_t work_wall_ns[PREP_WORK_COUNT];
static _Atomic uint64_t work_cpu_ns[PREP_WORK_COUNT];

static __thread struct timespec hold_started[PREP_LOCK_COUNT];
static __thread unsigned hold_active;
enum { PREP_WORK_DEPTH = 4 };
static __thread struct {
    int work;
    struct timespec wall, cpu;
} work_stack[PREP_WORK_DEPTH];
static __thread int work_depth;

static uint64_t timespec_ns(const struct timespec *t) {
    if (t->tv_sec < 0 || t->tv_nsec < 0 || t->tv_nsec >= 1000000000L) return 0;
    if ((uint64_t)t->tv_sec > UINT64_MAX / 1000000000ull) return UINT64_MAX;
    return (uint64_t)t->tv_sec * 1000000000ull + (uint64_t)t->tv_nsec;
}

static uint64_t elapsed_ns(const struct timespec *started, clockid_t clock) {
    struct timespec now;
    if (clock_gettime(clock, &now) != 0) return 0;
    uint64_t begin = timespec_ns(started), end = timespec_ns(&now);
    return end >= begin ? end - begin : 0;
}

static void add_max(_Atomic uint64_t *slot, uint64_t value) {
    uint64_t prev = atomic_load(slot);
    while (value > prev && !atomic_compare_exchange_weak(slot, &prev, value))
        ;
}

void prep_profile_enable(int on) {
    atomic_store_explicit(&enabled, on ? 1 : 0, memory_order_relaxed);
}

void prep_profile_reset(void) {
    atomic_store_explicit(&enabled, 0, memory_order_relaxed);
    for (int i = 0; i < PREP_LOCK_COUNT; i++) {
        atomic_store(&lock_calls[i], 0);
        atomic_store(&lock_wait_ns[i], 0);
        atomic_store(&lock_hold_ns[i], 0);
        atomic_store(&lock_max_wait_ns[i], 0);
    }
    for (int i = 0; i < PREP_WORK_COUNT; i++) {
        atomic_store(&work_calls[i], 0);
        atomic_store(&work_wall_ns[i], 0);
        atomic_store(&work_cpu_ns[i], 0);
    }
    atomic_store_explicit(&enabled, 1, memory_order_relaxed);
}

void prep_profile_snapshot(PrepLockStat locks[PREP_LOCK_COUNT], PrepWorkStat works[PREP_WORK_COUNT]) {
    if (!locks || !works) return;
    for (int i = 0; i < PREP_LOCK_COUNT; i++) {
        locks[i].calls = atomic_load(&lock_calls[i]);
        locks[i].wait_ns = atomic_load(&lock_wait_ns[i]);
        locks[i].hold_ns = atomic_load(&lock_hold_ns[i]);
        locks[i].max_wait_ns = atomic_load(&lock_max_wait_ns[i]);
    }
    for (int i = 0; i < PREP_WORK_COUNT; i++) {
        works[i].calls = atomic_load(&work_calls[i]);
        works[i].wall_ns = atomic_load(&work_wall_ns[i]);
        works[i].cpu_ns = atomic_load(&work_cpu_ns[i]);
    }
}

void prep_lock(pthread_mutex_t *mutex, int kind) {
    if (!mutex || kind < 0 || kind >= PREP_LOCK_COUNT ||
        !atomic_load_explicit(&enabled, memory_order_relaxed)) {
        if (mutex) pthread_mutex_lock(mutex);
        return;
    }
    struct timespec started;
    if (clock_gettime(CLOCK_MONOTONIC, &started) != 0) {
        pthread_mutex_lock(mutex);
        return;
    }
    pthread_mutex_lock(mutex);
    uint64_t wait = elapsed_ns(&started, CLOCK_MONOTONIC);
    atomic_fetch_add(&lock_calls[kind], 1);
    atomic_fetch_add(&lock_wait_ns[kind], wait);
    add_max(&lock_max_wait_ns[kind], wait);
    if (clock_gettime(CLOCK_MONOTONIC, &hold_started[kind]) == 0)
        hold_active |= 1u << kind;
}

void prep_unlock(pthread_mutex_t *mutex, int kind) {
    if (!mutex) return;
    if (kind < 0 || kind >= PREP_LOCK_COUNT || (hold_active & (1u << kind)) == 0) {
        pthread_mutex_unlock(mutex);
        return;
    }
    uint64_t hold = elapsed_ns(&hold_started[kind], CLOCK_MONOTONIC);
    hold_active &= ~(1u << kind);
    atomic_fetch_add(&lock_hold_ns[kind], hold);
    pthread_mutex_unlock(mutex);
}

void prep_work_begin(int work) {
    if (work < 0 || work >= PREP_WORK_COUNT ||
        !atomic_load_explicit(&enabled, memory_order_relaxed)) return;
    if (work_depth >= PREP_WORK_DEPTH) return;
    struct timespec wall, cpu;
    if (clock_gettime(CLOCK_MONOTONIC, &wall) != 0) return;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu) != 0) return;
    work_stack[work_depth].work = work;
    work_stack[work_depth].wall = wall;
    work_stack[work_depth].cpu = cpu;
    work_depth++;
}

void prep_work_end(int work) {
    if (work_depth <= 0 || work_stack[work_depth - 1].work != work) return;
    work_depth--;
    uint64_t wall = elapsed_ns(&work_stack[work_depth].wall, CLOCK_MONOTONIC);
    uint64_t cpu = elapsed_ns(&work_stack[work_depth].cpu, CLOCK_THREAD_CPUTIME_ID);
    atomic_fetch_add(&work_calls[work], 1);
    atomic_fetch_add(&work_wall_ns[work], wall);
    atomic_fetch_add(&work_cpu_ns[work], cpu);
}
