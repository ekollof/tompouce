/* Actual scheduler reservation and callback, with owned PCM and no devices. */
#include "../src/audio.c"
#include "../src/scheduler.c"
#include <assert.h>
#include <unistd.h>

enum { PERFORMANCE_FRAMES=8192, PERFORMANCE_NOTES=64 };

static int ramp_sample(void) {
    unsigned char wav[76]={
        'R','I','F','F',68,0,0,0,'W','A','V','E','f','m','t',' ',
        16,0,0,0,1,0,2,0,68,172,0,0,16,177,2,0,4,0,16,0,
        'd','a','t','a',32,0,0,0};
    for (int i=0;i<8;i++) {
        int16_t left=(int16_t)(i*2048),right=(int16_t)(-(i+1)*2048);
        wav[44+i*4]=(unsigned char)left; wav[45+i*4]=(unsigned char)((uint16_t)left>>8);
        wav[46+i*4]=(unsigned char)right; wav[47+i*4]=(unsigned char)((uint16_t)right>>8);
    }
    char path[]="/tmp/tompouce-performance-XXXXXX.wav";
    int fd=mkstemps(path,4); assert(fd>=0);
    assert(write(fd,wav,sizeof(wav))==(ssize_t)sizeof(wav)); assert(!close(fd));
    int sample=smp_acquire_abs(path,60); assert(sample>=0 && !unlink(path));
    return sample;
}

static void render_burst(int sample, double rate, unsigned long block, float *output) {
    event_queue_clear();
    memset(voices,0,sizeof(voices)); memset(web_duck,0,sizeof(web_duck));
    memset(duck_depth,0,sizeof(duck_depth)); memset(duck_atk,0,sizeof(duck_atk));
    pending_duck_count=0; pending_duck_serial=0; voice_onset_serial=0;
    next_voice_onset=HUGE_VAL; audio_time=0;
    audio_set_reference_mode(1); audio_toggle_play(1);
    /* Reverse acceptance order also checks that physical voice slots do not
       determine onset order. Everything fits the consumer's current horizon. */
    for (int i=PERFORMANCE_NOTES-1;i>=0;i--) {
        AudioEvent note={.generation=event_queue_generation(),.time=(1024+31*i+.4)/SR,
            .duration=3/SR,.inst=INST_TRI,.smp=sample,.wt=-1,.smp_speed=rate,
            .gain=.125,.sus=1,.smp_gate=1,.midi_chan=-1,.midi_note=-1,.orbit=3};
        dsp_params_init(&note.dsp); note.dsp.enabled=1;
        event_queue_push(&note);
        AudioEvent control={.generation=note.generation,.time=note.time,.control_only=1,
            .smp=-1,.wt=-1,.midi_chan=-1,.midi_note=-1,.duck_mask=1u<<9,
            .duck_attack=.02,.duck_depth=(i%4)*.25};
        dsp_params_init(&control.dsp); control.dsp.enabled=1;
        event_queue_push(&control);
    }
    assert(event_queue_depth()==2*PERFORMANCE_NOTES);
    int prepared=0;
    while (prepare_queued_audio(0)) prepared++;
    assert(prepared==2*PERFORMANCE_NOTES);
    assert(audio_active_voices()==PERFORMANCE_NOTES);
    assert(pending_duck_count==PERFORMANCE_NOTES);
    for (unsigned long at=0;at<PERFORMANCE_FRAMES;) {
        unsigned long count=PERFORMANCE_FRAMES-at;
        if (count>block) count=block;
        pa_callback(NULL,output+at*2,count,NULL,0,NULL); at+=count;
    }
    assert(!audio_active_voices() && !pending_duck_count && !duck_error_count);
    double last=(1024+31*(PERFORMANCE_NOTES-1)+.4)/SR;
    assert(fabs(web_duck[9].minimum-last)<1e-10);
    assert(fabs(web_duck[9].end-(last+.02))<1e-10);
    assert(orbit_duck_gain(9)==1);
    event_queue_clear();
}

static double expected_sample(double frame, int channel, int reverse) {
    /* The reverse right-channel sequence is independently checked against
       deployed reverseBuffer by effects-reference-qa.mjs. */
    const double reverse_right[]={0,-.5,-.4375,-.375,-.3125,-.25,-.1875,-.125};
    int i=(int)frame; double fraction=frame-i;
    assert(i>=0 && i+1<8);
    if (reverse && channel)
        return (1-fraction)*reverse_right[i]+fraction*reverse_right[i+1];
    double value=reverse?(7-frame)/16:frame/16;
    return channel?-value-1.0/16:value;
}

static int has_frequency(double frequency) {
    for (int i=0;i<MAX_VOICES;i++)
        if (voices[i].active && voices[i].freq==frequency) return 1;
    return 0;
}

static void voice_pool_fifo(void) {
    memset(voices,0,sizeof(voices)); next_voice_onset=HUGE_VAL;
    voice_onset_serial=0; audio_time=0;
    audio_set_reference_mode(1); audio_toggle_play(1);
    for (int i=0;i<MAX_VOICES+1;i++) {
        AudioEvent note={.time=.05-i*.0001,.duration=1,.inst=INST_SINE,
            .freq=100+i,.smp=-1,.wt=-1,.gain=i? .001:.5,
            .midi_chan=-1,.midi_note=-1};
        dsp_params_init(&note.dsp); note.dsp.enabled=1;
        audio_trigger_event(&note);
    }
    assert(audio_active_voices()==MAX_VOICES);
    assert(!has_frequency(100));
    for (int i=1;i<=MAX_VOICES;i++) assert(has_frequency(100+i));
    /* Reusing a low physical slot must not make its new source the oldest. */
    voices[64].active=0;
    for (int i=0;i<2;i++) {
        AudioEvent note={.time=.01,.duration=1,.inst=INST_SINE,.freq=1000+i,
            .smp=-1,.wt=-1,.gain=.1,.midi_chan=-1,.midi_note=-1};
        dsp_params_init(&note.dsp); note.dsp.enabled=1;
        audio_trigger_event(&note);
    }
    assert(audio_active_voices()==MAX_VOICES);
    assert(!has_frequency(101) && has_frequency(102));
    assert(has_frequency(228) && has_frequency(1000) && has_frequency(1001));
    memset(voices,0,sizeof(voices)); next_voice_onset=HUGE_VAL;
}

int main(void) {
    const double rates[]={.75,1,2,-.75,-1,-2};
    const unsigned long blocks[]={1,64,128,256,4096,8192};
    static float reference[PERFORMANCE_FRAMES*2],actual[PERFORMANCE_FRAMES*2];
    int sample=ramp_sample();
    for (unsigned r=0;r<sizeof(rates)/sizeof(rates[0]);r++) {
        render_burst(sample,rates[r],1,reference);
        for (int frame=0;frame<PERFORMANCE_FRAMES;frame++) {
            int relative=frame-1025, note=relative/31, within=relative%31;
            int sounding=relative>=0 && note<PERFORMANCE_NOTES && within<3;
            for (int channel=0;channel<2;channel++) {
                double expected=sounding?.125*expected_sample((within+.6)*fabs(rates[r]),channel,rates[r]<0):0;
                assert(fabs(reference[frame*2+channel]-expected)<1e-7);
            }
        }
        for (unsigned b=1;b<sizeof(blocks)/sizeof(blocks[0]);b++) {
            render_burst(sample,rates[r],blocks[b],actual);
            assert(!memcmp(reference,actual,sizeof(reference)));
        }
    }
    voice_pool_fifo();
    puts("64 notes + 64 duck callbacks: fractional/reverse PCM and 1..8192-frame bursts; saturated pool insertion FIFO ok");
    return 0;
}
