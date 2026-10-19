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

static int constant_sample(void) {
    size_t bytes=44+44100*2;
    unsigned char *wav=calloc(bytes,1); assert(wav);
    const unsigned char header[]={
        'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',
        16,0,0,0,1,0,1,0,68,172,0,0,136,88,1,0,2,0,16,0,
        'd','a','t','a',0,0,0,0};
    memcpy(wav,header,sizeof(header));
    for (int i=0;i<4;i++) { wav[4+i]=(unsigned char)((bytes-8)>>(i*8)); wav[40+i]=(unsigned char)((bytes-44)>>(i*8)); }
    for (int i=0;i<44100;i++) wav[44+i*2+1]=16; /* exact PCM16 0.125 */
    char path[]="/tmp/tompouce-retirement-XXXXXX.wav";
    int fd=mkstemps(path,4); assert(fd>=0);
    assert(write(fd,wav,bytes)==(ssize_t)bytes && !close(fd)); free(wav);
    int sample=smp_acquire_abs(path,60); assert(sample>=0 && !unlink(path));
    return sample;
}

static void render_interval(float *out, int frames, unsigned long block) {
    for (int at=0;at<frames;) {
        unsigned long count=(unsigned long)(frames-at);
        if (count>block) count=block;
        pa_callback(NULL,out+at*2,count,NULL,0,NULL); at+=(int)count;
    }
}

static void render_retirement(int sample, int synth, int delay, unsigned long block, float *out) {
    memset(voices,0,sizeof(voices)); memset(web_duck,0,sizeof(web_duck));
    memset(duck_depth,0,sizeof(duck_depth)); memset(duck_atk,0,sizeof(duck_atk));
    pending_duck_count=pending_delay_count=0; next_voice_onset=HUGE_VAL;
    voice_onset_serial=0; retirement_overflow=0; audio_time=0;
    audio_set_reference_mode(1); audio_toggle_play(1);
    orbits_ensure(); orbit_dt[3]=.02; orbit_fb[3]=0;
    AudioEvent note={.time=.1,.duration=.7,.inst=INST_SINE,.freq=synth?440:midi_to_freq(60),
        .smp=synth?-1:sample,.smp_speed=1,.smp_gate=1,.wt=-1,.gain=.1,
        .att=.02,.dec=.03,.sus=.6,.rel=.1,.orbit=3,.midi_chan=-1,.midi_note=-1,
        .delay_prepared=1,.room_prepared=1,.delay_send=delay?.5:0};
    dsp_params_init(&note.dsp); note.dsp.enabled=1;
    for (int i=0;i<MAX_VOICES;i++) { audio_trigger_event(&note); note.gain=0; }
    AudioEvent control={.time=.29,.control_only=1,.smp=-1,.wt=-1,.midi_chan=-1,.midi_note=-1,
        .delay_prepared=1,.room_prepared=1,.duck_mask=1u<<9,.duck_depth=1,.duck_attack=.1};
    dsp_params_init(&control.dsp); control.dsp.enabled=1;
    audio_trigger_event(&control); assert(pending_duck_count==1);
    render_interval(out,8960,block);
    note.time=.3; audio_trigger_event(&note);
    assert(voices[MAX_VOICES].active && voices[MAX_VOICES].retiring);
    assert(fabs(voices[MAX_VOICES].retire_end-.55)<1e-12);
    render_interval(out+8960*2,44100-8960,block);
    assert(!voices[MAX_VOICES].active && !pending_duck_count && !retirement_overflow);
    assert(fabs(web_duck[9].minimum-.29)<1e-10 && fabs(web_duck[9].end-.39)<1e-10);
    /* Flush the shared line before resetting this owned offline clock. */
    memset(voices,0,sizeof(voices));
    float flush[4096*2];
    for (int i=0;i<23;i++) pa_callback(NULL,flush,4096,NULL,0,NULL);
}

static double expected_retired(int frame, int synth) {
    double time=frame/SR;
    if (time<.1 || time>=.55) return 0;
    double age=time-.1;
    double env=age<.02?age/.02:age<.05?1-.4*(age-.02)/.03:.6;
    if (frame>=8960) {
        /* Actual deployed gain-node automation, independently captured by
           scripts/voice-eviction-reference-qa.mjs. */
        if (synth) env=.6*(.55-time)/(.55-.15);
        else env*=(.55-time)/(.55-8960/SR);
    }
    return synth?.03*env*sin(2*M_PI*440*age):.0125*env;
}

static void test_retirement_pcm(void) {
    static float reference[44100*2],actual[44100*2];
    const unsigned long blocks[]={64,4096,8192};
    int sample=constant_sample();
    for (int synth=0;synth<2;synth++) for (int delay=0;delay<2;delay++) {
        render_retirement(sample,synth,delay,1,reference);
        for (int frame=0;frame<44100;frame++) {
            double expected=expected_retired(frame,synth);
            if (delay && frame>=882) expected+=.5*(float)expected_retired(frame-882,synth);
            for (int channel=0;channel<2;channel++) assert(fabs(reference[frame*2+channel]-expected)<2e-6);
        }
        for (unsigned i=0;i<sizeof(blocks)/sizeof(blocks[0]);i++) {
            render_retirement(sample,synth,delay,blocks[i],actual);
            assert(!memcmp(reference,actual,sizeof(reference)));
        }
    }
}

static void test_retirement_capacity(void) {
    memset(voices,0,sizeof(voices)); audio_time=0; next_voice_onset=HUGE_VAL;
    voice_onset_serial=0; retirement_overflow=0;
    AudioEvent note={.time=.1,.duration=1,.inst=INST_SINE,.freq=440,.smp=-1,.wt=-1,
        .gain=0,.att=.02,.dec=.03,.sus=.6,.rel=.1,.midi_chan=-1,.midi_note=-1,
        .delay_prepared=1,.room_prepared=1};
    dsp_params_init(&note.dsp); note.dsp.enabled=1;
    for (int i=0;i<VOICE_SLOTS+1;i++) audio_trigger_event(&note);
    int fading=0;
    for (int i=MAX_VOICES;i<VOICE_SLOTS;i++) fading+=voices[i].active!=0;
    assert(fading==MAX_VOICES && audio_active_voices()==MAX_VOICES && retirement_overflow==1);
    char debug[1024]; assert(audio_debug_snapshot(debug,sizeof(debug),1)>0);
    assert(strstr(debug,"retiring=128/128"));
    audio_report_errors(); assert(!retirement_overflow);
    memset(voices,0,sizeof(voices)); next_voice_onset=HUGE_VAL;
    for (int i=0;i<MAX_VOICES;i++) audio_trigger_event(&note);
    note.time=.3; note.stretch=1; audio_trigger_event(&note);
    assert(fabs(voices[MAX_VOICES].retire_end-.51)<1e-12);
    memset(voices,0,sizeof(voices)); next_voice_onset=HUGE_VAL;
}

int main(void) {
    log_console_stderr(0);
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
    test_retirement_pcm();
    test_retirement_capacity();
    puts("Fractional/reverse PCM, FIFO retirement fades, shared delay tails and independent duck callbacks survive 1..8192-frame bursts; retirement capacity is bounded and observable");
    return 0;
}
