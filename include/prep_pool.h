#ifndef PREP_POOL_H
#define PREP_POOL_H
#include <stdatomic.h>

/* Bounded preparation workers. General jobs must not touch the audio callback,
   shared orbit state or the event queue; the generator publishes completed
   results in scheduling order. The serial lane runs generated-room jobs, which
   publish their kernel through the short voice lock. */

typedef struct PrepJob {
    void (*run)(struct PrepJob *job);
    void (*destroy)(struct PrepJob *job);
    atomic_uint refs;
    int done;
    struct PrepJob *next;
} PrepJob;

/* workers == 0 selects the process CPU count (bounded); capacity == 0 selects
   a bound derived from the worker count. Both values are clamped. */
int prep_pool_start(unsigned requested_workers, unsigned capacity);
void prep_pool_stop(void);
int prep_pool_workers(void);
int prep_pool_capacity(void);

void prep_job_init(PrepJob *job, void (*run)(PrepJob *), void (*destroy)(PrepJob *));
void prep_job_retain(PrepJob *job);
void prep_job_release(PrepJob *job);
/* 1 when queued; a running inline job is used only for shutdown fallback. */
int prep_pool_submit(PrepJob *job);
/* FIFO lane for generated impulses. It shares job completion rules while
   preserving the web-facing room order and bounded request queue. */
int prep_pool_submit_serial(PrepJob *job);
void prep_pool_serial_wait(void);
int prep_pool_serial_depth(void);
int prep_pool_serial_busy(void);
void prep_job_run_inline(PrepJob *job);
void prep_pool_wait(PrepJob *job);

#endif
