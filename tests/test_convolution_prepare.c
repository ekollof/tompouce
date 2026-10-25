/* Owned synthetic PCM and deterministic allocation/wait gates. No devices. */
#include "common.h"
#include "prep_profile.h"
#include <assert.h>
#include <stdatomic.h>
static void *fixture_calloc(size_t count,size_t size);
static int fixture_wait(pthread_cond_t *condition,pthread_mutex_t *mutex,int kind);
#define calloc fixture_calloc
#define prep_cond_wait fixture_wait
#define smp_get_stereo fixture_get_stereo
#include "../src/convolution.c"
#include "../src/reverb_impulse.c"
#undef calloc
#undef prep_cond_wait
#undef smp_get_stereo

static float source_pcm[8192];
void fixture_get_stereo(int id,float **pcm,int *length,int *channels) {
    (void)id; *pcm=source_pcm; *length=4096; *channels=2;
}
static pthread_mutex_t gate_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_ready=PTHREAD_COND_INITIALIZER;
static atomic_int gated,allocations,waiting;
static int entered,released,fail_at;
static void *fixture_calloc(size_t count,size_t size) {
    if (!atomic_load(&gated)) return calloc(count,size);
    int call=atomic_fetch_add(&allocations,1)+1;
    if (call==1) {
        pthread_mutex_lock(&gate_lock);
        entered=1; pthread_cond_broadcast(&gate_ready);
        while (!released) pthread_cond_wait(&gate_ready,&gate_lock);
        pthread_mutex_unlock(&gate_lock);
    }
    return call==fail_at?NULL:calloc(count,size);
}
static int fixture_wait(pthread_cond_t *condition,pthread_mutex_t *mutex,int kind) {
    static _Thread_local int observed;
    if (!observed) { observed=1; atomic_fetch_add(&waiting,1); }
    return prep_cond_wait(condition,mutex,kind);
}
static uint32_t linear_advance(uint32_t state,uint64_t count) {
    while (count--) state=state*1664525u+1013904223u;
    return state;
}
static void test_jump(void) {
    const uint32_t seeds[]={0,1,0xc0ffeeu,UINT32_MAX};
    const uint64_t counts[]={0,1,2,3,128,1323000};
    for (unsigned i=0;i<sizeof(seeds)/sizeof(*seeds);i++) {
        for (unsigned j=0;j<sizeof(counts)/sizeof(*counts);j++)
            assert(reverb_advance(seeds[i],counts[j])==linear_advance(seeds[i],counts[j]));
        assert(reverb_advance(seeds[i],1ull<<32)==seeds[i]);
        uint64_t a=(1ull<<40)+7,b=(1ull<<35)+13;
        assert(reverb_advance(seeds[i],a+b)==reverb_advance(reverb_advance(seeds[i],a),b));
    }
}
typedef struct { int room; double size; const ConvKernel *kernel; const char *error; } Request;
static void *prepare_thread(void *data) {
    Request *request=data;
    request->kernel=request->room?convolution_prepare_reverb_ex(request->size,0,0,0,&request->error):
        convolution_prepare(0,request->size,1,0);
    return NULL;
}
static void concurrent_case(int room,int failure) {
    Request requests[4]={0}; pthread_t threads[4];
    entered=released=0; fail_at=failure;
    atomic_store(&allocations,0); atomic_store(&waiting,0); atomic_store(&gated,1);
    convolution_reverb_reset_stream();
    for (int i=0;i<4;i++) requests[i]=(Request){.room=room,.size=.03+.005*failure};
    assert(!pthread_create(&threads[0],NULL,prepare_thread,&requests[0]));
    pthread_mutex_lock(&gate_lock);
    while (!entered) pthread_cond_wait(&gate_ready,&gate_lock);
    pthread_mutex_unlock(&gate_lock);
    for (int i=1;i<4;i++) assert(!pthread_create(&threads[i],NULL,prepare_thread,&requests[i]));
    for (int attempt=0;attempt<5000 && atomic_load(&waiting)!=3;attempt++) {
        struct timespec pause={0,1000000}; nanosleep(&pause,NULL);
    }
    assert(atomic_load(&waiting)==3);
    pthread_mutex_lock(&gate_lock); released=1; pthread_cond_broadcast(&gate_ready); pthread_mutex_unlock(&gate_lock);
    for (int i=0;i<4;i++) assert(!pthread_join(threads[i],NULL));
    atomic_store(&gated,0);
    assert(atomic_load(&allocations)==(failure?failure:3));
    for (int i=0;i<4;i++) {
        assert(requests[i].kernel==requests[0].kernel);
        assert(!!requests[i].kernel==!failure);
        if (room) assert(failure?requests[i].error!=NULL:requests[i].error==NULL);
    }
    uint64_t draws=2*(uint64_t)lround(requests[0].size*1.5*SR);
    uint64_t consumed=!failure?4*draws:failure==1?0:draws;
    if (room) assert(reverb_stream==linear_advance(0xc0ffeeu,consumed));
    if (failure) {
        prepare_thread(&requests[0]); /* Reservation failure is retryable. */
        assert(requests[0].kernel && !requests[0].error);
        if (room) assert(reverb_stream==linear_advance(0xc0ffeeu,consumed+draws));
    }
    if (room) {
        const char *error="stale";
        assert(convolution_prepare_reverb_ex(requests[0].size,0,0,0,&error)==requests[0].kernel);
        assert(!error);
        assert(reverb_stream==linear_advance(0xc0ffeeu,consumed+(failure?2:1)*draws));
    }
}
static void test_distinct_rooms(void) {
    convolution_reverb_reset_stream();
    Request requests[4]; pthread_t threads[4]; uint64_t draws=0;
    for (int i=0;i<4;i++) {
        requests[i]=(Request){.room=1,.size=.08+i*.01};
        draws+=2*(uint64_t)lround(requests[i].size*1.5*SR);
        assert(!pthread_create(&threads[i],NULL,prepare_thread,&requests[i]));
    }
    for (int i=0;i<4;i++) { assert(!pthread_join(threads[i],NULL)); assert(requests[i].kernel && !requests[i].error); }
    assert(reverb_stream==linear_advance(0xc0ffeeu,draws));
}
int main(void) {
    for (int i=0;i<4096;i++) source_pcm[i*2]=source_pcm[i*2+1]=(float)sin(i*.1);
    test_jump();
    for (int room=0;room<2;room++) for (int failure=0;failure<=3;failure++) concurrent_case(room,failure);
    test_distinct_rooms();
    puts("convolution shared preparation, allocation failures/retries and concurrent reverb random segments ok");
    return 0;
}
