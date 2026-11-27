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
/* The streaming engine must reproduce the direct block loop bit for
   bit on identical input: same block grid, same cursor series, same
   op order. Covers a partial first block (negative grid origin), a
   mid-stream factor switch with last-value-wins inside one block, and
   the mono mirror path. */
static void live_vocoder_case(void) {
    enum { N = 2048 + 64 };
    static float input[2][N], expect[2][N], got[2][N];
    for (int i=0;i<N;i++) {
        input[0][i]=(float)sin(i*.05);
        input[1][i]=(float)cos(i*.07);
    }
    double fa=vocoder_factor(.5), fb=vocoder_factor(-.25);
    long first=-64;
    /* Reference: direct block loop on the absolute grid. */
    {
        static float in[2][VOCODE_FFT], sum[2][VOCODE_FFT];
        static VocoderComplex freq[VOCODE_FFT], shifted[VOCODE_FFT];
        memset(in,0,sizeof in); memset(sum,0,sizeof sum);
        memset(expect,0,sizeof expect);
        for (long block=first;block<N;block+=VOCODE_HOP) {
            double factor=block<1024?fa:fb;
            for (int ch=0;ch<2;ch++) {
                memmove(in[ch],in[ch]+VOCODE_HOP,(VOCODE_FFT-VOCODE_HOP)*sizeof(float));
                memset(in[ch]+VOCODE_FFT-VOCODE_HOP,0,VOCODE_HOP*sizeof(float));
                for (int i=0;i<VOCODE_HOP;i++) {
                    long pos=block+i;
                    if (pos>=0 && pos<N) in[ch][VOCODE_FFT-VOCODE_HOP+i]=input[ch][pos];
                }
                unsigned cursor=(unsigned)(((block%VOCODE_FFT)+VOCODE_FFT)%VOCODE_FFT);
                vocoder_transform(in[ch],sum[ch],factor,cursor,freq,shifted);
                for (int i=0;i<VOCODE_HOP;i++) {
                    long pos=block+i;
                    if (pos>=0 && pos<N) expect[ch][pos]=sum[ch][i];
                }
                memmove(sum[ch],sum[ch]+VOCODE_HOP,(VOCODE_FFT-VOCODE_HOP)*sizeof(float));
                memset(sum[ch]+VOCODE_FFT-VOCODE_HOP,0,VOCODE_HOP*sizeof(float));
            }
        }
    }
    /* Streaming: partial first block, per-sample last-wins factor. */
    {
        LiveVocoder v;
        live_vocoder_init(&v,first,64,fa,2);
        long emitted=first;
        memset(got,0,sizeof got);
        for (long pos=0;pos<N;pos++) {
            /* The staged block selects the factor; churn it mid-block so
               only the last set before completion counts. */
            double factor=v.block<1024?fa:fb;
            live_vocoder_set_factor(&v,factor==fa?fb:fa);
            live_vocoder_set_factor(&v,factor);
            live_vocoder_push(&v,input[0][pos],input[1][pos]);
            while (live_vocoder_avail(&v)>0) {
                float l=live_vocoder_out_l(&v), r=live_vocoder_out_r(&v);
                live_vocoder_pop(&v);
                if (emitted>=0 && emitted<N) { got[0][emitted]=l; got[1][emitted]=r; }
                emitted++;
            }
        }
        assert(emitted==N);
        assert(!memcmp(expect,got,sizeof expect));
    }
    /* Mono: right input ignored, left mirrored to both outputs. */
    {
        LiveVocoder v;
        live_vocoder_init(&v,0,0,fa,1);
        static float mono_in[VOCODE_HOP*3], mono_got[VOCODE_HOP*3];
        for (int i=0;i<(int)(sizeof mono_in/sizeof mono_in[0]);i++)
            mono_in[i]=(float)sin(i*.11);
        int at=0;
        for (int i=0;i<(int)(sizeof mono_in/sizeof mono_in[0]);i++) {
            live_vocoder_push(&v,mono_in[i],1234.5f);
            while (live_vocoder_avail(&v)>0) {
                float l=live_vocoder_out_l(&v), r=live_vocoder_out_r(&v);
                live_vocoder_pop(&v);
                assert(l==r);
                mono_got[at++]=l;
            }
        }
        assert(at==(int)(sizeof mono_got/sizeof mono_got[0]));
        /* Same mono content through the stereo path's left channel. */
        LiveVocoder w;
        live_vocoder_init(&w,0,0,fa,2);
        int bt=0;
        static float stereo_got[VOCODE_HOP*3];
        for (int i=0;i<(int)(sizeof mono_in/sizeof mono_in[0]);i++) {
            live_vocoder_push(&w,mono_in[i],0);
            while (live_vocoder_avail(&w)>0) {
                stereo_got[bt++]=live_vocoder_out_l(&w);
                live_vocoder_pop(&w);
            }
        }
        assert(!memcmp(mono_got,stereo_got,sizeof mono_got));
    }
}
int main(void) {
    for (int i=0;i<4096;i++) source_pcm[i*2]=source_pcm[i*2+1]=(float)sin(i*.1);
    concurrent_case(0); concurrent_case(1); concurrent_case(2);
    live_vocoder_case();
    puts("stretch shared preparation, discarded owner, allocation failures and retry ok");
    puts("stretch live vocoder matches direct block loop bit-exact ok");
    return 0;
}
