/* Real compiled Strudel -> event resolver -> native callback, no audio device. */
#include "../src/audio.c"
#include "../src/js_audio.c"
#include <assert.h>

static void clear_voices(void) {
    for (int i=0;i<MAX_VOICES;i++) { native_synth_state_free(voices[i].native_state); native_synth_state_free(voices[i].native_right); }
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
int main(void) {
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
