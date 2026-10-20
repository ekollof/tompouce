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

static int constant_sample(void);
static void render_interval(float *out, int frames, unsigned long block);

static void drum_note(AudioEvent *note, const char *token, int inst, double gain) {
    memset(note,0,sizeof(*note));
    note->time=.1; note->duration=1; note->inst=inst; note->freq=440;
    note->smp=-1; note->wt=-1; note->gain=gain;
    note->att=.001; note->dec=.001; note->sus=1; note->rel=.1;
    note->midi_chan=-1; note->midi_note=-1;
    note->delay_prepared=1; note->room_prepared=1; note->is_drum=1;
    snprintf(note->token,sizeof(note->token),"%s",token);
    dsp_params_init(&note->dsp); note->dsp.enabled=1;
}

/* Stolen drums fade like every other evicted source. The deployed pool ramps
   each evicted entry to zero at the incoming target plus 250 ms regardless of
   source type (bd/sd/hh captures all show ramp+stop pairs in FIFO order). */
static void test_drum_retirement(void) {
    wt_init();
    static float reference[44100*2],pressured[44100*2];
    memset(voices,0,sizeof(voices)); audio_time=0; next_voice_onset=HUGE_VAL;
    voice_onset_serial=0; retirement_overflow=0;
    audio_set_reference_mode(1); audio_toggle_play(1);
    AudioEvent tom;
    drum_note(&tom,"tom",INST_TOM,.1);
    audio_trigger_event(&tom);
    render_interval(reference,44100,128);
    /* Pool pressure: the audible TOM is oldest, 127 silent TOMs fill the
       pool, and a silent TOM evicts it at .3 (retire_end .55). */
    memset(voices,0,sizeof(voices)); audio_time=0; next_voice_onset=HUGE_VAL;
    voice_onset_serial=0; retirement_overflow=0;
    audio_trigger_event(&tom);
    tom.gain=0;
    for (int i=1;i<MAX_VOICES;i++) audio_trigger_event(&tom);
    assert(audio_active_voices()==MAX_VOICES);
    render_interval(pressured,8960,128);
    tom.time=.3; audio_trigger_event(&tom);
    assert(voices[MAX_VOICES].active && voices[MAX_VOICES].retiring);
    assert(fabs(voices[MAX_VOICES].retire_end-.55)<1e-12);
    assert(!retirement_overflow);
    double anchor=voices[MAX_VOICES].retire_anchor,level=voices[MAX_VOICES].retire_level;
    double span=voices[MAX_VOICES].retire_end-anchor; assert(span>0);
    double trigged=8960.0/SR;
    render_interval(pressured+8960*2,44100-8960,128);
    /* The retired TOM is the solo TOM scaled by the retirement fade; the
       silent pool voices contribute nothing. Afterwards it stops dead. */
    for (int frame=0;frame<44100;frame++) {
        double t=frame/SR;
        float expected;
        if (frame>=24255) expected=0;
        else if (t<trigged || t<anchor) expected=reference[frame*2];
        else expected=(float)(reference[frame*2]*level*(.55-t)/span);
        assert(fabs(pressured[frame*2]-expected)<2e-6 && fabs(pressured[frame*2+1]-expected)<2e-6);
    }
    memset(voices,0,sizeof(voices)); next_voice_onset=HUGE_VAL;
}

/* Mixed deterministic types under pool pressure: FIFO eviction, the same
   +250 ms stop deadline for every source type, bounded retiring graphs and
   bit-identical rendering across callback sizes. */
static void test_mixed_pool_saturation(void) {
    wt_init();
    int sample=constant_sample();
    static float reference[44100*2],actual[44100*2];
    AudioEvent sine={.time=.1,.duration=1,.inst=INST_SINE,.freq=440,
        .smp=-1,.wt=-1,.gain=.05,.att=.01,.dec=.05,.sus=.8,.rel=.1,
        .midi_chan=-1,.midi_note=-1,.delay_prepared=1,.room_prepared=1};
    dsp_params_init(&sine.dsp); sine.dsp.enabled=1;
    AudioEvent kick; drum_note(&kick,"bd",INST_KICK,.05);
    AudioEvent tom; drum_note(&tom,"tom",INST_TOM,.05);
    AudioEvent samp={.time=.1,.duration=1,.inst=INST_SINE,.freq=midi_to_freq(60),
        .smp=sample,.smp_speed=1,.smp_gate=1,.wt=-1,.gain=.05,
        .att=.01,.dec=.05,.sus=.8,.rel=.1,
        .midi_chan=-1,.midi_note=-1,.delay_prepared=1,.room_prepared=1};
    dsp_params_init(&samp.dsp); samp.dsp.enabled=1;
    for (int pass=0;pass<2;pass++) {
        memset(voices,0,sizeof(voices)); audio_time=0; next_voice_onset=HUGE_VAL;
        voice_onset_serial=0; retirement_overflow=0;
        audio_set_reference_mode(1); audio_toggle_play(1);
        for (int i=0;i<MAX_VOICES;i++) {
            AudioEvent note=sine;
            if (i%4==1) note=kick; else if (i%4==2) note=tom; else if (i%4==3) note=samp;
            audio_trigger_event(&note);
        }
        assert(audio_active_voices()==MAX_VOICES);
        AudioEvent evictor=sine; evictor.time=.3; evictor.gain=0;
        audio_trigger_event(&evictor);
        assert(audio_active_voices()==MAX_VOICES);
        assert(voices[MAX_VOICES].active && voices[MAX_VOICES].retiring);
        assert(fabs(voices[MAX_VOICES].retire_end-.55)<1e-12);
        assert(!retirement_overflow);
        render_interval(pass?actual:reference,44100,pass?8192:1);
    }
    assert(!memcmp(reference,actual,sizeof(reference)));
    memset(voices,0,sizeof(voices)); next_voice_onset=HUGE_VAL;
}

static void test_cut_ordering(void) {
    int sample=constant_sample();
    memset(voices,0,sizeof(voices)); next_voice_onset=HUGE_VAL;
    voice_onset_serial=0; audio_time=0;
    audio_set_reference_mode(1); audio_toggle_play(1);
    /* Three sampled voices in group 1, one in group 2, one oscillator with cut=1. */
    AudioEvent base={.time=.1,.duration=1,.inst=INST_SINE,.freq=440,
        .smp=sample,.smp_speed=1,.smp_gate=1,.wt=-1,.gain=.1,
        .att=.001,.dec=.001,.sus=1,.rel=.1,
        .midi_chan=-1,.midi_note=-1,.delay_prepared=1,.room_prepared=1};
    dsp_params_init(&base.dsp); base.dsp.enabled=1; base.dsp.p[DSP_CUT]=1;
    audio_trigger_event(&base);
    base.time=.12; audio_trigger_event(&base);
    AudioEvent other=base; other.time=.14; other.dsp.p[DSP_CUT]=2;
    audio_trigger_event(&other);
    AudioEvent osc={.time=.16,.duration=1,.inst=INST_SINE,.freq=440,
        .smp=-1,.wt=-1,.gain=.1,.att=.001,.dec=.001,.sus=1,.rel=.1,
        .midi_chan=-1,.midi_note=-1,.delay_prepared=1,.room_prepared=1};
    dsp_params_init(&osc.dsp); osc.dsp.enabled=1; osc.dsp.p[DSP_CUT]=1;
    audio_trigger_event(&osc);
    /* Same start time as the next cutter: onset_serial decides ordering. */
    AudioEvent same=base; same.time=.2; audio_trigger_event(&same);
    AudioEvent cutter=base; cutter.time=.2; cutter.gain=.1;
    audio_trigger_event(&cutter);
    /* Earlier group-1 sampled voices are cut; group 2 untouched. The first
       cutter wins: the .1 voice cut at .12 keeps that time, later voices cut
       at .2. Same-time ordering uses onset_serial. */
    int cut_group1=0, cut_group2=0;
    for (int i=0;i<VOICE_SLOTS;i++) {
        if (!voices[i].active || !voices[i].sampled_source) continue;
        if (voices[i].dsp.control.p[DSP_CUT]==2) {
            assert(!voices[i].cut_pending);
            cut_group2++;
        } else if (voices[i].start_time<=.2+1e-12 && voices[i].onset_serial<voice_onset_serial) {
            /* Exclude the newest cutter itself (largest onset_serial). */
            if (voices[i].onset_serial==voice_onset_serial) continue;
            assert(voices[i].cut_pending);
            assert(voices[i].cut_time<=.2+1e-12);
            cut_group1++;
        }
    }
    assert(cut_group1==3 && cut_group2==1);
    /* The .1 voice keeps its earliest cut at .12; .12 and same-time voices cut at .2. */
    int saw_early=0, saw_ontime=0;
    for (int i=0;i<VOICE_SLOTS;i++) {
        if (!voices[i].active || !voices[i].sampled_source) continue;
        if (voices[i].dsp.control.p[DSP_CUT]!=1 || !voices[i].cut_pending) continue;
        if (fabs(voices[i].cut_time-.12)<1e-12) saw_early=1;
        if (fabs(voices[i].cut_time-.2)<1e-12) saw_ontime=1;
    }
    assert(saw_early && saw_ontime);
    /* Oscillator with cut=1 is not part of the sampled cut group. */
    for (int i=0;i<VOICE_SLOTS;i++)
        if (voices[i].active && !voices[i].sampled_source)
            assert(!voices[i].cut_pending);
    /* Re-cutting the same group does not re-arm already-cut voices. */
    AudioEvent cutter2=base; cutter2.time=.25;
    audio_trigger_event(&cutter2);
    for (int i=0;i<VOICE_SLOTS;i++) {
        if (!voices[i].active || !voices[i].sampled_source) continue;
        if (fabs(voices[i].start_time-.2)<1e-12 && voices[i].dsp.control.p[DSP_CUT]==1)
            assert(fabs(voices[i].cut_time-.2)<1e-12 || fabs(voices[i].cut_time-.25)<1e-12);
    }
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

static void test_empty_source_eviction(void) {
    memset(voices,0,sizeof(voices)); audio_time=0; next_voice_onset=HUGE_VAL;
    voice_onset_serial=0; retirement_overflow=0;
    AudioEvent note={.time=.1,.duration=1,.inst=INST_SINE,.freq=440,.smp=-1,.wt=-1,
        .gain=0,.att=.02,.dec=.03,.sus=.6,.rel=.1,.midi_chan=-1,.midi_note=-1,
        .delay_prepared=1,.room_prepared=1};
    dsp_params_init(&note.dsp); note.dsp.enabled=1;
    for(int i=0;i<MAX_VOICES;i++) audio_trigger_event(&note);
    AudioEvent absent={.time=.3,.control_only=1,.eviction_only=1,.stretch=1};
    audio_trigger_event(&absent);
    assert(audio_active_voices()==MAX_VOICES-1 && !voices[0].active);
    assert(voices[MAX_VOICES].active && voices[MAX_VOICES].onset_serial==1);
    assert(fabs(voices[MAX_VOICES].retire_end-.51)<1e-12);
    audio_trigger_event(&absent); /* The vacated Map entry stays free. */
    assert(!voices[MAX_VOICES+1].active && audio_active_voices()==MAX_VOICES-1);
    audio_trigger_event(&note); assert(voices[0].onset_serial==MAX_VOICES+1);
    absent.stretch=0; audio_trigger_event(&absent);
    assert(!voices[1].active && voices[MAX_VOICES+1].onset_serial==2);
    assert(fabs(voices[MAX_VOICES+1].retire_end-.55)<1e-12);
    memset(voices,0,sizeof(voices)); next_voice_onset=HUGE_VAL;
}

static void test_coincident_retirement_endpoint(void) {
    memset(voices,0,sizeof(voices)); audio_time=0; next_voice_onset=HUGE_VAL;
    AudioEvent note={.time=.1,.duration=.7,.inst=INST_SINE,.freq=440,.smp=-1,.wt=-1,
        .gain=.1,.att=.4,.dec=.3,.sus=.2,.rel=.4,.midi_chan=-1,.midi_note=-1,
        .delay_prepared=1,.room_prepared=1};
    dsp_params_init(&note.dsp); note.dsp.enabled=1;
    for(int i=0;i<MAX_VOICES;i++) {
        audio_trigger_event(&note); voices[i].freq=0; voices[i].phase=M_PI/2; note.gain=0;
    }
    static float pcm[44100*2]; render_interval(pcm,8960,128);
    note.time=.25; audio_trigger_event(&note);
    render_interval(pcm+8960*2,44100-8960,128);
    for(int i=8960;i<44100;i++) {
        double expected=i<22050?.03*(i/SR-.1)/.4:0;
        assert(fabs(pcm[i*2]-expected)<1e-7 && fabs(pcm[i*2+1]-expected)<1e-7);
    }
    memset(voices,0,sizeof(voices)); next_voice_onset=HUGE_VAL;
}

static void render_retiring_effect_tail(int effect, unsigned long block, float *pcm) {
    memset(voices,0,sizeof(voices)); audio_time=0; next_voice_onset=HUGE_VAL;
    voice_onset_serial=0; retirement_overflow=0;
    int sample=constant_sample();
    /* HPF/BPF remove DC, so those cases use a sine source for tail energy. */
    int sine_source=(effect==1||effect==2);
    AudioEvent note={.time=.1,.duration=.7,.inst=INST_SINE,.freq=midi_to_freq(60),.smp=sine_source?-1:sample,.wt=-1,
        .gain=.1,.smp_speed=1,.smp_gate=1,.att=.001,.dec=.001,.sus=1,.rel=.1,
        .midi_chan=-1,.midi_note=-1,.delay_prepared=1,.room_prepared=1};
    dsp_params_init(&note.dsp); note.dsp.enabled=1;
    switch (effect) {
    case 0: note.dsp.p[DSP_LPF]=100; note.dsp.p[DSP_LPQ]=10; break;
    case 1: note.dsp.p[DSP_HPF]=5000; note.dsp.p[DSP_HPQ]=1; break;
    case 2: note.dsp.p[DSP_BPF]=1000; note.dsp.p[DSP_BPQ]=2; break;
    case 3: note.dsp.p[DSP_VOWEL]=0; break;
    case 4: note.dsp.p[DSP_PHASER]=2; note.dsp.p[DSP_PHDEPTH]=.75; break;
    case 5: note.dsp.p[DSP_CHORUS]=.5; break;
    case 6: note.dsp.p[DSP_COARSE]=8; break;
    default: assert(0);
    }
    for(int i=0;i<MAX_VOICES;i++) {audio_trigger_event(&note);note.gain=0;}
    render_interval(pcm,8960,block); note.time=.3; audio_trigger_event(&note);
    render_interval(pcm+8960*2,26460-8960,block);
    assert(voices[MAX_VOICES].active && audio_time>voices[MAX_VOICES].retire_end);
    render_interval(pcm+26460*2,44100-26460,block);
    assert(!voices[MAX_VOICES].active);
    double energy=0;
    for(int frame=24255;frame<26460;frame++) energy+=pcm[frame*2]*pcm[frame*2];
    /* Coarse hold freezes the last value for up to one hold period after the
       source stops, then renders silence; stateful filters and chorus retain
       audible ring-out past retire_end. The retired phaser is bypassed (the
       web's eviction gain sits downstream of it), so its tail is silent. */
    if (effect==6) {
        for(int frame=24400;frame<26460;frame++) assert(pcm[frame*2]==0 && pcm[frame*2+1]==0);
    } else if (effect==4) {
        for(int frame=24255;frame<26460;frame++) assert(pcm[frame*2]==0 && pcm[frame*2+1]==0);
    } else {
        assert(energy>1e-12);
    }
    for(int frame=39691;frame<44100;frame++) assert(pcm[frame*2]==0 && pcm[frame*2+1]==0);
}

static void test_retiring_filter_tail(void) {
    static float reference[44100*2],actual[44100*2];
    for (int effect=0;effect<7;effect++) {
        render_retiring_effect_tail(effect,1,reference);
        render_retiring_effect_tail(effect,8192,actual);
        assert(!memcmp(reference,actual,sizeof(reference)));
    }
    memset(voices,0,sizeof(voices)); next_voice_onset=HUGE_VAL;
}

static void voice_reference(void) {
    size_t capacity=1024*1024; char *input=malloc(capacity); assert(input);
    size_t length=fread(input,1,capacity-1,stdin);
    assert(!ferror(stdin) && feof(stdin)); input[length]=0;
    cJSON *tests=cJSON_Parse(input),*results=cJSON_CreateArray(); assert(tests && results);
    int sample=constant_sample();
    const cJSON *test;
    cJSON_ArrayForEach(test,tests) {
        memset(voices,0,sizeof(voices)); audio_time=0; next_voice_onset=HUGE_VAL;
        voice_onset_serial=0; pending_duck_count=pending_delay_count=0;
        audio_set_reference_mode(1); audio_toggle_play(1);
        const cJSON *value=cJSON_GetObjectItemCaseSensitive(test,"value");
        int synth=!strcmp(cJSON_GetObjectItemCaseSensitive(test,"sound")->valuestring,"sine");
        AudioEvent note={.time=.1,.duration=cJSON_GetObjectItemCaseSensitive(test,"duration")->valuedouble,
            .inst=INST_SINE,.freq=synth?0:midi_to_freq(60),.smp=synth?-1:sample,.wt=-1,
            .gain=.1,.smp_speed=1,.smp_gate=1,.midi_chan=-1,.midi_note=-1,
            .delay_prepared=1,.room_prepared=1};
        const char *names[]={"attack","decay","sustain","release"};
        const double defaults[]={.001,.05,.6,.01}; double envelope[4];
        dsp_read_adsr(value,names,defaults,envelope);
        note.att=envelope[0]; note.dec=envelope[1]; note.sus=envelope[2]; note.rel=envelope[3];
        dsp_params_read(&note.dsp,value);
        const cJSON *cut=cJSON_GetObjectItemCaseSensitive(test,"cut");
        for(int i=0;i<MAX_VOICES;i++) {
            audio_trigger_event(&note); voices[i].phase=M_PI/2;
            if(synth) voices[i].freq=0;
            note.gain=0;
        }
        if(cJSON_IsTrue(cut)) {voices[0].dsp.control.p[DSP_CUT]=1;note.dsp.p[DSP_CUT]=1;}
        static float pcm[44100*2];
        render_interval(pcm,8960,128);
        note.time=cJSON_GetObjectItemCaseSensitive(test,"incoming")->valuedouble;
        audio_trigger_event(&note);
        render_interval(pcm+8960*2,44100-8960,128);
        cJSON *points=cJSON_CreateArray(); assert(points);
        const cJSON *point;
        cJSON_ArrayForEach(point,cJSON_GetObjectItemCaseSensitive(test,"points")) {
            int frame=cJSON_GetObjectItemCaseSensitive(point,"frame")->valueint;
            assert(frame>=0 && frame<44100);
            cJSON_AddItemToArray(points,cJSON_CreateNumber(pcm[frame*2]/(synth?.03:.0125)));
        }
        cJSON_AddItemToArray(results,points);
    }
    char *json=cJSON_PrintUnformatted(results); assert(json); puts(json); free(json);
    cJSON_Delete(tests); cJSON_Delete(results); free(input);
}

int main(int argc, char **argv) {
    log_console_stderr(0);
    if(argc>1 && !strcmp(argv[1],"--voice-reference")) {voice_reference();return 0;}
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
    test_cut_ordering();
    test_drum_retirement();
    test_mixed_pool_saturation();
    test_retirement_pcm();
    test_retirement_capacity();
    test_empty_source_eviction();
    test_coincident_retirement_endpoint();
    test_retiring_filter_tail();
    puts("Fractional/reverse PCM, FIFO retirement fades, cut ordering, drum fades, mixed-type saturation, per-voice effect tails and independent duck callbacks survive 1..8192-frame bursts; retirement capacity is bounded and observable");
    return 0;
}
