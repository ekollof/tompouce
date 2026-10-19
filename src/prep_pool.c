#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "prep_pool.h"
#include "common.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#ifdef __linux__
#include <sched.h>
#endif

#define PREP_MAX_WORKERS 8
#define PREP_MAX_QUEUE 512
#define PREP_SERIAL_QUEUE 32
#define PREP_DEFAULT_WORKERS 4

static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pool_work = PTHREAD_COND_INITIALIZER;
static pthread_cond_t pool_done = PTHREAD_COND_INITIALIZER;
static pthread_cond_t serial_work = PTHREAD_COND_INITIALIZER;
static pthread_cond_t serial_done = PTHREAD_COND_INITIALIZER;
static pthread_t workers[PREP_MAX_WORKERS];
static pthread_t serial_thread;
static PrepJob *queue_head, *queue_tail;
static PrepJob *serial_head, *serial_tail;
static unsigned worker_count, queue_capacity, queued, active, serial_queued;
static int serial_active;
static int started, stopping;

static unsigned affinity_cpus(void) {
#ifdef __linux__
    cpu_set_t set;
    if (sched_getaffinity(0,sizeof(set),&set)==0) {
        int count=CPU_COUNT(&set);
        if (count>0) return (unsigned)count;
    }
#endif
    long cpus=sysconf(_SC_NPROCESSORS_ONLN);
    return cpus>0?(unsigned)cpus:1;
}

static unsigned cgroup_quota_cpus(void) {
    unsigned quota=0;
    FILE *file=fopen("/sys/fs/cgroup/cpu.max","r");
    if (file) {
        char limit[32]={0};
        unsigned long period=0;
        if (fscanf(file,"%31s %lu",limit,&period)==2 && strcmp(limit,"max") &&
            period>0) {
            char *end=NULL;
            unsigned long value=strtoul(limit,&end,10);
            if (end && !*end && value>0) quota=(unsigned)((value+period-1)/period);
        }
        fclose(file);
        if (quota) return quota;
    }
    FILE *quota_file=fopen("/sys/fs/cgroup/cpu/cpu.cfs_quota_us","r");
    FILE *period_file=fopen("/sys/fs/cgroup/cpu/cpu.cfs_period_us","r");
    if (quota_file && period_file) {
        long long limit=0, period=0;
        if (fscanf(quota_file,"%lld",&limit)==1 &&
            fscanf(period_file,"%lld",&period)==1 && limit>0 && period>0)
            quota=(unsigned)((limit+period-1)/period);
    }
    if (quota_file) fclose(quota_file);
    if (period_file) fclose(period_file);
    return quota;
}

int prep_pool_cpu_budget(void) {
    unsigned budget=affinity_cpus();
    unsigned quota=cgroup_quota_cpus();
    if (quota && quota<budget) budget=quota;
    if (budget<1) budget=1;
    return (int)budget;
}

static unsigned default_workers(void) {
    unsigned cpus=(unsigned)prep_pool_cpu_budget();
    if (cpus>PREP_DEFAULT_WORKERS) cpus=PREP_DEFAULT_WORKERS;
    const char *setting = getenv("TOMPOUCE_PREP_WORKERS");
    if (setting && *setting) {
        char *end = NULL;
        long value = strtol(setting, &end, 10);
        if (end && !*end && value >= 1 && value <= PREP_MAX_WORKERS) cpus = value;
    }
    return cpus;
}

static void *worker_main(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&pool_lock);
        while (!queue_head && !stopping) pthread_cond_wait(&pool_work, &pool_lock);
        if (stopping && !queue_head) { pthread_mutex_unlock(&pool_lock); break; }
        PrepJob *job = queue_head;
        queue_head = job->next;
        if (!queue_head) queue_tail = NULL;
        queued--; active++;
        pthread_mutex_unlock(&pool_lock);
        job->run(job);
        pthread_mutex_lock(&pool_lock);
        job->done = 1;
        active--;
        pthread_cond_broadcast(&pool_done);
        pthread_mutex_unlock(&pool_lock);
        prep_job_release(job);
    }
    return NULL;
}

static void *serial_worker_main(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&pool_lock);
        while (!serial_head && !stopping) pthread_cond_wait(&serial_work, &pool_lock);
        if (stopping && !serial_head) { pthread_mutex_unlock(&pool_lock); break; }
        PrepJob *job = serial_head;
        serial_head = job->next;
        if (!serial_head) serial_tail = NULL;
        serial_queued--; serial_active = 1;
        pthread_mutex_unlock(&pool_lock);
        job->run(job);
        pthread_mutex_lock(&pool_lock);
        job->done = 1;
        serial_active = 0;
        pthread_cond_broadcast(&pool_done);
        pthread_cond_broadcast(&serial_done);
        pthread_mutex_unlock(&pool_lock);
        prep_job_release(job);
    }
    return NULL;
}

int prep_pool_start(unsigned requested, unsigned capacity) {
    pthread_mutex_lock(&pool_lock);
    if (started) { pthread_mutex_unlock(&pool_lock); return 1; }
    worker_count = requested ? requested : default_workers();
    if (worker_count < 1) worker_count = 1;
    if (worker_count > PREP_MAX_WORKERS) worker_count = PREP_MAX_WORKERS;
    queue_capacity = capacity ? capacity : worker_count * 32u;
    if (queue_capacity < 8) queue_capacity = 8;
    if (queue_capacity > PREP_MAX_QUEUE) queue_capacity = PREP_MAX_QUEUE;
    stopping = 0;
    unsigned created = 0;
    for (; created < worker_count; created++)
        if (pthread_create(&workers[created], NULL, worker_main, NULL) != 0) break;
    int serial_created = pthread_create(&serial_thread, NULL, serial_worker_main, NULL) == 0;
    if (created != worker_count || !serial_created) {
        stopping = 1;
        pthread_cond_broadcast(&pool_work);
        pthread_cond_broadcast(&serial_work);
        pthread_mutex_unlock(&pool_lock);
        for (unsigned i = 0; i < created; i++) pthread_join(workers[i], NULL);
        if (serial_created) pthread_join(serial_thread, NULL);
        pthread_mutex_lock(&pool_lock);
        stopping = 0;
        pthread_mutex_unlock(&pool_lock);
        log_console("prep pool: worker start failed; using inline preparation");
        return 0;
    }
    started = 1;
    pthread_mutex_unlock(&pool_lock);
    log_console("prep pool: %u workers, %u queued jobs", worker_count, queue_capacity);
    return 1;
}

void prep_pool_stop(void) {
    pthread_mutex_lock(&pool_lock);
    if (!started) { pthread_mutex_unlock(&pool_lock); return; }
    stopping = 1;
    pthread_cond_broadcast(&pool_work);
    pthread_cond_broadcast(&serial_work);
    pthread_mutex_unlock(&pool_lock);
    for (unsigned i = 0; i < worker_count; i++) pthread_join(workers[i], NULL);
    pthread_join(serial_thread, NULL);
    pthread_mutex_lock(&pool_lock);
    PrepJob *orphans = queue_head;
    queue_head = queue_tail = NULL;
    unsigned orphaned = queued;
    queued = 0;
    for (PrepJob *job = orphans; job; job = job->next) job->done = 1;
    PrepJob *serial_orphans = serial_head;
    serial_head = serial_tail = NULL;
    unsigned serial_orphaned = serial_queued;
    serial_queued = 0; serial_active = 0;
    for (PrepJob *job = serial_orphans; job; job = job->next) job->done = 1;
    pthread_cond_broadcast(&pool_done);
    pthread_cond_broadcast(&serial_done);
    started = 0; stopping = 0; worker_count = 0; queue_capacity = 0; active = 0;
    pthread_mutex_unlock(&pool_lock);
    while (orphans) {
        PrepJob *next = orphans->next;
        prep_job_release(orphans);
        orphans = next;
    }
    while (serial_orphans) {
        PrepJob *next = serial_orphans->next;
        prep_job_release(serial_orphans);
        serial_orphans = next;
    }
    if (orphaned + serial_orphaned)
        log_console("prep pool: discarded %u queued jobs", orphaned + serial_orphaned);
}

int prep_pool_workers(void) {
    pthread_mutex_lock(&pool_lock);
    int value = started ? (int)worker_count : 0;
    pthread_mutex_unlock(&pool_lock);
    return value;
}

int prep_pool_capacity(void) {
    pthread_mutex_lock(&pool_lock);
    int value = started ? (int)queue_capacity : 0;
    pthread_mutex_unlock(&pool_lock);
    return value;
}

void prep_job_init(PrepJob *job, void (*run)(PrepJob *), void (*destroy)(PrepJob *)) {
    if (!job) return;
    memset(job, 0, sizeof(*job));
    job->run = run;
    job->destroy = destroy;
    atomic_init(&job->refs, 1);
}

void prep_job_retain(PrepJob *job) {
    if (job) atomic_fetch_add(&job->refs, 1);
}

void prep_job_release(PrepJob *job) {
    if (!job) return;
    if (atomic_fetch_sub(&job->refs, 1) != 1) return;
    if (job->destroy) job->destroy(job);
    else free(job);
}

int prep_pool_submit(PrepJob *job) {
    if (!job) return 0;
    pthread_mutex_lock(&pool_lock);
    while (started && !stopping && queued >= queue_capacity)
        pthread_cond_wait(&pool_done, &pool_lock);
    if (!started || stopping) { pthread_mutex_unlock(&pool_lock); return 0; }
    prep_job_retain(job);
    job->next = NULL;
    if (queue_tail) queue_tail->next = job;
    else queue_head = job;
    queue_tail = job;
    queued++;
    pthread_cond_signal(&pool_work);
    pthread_mutex_unlock(&pool_lock);
    return 1;
}

int prep_pool_submit_serial(PrepJob *job) {
    if (!job) return 0;
    pthread_mutex_lock(&pool_lock);
    if (!started || stopping || serial_queued >= PREP_SERIAL_QUEUE) {
        pthread_mutex_unlock(&pool_lock); return 0;
    }
    prep_job_retain(job);
    job->next = NULL;
    if (serial_tail) serial_tail->next = job;
    else serial_head = job;
    serial_tail = job;
    serial_queued++;
    pthread_cond_signal(&serial_work);
    pthread_mutex_unlock(&pool_lock);
    return 1;
}

void prep_pool_serial_wait(void) {
    pthread_mutex_lock(&pool_lock);
    while (serial_head || serial_active)
        pthread_cond_wait(&serial_done, &pool_lock);
    pthread_mutex_unlock(&pool_lock);
}

int prep_pool_serial_depth(void) {
    pthread_mutex_lock(&pool_lock);
    int value = (int)serial_queued;
    pthread_mutex_unlock(&pool_lock);
    return value;
}

int prep_pool_serial_busy(void) {
    pthread_mutex_lock(&pool_lock);
    int value = serial_active;
    pthread_mutex_unlock(&pool_lock);
    return value;
}

void prep_job_run_inline(PrepJob *job) {
    if (!job) return;
    job->run(job);
    pthread_mutex_lock(&pool_lock);
    job->done = 1;
    pthread_cond_broadcast(&pool_done);
    pthread_mutex_unlock(&pool_lock);
}

void prep_pool_wait(PrepJob *job) {
    if (!job) return;
    pthread_mutex_lock(&pool_lock);
    while (!job->done) pthread_cond_wait(&pool_done, &pool_lock);
    pthread_mutex_unlock(&pool_lock);
}
