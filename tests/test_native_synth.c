/* Real compiled Strudel -> event resolver -> native callback, no audio device. */
#include "../src/audio.c"
#include "../src/js_audio.c"
#include <assert.h>

static void clear_voices(void) {
    for (int i=0;i<VOICE_SLOTS;i++) {
        native_synth_state_free(voices[i].native_state); native_synth_state_free(voices[i].native_right);
        stretch_release(voices[i].stretch_buffer);
    }
    memset(voices,0,sizeof(voices));
}
static AudioEvent prepare(const char *code) {
    char error[512]; cJSON *metadata=NULL;
    JsProgram *program=javascript_compile(code,120,&metadata,error,sizeof(error));
    if (!program) fprintf(stderr,"compile: %s\n%s\n",error,code);
    assert(program);
    cJSON *haps=javascript_query_cps(program,0,1,.5);
    if (!cJSON_IsArray(haps) || !cJSON_GetArraySize(haps)) fprintf(stderr,"no haps: %s\n",code);
    assert(cJSON_IsArray(haps) && cJSON_GetArraySize(haps)>0);
    AudioEvent event;
    const cJSON *hap=cJSON_GetArrayItem(haps,0), *value=cJSON_GetObjectItemCaseSensitive(hap,"value");
    assert(js_audio_event(hap,0,2,&event) && resolve_sample(&event,value) && event.native_synth);
    cJSON_Delete(haps); cJSON_Delete(metadata); javascript_free(program); return event;
}
static void render(AudioEvent *event) {
    clear_voices(); playing=1; audio_time=0; audio_set_reference_mode(1); audio_trigger_event(event);
    float out[2048]; double energy=0;
    for (int block=0;block<8;block++) {
        pa_callback(NULL,out,1024,NULL,0,NULL);
        for (int i=0;i<2048;i++) { assert(isfinite(out[i]) && fabs(out[i])<=1); energy+=out[i]*out[i]; }
    }
    assert(energy>1e-5); clear_voices();
}

static AudioEvent prepare_gm(const char *code) {
    char error[512]; cJSON *metadata=NULL;
    JsProgram *program=javascript_compile(code,120,&metadata,error,sizeof(error)); assert(program);
    cJSON *haps=javascript_query_cps(program,0,1,.5); assert(cJSON_GetArraySize(haps)==1);
    const cJSON *hap=cJSON_GetArrayItem(haps,0), *value=cJSON_GetObjectItemCaseSensitive(hap,"value");
    AudioEvent event; assert(js_audio_event(hap,0,2,&event) && resolve_sample(&event,value));
    assert(event.stretch_buffer && !event.native_synth && !event.is_drum);
    cJSON_Delete(haps); cJSON_Delete(metadata); javascript_free(program); return event;
}

static void test_retiring_resources(void) {
    const char *sources[]={"note(60).s('bytebeat').bbexpr('t*5')",
        "note(60).s('sine').K(audioin().mul(.5).out())"};
    for (unsigned source=0;source<sizeof(sources)/sizeof(sources[0]);source++) {
        clear_voices(); audio_time=0; playing=1; next_voice_onset=HUGE_VAL;
        AudioEvent event=prepare(sources[source]); audio_trigger_event(&event);
        NativeSynthState *left=voices[0].native_state,*right=voices[0].native_right;
        assert(left && (!native_synth_effect(event.native_synth) || right));
        native_synth_release(event.native_synth);
        AudioEvent silent={.time=.1,.duration=1,.inst=INST_SINE,.freq=440,.smp=-1,.wt=-1,
            .gain=0,.att=.001,.dec=.05,.sus=.6,.rel=.01,.midi_chan=-1,.midi_note=-1,
            .delay_prepared=1,.room_prepared=1};
        dsp_params_init(&silent.dsp); silent.dsp.enabled=1;
        for (int i=0;i<MAX_VOICES;i++) audio_trigger_event(&silent);
        assert(voices[MAX_VOICES].native_state==left && voices[MAX_VOICES].native_right==right);
        assert(voices[MAX_VOICES].retiring && !voices[0].native_state);
        float output[1024];
        for (int i=0;i<32;i++) pa_callback(NULL,output,512,NULL,0,NULL);
        assert(!voices[MAX_VOICES].active && voices[MAX_VOICES].native_state==left);
        clear_voices(); /* The owning state references are freed exactly once, off callback. */
    }
}

static void test_gm(void) {
    assert(gm_synth_count()==125 && gm_synth_program("gm_piano")==0 && gm_synth_program("gm_flute")==73);
    assert(gm_synth_program("gm_gunshot")==127 && gm_synth_program("gm_missing")==-1);
    if (!gm_synth_available()) { puts("GM soundfont unavailable: native rendering checks skipped"); return; }
    SoundInfo rows[256]; assert(sounds_search(SOUND_SYNTHS,"gm_",rows,256)==125);
    websamp_load_code("samples({gm_flute:['https://example.com/custom-flute.wav']})");
    assert(sounds_search(SOUND_SYNTHS,"gm_flute",rows,256)==0);
    assert(sounds_search(SOUND_USER,"gm_flute",rows,256)==1 && websamp_slot_count("gm_flute")==1);
    websamp_load_code("");
    AudioEvent flute=prepare_gm("note(60.5).s('gm_flute').release(.05)");
    AudioEvent again=prepare_gm("note(60.5).s('gm_flute').release(.05).gain(.2).lpf(1200).pan(.8)");
    assert(flute.stretch_buffer==again.stretch_buffer && stretch_age_offset(flute.stretch_buffer)==0);
    const float *pcm=stretch_pcm(flute.stretch_buffer); double energy=0;
    for (int i=0;i<stretch_frames(flute.stretch_buffer)*2;i++) { assert(isfinite(pcm[i])); energy+=pcm[i]*pcm[i]; }
    assert(energy>1 && stretch_frames(flute.stretch_buffer)==(int)ceil(2.05*SR));
    render(&again);
    AudioEvent piano=prepare_gm("note(60.5).s('gm_piano').release(.05)");
    assert(piano.stretch_buffer!=flute.stretch_buffer);
    const float *other=stretch_pcm(piano.stretch_buffer); double difference=0;
    for (int i=0;i<44100;i++) difference+=fabs(pcm[i]-other[i]);
    assert(difference>1);
    stretch_release(piano.stretch_buffer); stretch_release(again.stretch_buffer); stretch_release(flute.stretch_buffer);
    /* Exercise every registered program and cache eviction without a device. */
    for (int i=0;i<gm_synth_count();i++) {
        AudioEvent event={0}; snprintf(event.token,sizeof(event.token),"%s",gm_synth_name(i));
        event.freq=440; event.duration=.02; event.att=.001; event.dec=.001; event.sus=1; event.rel=.01; event.smp_speed=1;
        dsp_params_init(&event.dsp); assert(gm_synth_prepare(&event));
        assert(stretch_frames(event.stretch_buffer)==1323);
        stretch_release(event.stretch_buffer);
    }
    /* Keep a voice alive after preparation ownership is dropped and evicted. */
    AudioEvent held=prepare_gm("note(64).s('gm_violin').release(.01)");
    clear_voices(); playing=1; audio_time=0; audio_trigger_event(&held); stretch_release(held.stretch_buffer);
    for (int i=0;i<150;i++) {
        AudioEvent event=held; event.stretch_buffer=NULL; event.freq=100+i; event.duration=.01;
        assert(gm_synth_prepare(&event)); stretch_release(event.stretch_buffer);
    }
    float out[1024]; pa_callback(NULL,out,512,NULL,0,NULL); clear_voices();
    puts("GM programs, fractional notes, immutable PCM reuse, eviction and native callback ok");
}

static AudioEvent prepare_wavetable(const char *code) {
    char error[512]; cJSON *metadata=NULL;
    JsProgram *program=javascript_compile(code,120,&metadata,error,sizeof(error)); assert(program);
    cJSON *haps=javascript_query_cps(program,0,1,.5); assert(cJSON_GetArraySize(haps)==1);
    const cJSON *hap=cJSON_GetArrayItem(haps,0), *value=cJSON_GetObjectItemCaseSensitive(hap,"value");
    AudioEvent event; assert(js_audio_event(hap,0,2,&event) && resolve_sample(&event,value));
    assert(event.smp>=0 && event.smp_gate && !event.is_drum && event.dsp.p[DSP_LOOP]==1);
    cJSON_Delete(haps); cJSON_Delete(metadata); javascript_free(program); return event;
}

static void test_wavetable_banks(void) {
    cJSON *hap=cJSON_Parse("{\"begin\":0,\"end\":1,\"value\":{\"s\":\"wt_test\"}}");
    AudioEvent event; assert(js_audio_event(hap,0,2,&event));
    assert(!event.is_drum && event.freq>0 && event.dsp.p[DSP_LOOP]==1);
    cJSON_AddNumberToObject(cJSON_GetObjectItemCaseSensitive(hap,"value"),"loop",0);
    assert(js_audio_event(hap,0,2,&event) && event.dsp.p[DSP_LOOP]==0); cJSON_Delete(hap);
    char url[512];
    if (!websamp_default_url("wt_flute",0,url,sizeof(url))) { puts("AKWF assets unavailable: real-bank callback checks skipped"); return; }
    SoundInfo rows[256]; int count=sounds_search(SOUND_WAVETABLES,"wt_",rows,256); assert(count==65);
    for (int i=0;i<count;i++) {
        assert(rows[i].pitched && rows[i].slots>0);
        int id=websamp_acquire(rows[i].name,69); assert(id>=0);
        float *pcm; int frames,channels; smp_get_stereo(id,&pcm,&frames,&channels);
        assert(pcm && frames>2 && (channels==1 || channels==2));
    }
    AudioEvent flute=prepare_wavetable("note(69).s('wt_flute').gain(.2).sustain(1).release(.01)");
    AudioEvent variation=prepare_wavetable("note(69).s('wt_flute').n(1)"); assert(variation.smp!=flute.smp);
    AudioEvent last=prepare_wavetable("note(69).s('wt_flute').n(-1)");
    AudioEvent wrapped=prepare_wavetable("note(69).s('wt_flute').n(16)"); assert(last.smp==wrapped.smp);
    float *pcm; int frames,channels; smp_get_stereo(flute.smp,&pcm,&frames,&channels);
    clear_voices(); playing=1; audio_time=0; audio_trigger_event(&flute);
    assert(fabs(voices[0].smpstep-frames*440/SR)<1e-9);
    float out[1024]; double late_energy=0;
    for (int block=0;block<100;block++) {
        pa_callback(NULL,out,512,NULL,0,NULL);
        if (block==99) for (int i=0;i<1024;i++) { assert(isfinite(out[i])); late_energy+=out[i]*out[i]; }
    }
    assert(voices[0].active && late_energy>1e-5); /* Sustained far beyond the source's one cycle. */
    for (int block=0;block<90;block++) pa_callback(NULL,out,512,NULL,0,NULL);
    assert(!voices[0].active); clear_voices();
    AudioEvent stereo=prepare_wavetable("note(60).s('wt_stereo')");
    smp_get_stereo(stereo.smp,&pcm,&frames,&channels); assert(channels==2);
    render(&stereo);
    puts("65 AKWF banks decode offline; variant order/wrapping, exact cycle pitch, stereo and sustain/release ok");
}
int main(void) {
#ifdef __linux__
    snd_lib_error_set_handler(alsa_silent);
#endif
    test_gm();
    test_retiring_resources();
    test_wavetable_banks();
    const char *waves[]={"zzfx","z_sine","z_triangle","z_sawtooth","z_square","z_tan","z_noise"};
    for (int i=0;i<7;i++) {
        char code[256]; snprintf(code,sizeof(code),"note(60).s('%s').gain(.2).zrand(0).zdelay(.01)",waves[i]);
        AudioEvent event=prepare(code); assert(event.inst==INST_NATIVE && !event.is_drum); render(&event); native_synth_release(event.native_synth);
    }
    AudioEvent raw=prepare("s('zzfx').zzfx([.25,0,440,0,.1,.1,2,1])");
    render(&raw); native_synth_release(raw.native_synth);
    for (int i=0;i<15;i++) {
        char code[128]; snprintf(code,sizeof(code),"note(60).s('bytebeat:%d').bbst(0)",i);
        AudioEvent preset=prepare(code); render(&preset); native_synth_release(preset.native_synth);
    }
    AudioEvent beat=prepare("note(60).s('bytebeat').bbexpr('t*(t>>15^t>>66)').bbst(100)");
    render(&beat);
    NativeSynthState *a=native_synth_state_new(beat.native_synth), *b=native_synth_state_new(beat.native_synth);
    assert(a && b);
    for (int i=0;i<4096;i++) {
        double left,right,other_l,other_r,age=i/SR;
        assert(native_synth_process(a,age,2,beat.freq,0,0,0,&left,&right));
        assert(native_synth_process(b,age,2,beat.freq,64,0,0,&other_l,&other_r));
        assert(left==other_l && right==other_r); /* bbst resets time independently of song origin. */
    }
    native_synth_state_free(a); native_synth_state_free(b);
    native_synth_release(beat.native_synth);
    const char *graphs[]={
        "note(60).K(sine(sFreq).mul(.1).out())",
        "note(60).K(saw(sFreq).mul(sGate.adsr(.001,.1,.5,.1)).mul(.1).out())",
        "note(60).K(sine(sFreq).add(x=>x.delay(.01).mul(.3)).mul(.1).out())",
        "note(60).K(()=>{ const f=n(sFreq).mul(S(\"1 2\")); sine(f).mul(.1).out(); })",
        "note(60).s('sine').K(audioin().mul(.5).out())",
    };
    for (size_t i=0;i<sizeof(graphs)/sizeof(*graphs);i++) { AudioEvent event=prepare(graphs[i]); render(&event); native_synth_release(event.native_synth); }
    AudioEvent effect=prepare("note(60).K(audioin().mul(.5).out())");
    playing=1; audio_trigger_event(&effect);
    VoiceMix mix={0}; route_voice(&voices[0],.25,-.5,&mix);
    assert(fabs(mix.left-.125)<1e-9 && fabs(mix.right+.25)<1e-9);
    clear_voices(); native_synth_release(effect.native_synth);
    cJSON *bad=cJSON_Parse("{\"s\":\"bytebeat\",\"_nativeProgram\":{\"kind\":\"bytebeat\",\"result\":0,\"ops\":[{\"op\":\"+\",\"value\":0,\"ins\":[999]}]}}");
    assert(bad && !native_synth_prepare(bad,440,1)); cJSON_Delete(bad);
    char error[512]; cJSON *metadata=NULL;
    JsProgram *invalid=javascript_compile("note(60).K(audioin().mouseX().out())",120,&metadata,error,sizeof(error));
    assert(!invalid); cJSON_Delete(metadata);
    metadata=NULL;
    invalid=javascript_compile("s('bytebeat').bbexpr('(()=>{while(true){}})()')",120,&metadata,error,sizeof(error));
    assert(!invalid); cJSON_Delete(metadata);
    /* More than a full cache of distinct programs must not invalidate a live
       state, and released programs must leave room for new live-code edits. */
    cJSON *parameters=cJSON_Parse("{\"s\":\"z_sine\",\"zrand\":0}"); assert(parameters);
    const NativeSynth *held=native_synth_prepare(parameters,440,.1); assert(held);
    NativeSynthState *state=native_synth_state_new(held); assert(state); native_synth_release(held);
    for (int i=0;i<300;i++) {
        const NativeSynth *other=native_synth_prepare(parameters,100+i,.1); assert(other);
        native_synth_release(other);
    }
    double left,right; assert(native_synth_process(state,.01,.1,440,0,0,0,&left,&right));
    native_synth_state_free(state); cJSON_Delete(parameters);
    /* Generated room preparation is cached, bounded, and a real convolution. */
    const ConvKernel *room=convolution_prepare_reverb(.2,.01,15000,1000);
    assert(room && room==convolution_prepare_reverb(.2,.01,15000,1000));
    assert(!convolution_prepare_reverb(10,.1,15000,1000));
    static ConvState conv; convolution_set(&conv,room); double wet_energy=0;
    for (int i=0;i<15000;i++) {
        convolution_process(&conv,i==0?1:0,i==0?1:0,&left,&right);
        assert(isfinite(left) && isfinite(right)); wet_energy+=left*left+right*right;
    }
    assert(wet_energy>1e-5);
    puts("native ZzFX, bytebeat, Kabelsalat source/effect/feedback and callback checks ok"); return 0;
}
