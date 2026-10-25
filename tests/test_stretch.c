/* Deterministic concurrent miss/failure coverage without filesystem samples. */
#include "common.h"
#include <assert.h>
#include <stdatomic.h>
static void *fixture_calloc(size_t count,size_t size);
#define calloc fixture_calloc
#define smp_get_stereo fixture_get_stereo
#define smp_base_midi fixture_base_midi
#include "../src/stretch.c"
#undef calloc
#undef smp_get_stereo
#undef smp_base_midi

static float source_pcm[8192];
void fixture_get_stereo(int id,float **pcm,int *length,int *channels) {
    (void)id; *pcm=source_pcm; *length=4096; *channels=2;
}
int fixture_base_midi(int id) { (void)id; return 60; }
static pthread_mutex_t gate_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_ready=PTHREAD_COND_INITIALIZER;
static int entered,released,fail_at;
static atomic_int allocations;
static void *fixture_calloc(size_t count,size_t size) {
    int call=atomic_fetch_add(&allocations,1)+1;
    if (call==1) {
        pthread_mutex_lock(&gate_lock);
        entered=1; pthread_cond_broadcast(&gate_ready);
        while (!released) pthread_cond_wait(&gate_ready,&gate_lock);
        pthread_mutex_unlock(&gate_lock);
    }
    return call==fail_at?NULL:calloc(count,size);
}
typedef struct { AudioEvent event; const StretchBuffer *buffer; int abandon; } Request;
static void *prepare_thread(void *data) {
    Request *request=data;
    request->buffer=stretch_prepare(&request->event);
    /* Simulate the submitting generation becoming obsolete. Waiters must
       retain a result even if its original owner immediately discards it. */
    if (request->abandon) stretch_release(request->buffer);
    return NULL;
}
static void concurrent_case(int failure) {
    Request requests[4]={0}; pthread_t threads[4];
    entered=released=0; fail_at=failure; atomic_store(&allocations,0);
    for (int i=0;i<4;i++) {
        AudioEvent *event=&requests[i].event;
        event->smp=0; event->stretch=1.3+failure*.1; event->smp_speed=1;
        event->sus=1; dsp_params_init(&event->dsp);
    }
    requests[0].abandon=1;
    assert(!pthread_create(&threads[0],NULL,prepare_thread,&requests[0]));
    pthread_mutex_lock(&gate_lock);
    while (!entered) pthread_cond_wait(&gate_ready,&gate_lock);
    pthread_mutex_unlock(&gate_lock);
    for (int i=1;i<4;i++) assert(!pthread_create(&threads[i],NULL,prepare_thread,&requests[i]));
    int waiting=0;
    for (int attempt=0;attempt<5000 && waiting!=3;attempt++) {
        pthread_mutex_lock(&lock); waiting=0;
        for (int i=0;i<CACHE_SIZE;i++) waiting+=(int)cache[i].waiters;
        pthread_mutex_unlock(&lock);
        if (waiting!=3) { struct timespec pause={0,1000000}; nanosleep(&pause,NULL); }
    }
    assert(waiting==3);
    pthread_mutex_lock(&gate_lock); released=1; pthread_cond_broadcast(&gate_ready); pthread_mutex_unlock(&gate_lock);
    for (int i=0;i<4;i++) assert(!pthread_join(threads[i],NULL));
    assert(atomic_load(&allocations)==2); /* One buffer and one FFT work area. */
    for (int i=0;i<4;i++) {
        assert(requests[i].buffer==requests[0].buffer);
        assert(!!requests[i].buffer==!failure);
        if (i) stretch_release(requests[i].buffer);
    }
    for (int i=0;i<CACHE_SIZE;i++) assert(!cache[i].reserved && !cache[i].waiters);
    if (failure) {
        /* Failure frees the reservation and permits a later successful retry. */
        fail_at=0;
        const StretchBuffer *retry=stretch_prepare(&requests[0].event);
        assert(retry); stretch_release(retry);
    }
}
int main(void) {
    for (int i=0;i<4096;i++) source_pcm[i*2]=source_pcm[i*2+1]=(float)sin(i*.1);
    concurrent_case(0); concurrent_case(1); concurrent_case(2);
    puts("stretch shared preparation, discarded owner, allocation failures and retry ok");
    return 0;
}
