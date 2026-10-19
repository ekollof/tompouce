/* Owned offline room worker and callback; no audio device or user state. */
#include "audio.h"
#include "convolution.h"
#include <assert.h>
static const ConvKernel *controlled_reverb(double,double,double,double);
#define convolution_prepare_reverb controlled_reverb
#include "../src/audio.c"
#undef convolution_prepare_reverb

static pthread_mutex_t preparation_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t preparation_wake=PTHREAD_COND_INITIALIZER;
static int held,entered,fail;
static const ConvKernel *replacement;
static const ConvKernel *controlled_reverb(double size,double fade,double lp,double dim) {
    pthread_mutex_lock(&preparation_lock);
    entered=1; pthread_cond_broadcast(&preparation_wake);
    while (held) pthread_cond_wait(&preparation_wake,&preparation_lock);
    int reject=fail; const ConvKernel *kernel=replacement;
    pthread_mutex_unlock(&preparation_lock);
    return reject?NULL:kernel?kernel:convolution_prepare_reverb(size,fade,lp,dim);
}
static void hold_preparation(int reject,const ConvKernel *kernel) {
    pthread_mutex_lock(&preparation_lock);
    held=1; entered=0; fail=reject; replacement=kernel;
    pthread_mutex_unlock(&preparation_lock);
}
static void wait_entered(void) {
    pthread_mutex_lock(&preparation_lock);
    while (!entered) pthread_cond_wait(&preparation_wake,&preparation_lock);
    pthread_mutex_unlock(&preparation_lock);
}
static void release_preparation(void) {
    pthread_mutex_lock(&preparation_lock);
    held=0; pthread_cond_broadcast(&preparation_wake);
    pthread_mutex_unlock(&preparation_lock);
    audio_wait_room_preparation();
}
static AudioEvent room(int orbit,double size) {
    AudioEvent event={.orbit=orbit,.room_sample=-1,.room_params_valid=1,.room_speed=1};
    dsp_params_init(&event.dsp); event.dsp.enabled=1;
    event.dsp.p[DSP_ROOM]=1; event.dsp.p[DSP_SIZE]=size;
    audio_inherit_room(&event,1);
    return event;
}
int main(void) {
    float pcm[2048]; for (unsigned i=0;i<2048;i++) pcm[i]=.1;
    ConvKernel *first=convolution_kernel(pcm,1024,2,0);
    memset(pcm,0,sizeof(pcm));
    ConvKernel *second=convolution_kernel(pcm,1024,2,0);
    assert(first && second);
    publish_room(3,first,1);
    VoiceMix seed={.room_l=1,.room_r=-1}; native_reverb(3,&seed);
    ConvState *history=malloc(sizeof(*history)); assert(history);
    memcpy(history,&convolvers[3],sizeof(*history));
    hold_preparation(0,second);
    AudioEvent event=room(3,.1); audio_prepare_room(&event); wait_entered();
    assert(event.room_prepared && atomic_load(&requested_ir[3])==first);
    for (int i=0;i<256;i++) {
        VoiceMix mix={0}; double left,right;
        native_reverb(3,&mix); convolution_process(history,0,0,&left,&right);
        assert(mix.left==left && mix.right==right);
    }
    /* Accepted note rendering progresses while the generation worker is held. */
    AudioEvent note={.inst=INST_SINE,.smp=-1,.freq=440,.duration=.1,.gain=.1,
        .orbit=3,.room_prepared=1,.delay_prepared=1};
    audio_set_reference_mode(1); audio_trigger_event(&note);
    float output[128]; pa_callback(NULL,output,64,NULL,0,NULL);
    assert(voices[0].active && voices[0].age>0 && room_worker_busy);
    for (unsigned i=0;i<128;i++) assert(isfinite(output[i]));
    /* Repeated settings do not enqueue duplicate generation or reset tails. */
    AudioEvent same=room(3,.1); assert(!same.room_rebuild); audio_prepare_room(&same);
    assert(!room_count);
    /* Completion callbacks are not invalidated by an intervening custom IR. */
    AudioEvent custom={.orbit=3,.room_sample=123,.room_params_valid=1,
        .room_rebuild=1,.ir_kernel=first,.dsp=event.dsp};
    audio_prepare_room(&custom);
    assert(atomic_load(&requested_ir[3])==first);
    release_preparation();
    assert(atomic_load(&requested_ir[3])==second);
    VoiceMix changed={0}; native_reverb(3,&changed);
    assert(changed.left==0 && changed.right==0 && !convolvers[3].remaining);

    hold_preparation(1,NULL);
    event=room(3,.2); audio_prepare_room(&event); wait_entered();
    assert(atomic_load(&requested_ir[3])==second);
    release_preparation();
    assert(atomic_load(&requested_ir[3])==second && room_failures==1);
    same=room(3,.2); assert(!same.room_rebuild); audio_prepare_room(&same);
    audio_wait_room_preparation(); assert(room_failures==1);
    /* Failed first creation stays wet-silent; no substitute Schroeder room. */
    event=room(4,.2); audio_prepare_room(&event); audio_wait_room_preparation();
    for (int i=0;i<4096;i++) {
        VoiceMix mix={.room_l=i==0?1:0,.room_r=i==0?1:0}; native_reverb(4,&mix);
        assert(mix.left==0 && mix.right==0);
    }
    assert(!atomic_load(&requested_ir[4]) && room_failures==2);
    /* Queue saturation is bounded, observable and keeps accepted source work. */
    hold_preparation(0,second);
    event=room(3,.3); audio_prepare_room(&event); wait_entered();
    for (int i=0;i<ROOM_REQUESTS+1;i++) {
        event=room(3,.4+i*.01); audio_prepare_room(&event); assert(event.room_prepared);
    }
    assert(room_count==ROOM_REQUESTS && room_failures==3);
    release_preparation();
    audio_report_errors(); assert(!room_failures);
    stop_room_worker(); assert(!room_worker_started && !room_count && !room_worker_busy);
    /* Production generator rejects an unsupported duration without publishing. */
    replacement=NULL; fail=0;
    event=room(3,3); audio_prepare_room(&event); audio_wait_room_preparation();
    assert(atomic_load(&requested_ir[3])==second && room_failures==1);
    stop_room_worker();
    convolution_set(&convolvers[3],NULL);
    convolution_kernel_free(first); convolution_kernel_free(second); free(history);
    puts("Room worker preserves pending/failure tails, publishes completion, bounds requests and shuts down cleanly");
    return 0;
}
