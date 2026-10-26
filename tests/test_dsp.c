#include "dsp.h"
#include "js_audio.h"
#include "audio.h"
#include "track.h"
#include "scheduler.h"
#include "banks.h"
#include "sampler.h"
#include <assert.h>
#include <dirent.h>
#include <sys/wait.h>
#include <unistd.h>

static int source_event(const char *text,AudioEvent *event) {
    cJSON *hap=cJSON_CreateObject(); assert(hap);
    cJSON_AddNumberToObject(hap,"begin",0);cJSON_AddNumberToObject(hap,"end",1);
    cJSON *value=cJSON_Parse(text); assert(value);
    cJSON_AddItemToObject(hap,"value",value);
    int result=js_audio_event(hap,0,1,event);
    if (result) result=js_audio_resolve_sample(event,value);
    cJSON_Delete(hap);return result;
}

static void test_source_controls(void) {
    AudioEvent event;
    assert(source_event("{\"s\":\"pulse\",\"note\":69,\"pwrate\":3}",&event));
    assert(event.inst==INST_PULSE && event.dsp.p[DSP_PWSWEEP]==.3);
    assert(source_event("{\"s\":\"pulse\",\"pwsweep\":0.4}",&event));
    assert(event.dsp.p[DSP_PWRATE]==1);
    assert(source_event("{\"s\":\"sawtooth\",\"n\":1}",&event));
    assert(event.inst==INST_ADDITIVE && event.dsp.periodic && event.dsp.n_partials==1);
    DspState state;dsp_init(&state,&event.dsp);
    assert(fabs(dsp_additive(&state,M_PI/2,440)+1)<1e-6);
    const DspPeriodic *first=event.dsp.periodic;
    assert(source_event("{\"s\":\"sawtooth\",\"n\":1,\"cutoff\":1000}",&event));
    assert(event.dsp.periodic==first); /* Effects do not duplicate source assets. */
    assert(source_event("{\"s\":\"triangle\",\"n\":1}",&event));
    dsp_init(&state,&event.dsp);assert(fabs(dsp_additive(&state,0,440)-1)<1e-6);
    assert(source_event("{\"s\":\"user\",\"partials\":[1],\"phases\":[0.25]}",&event));
    dsp_init(&state,&event.dsp);assert(fabs(dsp_additive(&state,0,440)+1)<1e-6);
    assert(source_event("{\"s\":\"sine\",\"partials\":[0,0,1]}",&event));
    assert(event.inst==INST_SINE && !event.dsp.periodic);
    assert(source_event("{\"s\":\"sawtooth\",\"n\":0}",&event) && event.inst==INST_SAW);
    assert(!source_event("{\"s\":\"sawtooth\",\"n\":-1}",&event));
    assert(source_event("{\"s\":\"square\",\"n\":2047}",&event));
    dsp_init(&state,&event.dsp);
    for (int i=0;i<2048;i++) assert(isfinite(dsp_additive(&state,i*.01,20+i*12)));
    assert(source_event("{\"s\":\"sawtooth\",\"noise\":1}",&event));
    assert(event.dsp.noise_mix==1 && event.dsp.source_noise);
    const float *pink=event.dsp.source_noise;
    assert(source_event("{\"s\":\"pink\"}",&event) && event.dsp.source_noise==pink);
    dsp_init(&state,&event.dsp);
    double initial=dsp_source_noise(&state);
    for (int i=1;i<88200;i++) assert(isfinite(dsp_source_noise(&state)));
    assert(dsp_source_noise(&state)==initial); /* Same two-second web loop. */
    state.source_noise_position=88200-.5;
    assert(fabs(dsp_source_noise(&state)-(pink[88199]+pink[0])*.5)<1e-8);
    assert(state.source_noise_position==.5);
}

static void test_periodic_limits(void) {
    pid_t child=fork();assert(child>=0);
    if (!child) {
        DspParams p;dsp_params_init(&p);p.periodic_kind=4;p.n_partials=1;p.partials[0]=3.14159;
        assert(dsp_prepare_periodic(&p));const DspPeriodic *retained=p.periodic;
        int full=0;
        for (int i=0;i<257;i++) {
            p.periodic=NULL;p.partials[0]=i+10;
            if (!dsp_prepare_periodic(&p)) {full=1;break;}
        }
        assert(full);
        p.partials[0]=3.14159;assert(dsp_prepare_periodic(&p) && p.periodic==retained);
        p.periodic_numeric=0;p.n_partials=33;assert(!dsp_prepare_periodic(&p));
        p.periodic_numeric=1;p.n_partials=2048;assert(!dsp_prepare_periodic(&p));
        _exit(0);
    }
    int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
}

static void test_supersaw(void) {
    DspParams params; dsp_params_init(&params);
    assert(params.p[DSP_UNISON]==5 && params.p[DSP_SPREAD]==.6 && params.supersaw_detune==.18);
    params.enabled=1;
    DspState first,second; dsp_init(&first,&params); dsp_init(&second,&params);
    dsp_supersaw_init(&first,1); dsp_supersaw_init(&second,2);
    assert(first.supersaw_voices==5 && first.supersaw_gain==1/sqrt(5));
    assert(fabs(first.supersaw_pan_l-sqrt(.2))<1e-15);
    assert(fabs(first.supersaw_pan_r-sqrt(.8))<1e-15);
    for (unsigned i=0;i<5;i++) {
        assert(first.saw_phase[i]>0 && first.saw_phase[i]<1);
        assert(first.saw_phase[i]!=second.saw_phase[i]);
        if (i) assert(first.saw_phase[i]!=first.saw_phase[i-1]);
    }
    assert(fabs(first.saw_ratio[0]*first.saw_ratio[4]-1)<1e-15);
    params.p[DSP_UNISON]=1; params.p[DSP_SPREAD]=1;
    dsp_init(&first,&params); dsp_supersaw_init(&first,1);
    double phase=first.saw_phase[0];
    assert(first.saw_ratio[0]==1);
    dsp_supersaw(&first,440);
    assert(fabs(first.supersaw_l-(2*phase-1)*sqrt(.5))<1e-15);
    assert(first.supersaw_l==first.supersaw_r);
    params.p[DSP_UNISON]=100;
    dsp_init(&first,&params); dsp_supersaw_init(&first,1);
    assert(first.supersaw_voices==100);
    for (int i=0;i<4096;i++) assert(isfinite(dsp_supersaw(&first,12000)));
    params.p[DSP_UNISON]=1.000001; params.supersaw_detune=9600;
    dsp_init(&first,&params); dsp_supersaw_init(&first,1);
    for (int i=0;i<512;i++) assert(isfinite(dsp_supersaw(&first,12000)));
    for (unsigned i=0;i<first.supersaw_voices;i++) assert(isfinite(first.saw_phase[i]));
    cJSON *hap=cJSON_Parse("{\"begin\":0,\"end\":1,\"value\":{\"s\":\"supersaw\",\"note\":69,\"n\":0.5,\"detune\":12}}");
    assert(hap); AudioEvent event;
    assert(js_audio_event(hap,0,1,&event));
    assert(event.freq==440 && event.dsp.supersaw_detune==12);
    cJSON *value=cJSON_GetObjectItemCaseSensitive(hap,"value");
    cJSON_DeleteItemFromObjectCaseSensitive(value,"detune");
    assert(js_audio_event(hap,0,1,&event) && event.dsp.supersaw_detune==.5);
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(value,"s"),"sawtooth");
    cJSON_AddNumberToObject(value,"detune",1200);
    assert(js_audio_event(hap,0,1,&event) && event.freq==440);
    cJSON_Delete(hap);
}

static void test_standalone_file_event(void) {
    pid_t child=fork(); assert(child>=0);
    if (!child) {
        char root[]="/tmp/tompouce-file-event-XXXXXX"; assert(mkdtemp(root));
        assert(!setenv("XDG_CACHE_HOME",root,1) && !setenv("XDG_CONFIG_HOME",root,1));
        assert(!setenv("TOMPOUCE_BANKS",root,1));
        char path[512]; snprintf(path,sizeof(path),"%s/My Drum.wav",root);
        const unsigned char wav[]={ 'R','I','F','F',40,0,0,0,'W','A','V','E','f','m','t',' ',
            16,0,0,0,1,0,1,0,68,172,0,0,136,88,1,0,2,0,16,0,'d','a','t','a',4,0,0,0,0,32,0,192 };
        FILE *file=fopen(path,"wb"); assert(file);
        assert(fwrite(wav,1,sizeof(wav),file)==sizeof(wav)); assert(!fclose(file));
        banks_start_index(root);
        for (int i=0;!banks_ready() && i<500;i++) usleep(10000);
        assert(banks_ready());
        cJSON *metadata=NULL; char error[512];
        JsProgram *program=javascript_compile("s(pure('My Drum.wav'))",120,&metadata,error,sizeof(error)); assert(program);
        js_audio_schedule(program,0,0,event_queue_generation());
        assert(event_queue_depth()==1);
        char queued[2048]; assert(scheduler_debug_queue(queued,sizeof(queued),1)>0);
        assert(strstr(queued,"smp=0 "));
        float *pcm=NULL; int frames=0,channels=0;
        smp_get_stereo(0,&pcm,&frames,&channels);
        assert(pcm && frames==2 && channels==1 && fabs(pcm[0]-.25)<1e-6);
        event_queue_clear(); javascript_free(program); cJSON_Delete(metadata);
        assert(!unlink(path));
        snprintf(path,sizeof(path),"%s/tompouce/banks.ldb",root);
        DIR *dir=opendir(path); assert(dir); struct dirent *entry;
        while ((entry=readdir(dir))) {
            if (!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
            char item[1024]; snprintf(item,sizeof(item),"%s/%s",path,entry->d_name); assert(!unlink(item));
        }
        closedir(dir); assert(!rmdir(path));
        snprintf(path,sizeof(path),"%s/tompouce",root); assert(!rmdir(path)); assert(!rmdir(root));
        _exit(0);
    }
    int status; assert(waitpid(child,&status,0)==child); assert(WIFEXITED(status) && !WEXITSTATUS(status));
}

static double rms(const char *controls, double hz) {
    cJSON *json = cJSON_Parse(controls);
    assert(json);
    DspParams parameters; DspState state;
    dsp_params_read(&parameters,json);
    dsp_init(&state,&parameters);
    cJSON_Delete(json);
    double sum = 0;
    for (int i = 0; i < 44100; i++) {
        double age = i/SR;
        double x = dsp_process(&state,sin(2*M_PI*hz*age),age,2);
        assert(isfinite(x));
        if (i > 4410) sum += x*x;
    }
    return sqrt(sum/39689);
}

static void test_filter_envelopes(void) {
    for (int type = 0; type < 3; type++) {
        for (int sign = -1; sign <= 1; sign += 2) {
            DspParams p;
            dsp_params_init(&p);
            int base = type*7;
            p.p[base] = 1000; p.p[base+2] = sign*2;
            p.p[DSP_ANCHOR] = .5;
            p.p[base+3] = .2; p.p[base+4] = .2;
            p.p[base+5] = .5; p.p[base+6] = .2;
            double start = sign > 0 ? 500 : 2000;
            double peak = sign > 0 ? 2000 : 500;
            assert(fabs(dsp_filter_frequency(&p,type,0,1)-start) < 1e-8);
            assert(fabs(dsp_filter_frequency(&p,type,.1,1)-1000) < 1e-8);
            assert(fabs(dsp_filter_frequency(&p,type,.2,1)-peak) < 1e-8);
            assert(fabs(dsp_filter_frequency(&p,type,.5,1)-1250) < 1e-8);
            assert(fabs(dsp_filter_frequency(&p,type,1.2,1)-start) < 1e-8);
            /* A short note ramps to the interpolated endpoint at gate. */
            assert(fabs(dsp_filter_frequency(&p,type,.1,.1)-1250) < 1e-8);
            assert(fabs(dsp_filter_frequency(&p,type,.3,.1)-start) < 1e-8);
        }
    }
}

/* The cached-activity fast paths must be bit-identical to the full paths.
   controls are immutable per voice, so forcing the flags reproduces what the
   slow path would have computed. */
static void test_inactive_fast_paths(void) {
    DspParams params;
    dsp_params_init(&params);
    params.enabled = 1;
    DspState fast, slow;
    dsp_init(&fast, &params);
    dsp_init(&slow, &params);
    assert(!fast.effects_active && !fast.pitch_active && !fast.fm_active);
    slow.effects_active = 1; slow.pitch_active = 1; slow.fm_active = 1;
    for (int i = 0; i < 4096; i++) {
        double age = i / SR;
        double x = sin(i * .37) * (i % 7 ? .3 : 90);
        assert(dsp_process(&fast, x, age, 1) == dsp_process(&slow, x, age, 1));
        assert(dsp_frequency(&fast, 440, age, 1) == dsp_frequency(&slow, 440, age, 1));
        assert(dsp_frequency(&fast, 1234.5, age, 1) == 1234.5);
        assert(dsp_pitch_multiplier(&params, age, 1) == 1);
    }
    /* Guard rails: post gain and any filter/pitch control must take the full
       path, and the derived flags must notice them. */
    DspParams post; dsp_params_init(&post); post.enabled = 1; post.p[DSP_POST] = 2;
    DspState post_state; dsp_init(&post_state, &post);
    assert(post_state.effects_active);
    DspParams pitch; dsp_params_init(&pitch); pitch.enabled = 1; pitch.p[DSP_PENV] = 12;
    DspState pitch_state; dsp_init(&pitch_state, &pitch);
    assert(pitch_state.pitch_active && !pitch_state.effects_active);
    DspParams fm; dsp_params_init(&fm); fm.enabled = 1; fm.p[DSP_FMBASE] = 2;
    DspState fm_state; dsp_init(&fm_state, &fm);
    assert(fm_state.fm_active);
    puts("inactive DSP fast paths are bit-identical");
}

static void test_fm_source_semantics(void) {
    cJSON *value=cJSON_Parse("{\"fmi\":0.5,\"fmh\":2}"); assert(value);
    DspParams p; dsp_params_read(&p,value);
    assert(!p.fm_envelopes);
    DspState state; dsp_init(&state,&p);
    dsp_oscillator_onset(&state,.274);
    /* First pulled modulator block starts at phase zero. The carrier reads
       76 of its values, then the next full modulator block starts at 128. */
    for (int i=0;i<300;i++) {
        int frame=i<76?i:i+52;
        double expected=440*(1+sin(2*M_PI*880*frame/SR));
        assert(fabs(dsp_frequency(&state,440,i/SR,.5)-expected)<1e-8);
    }
    cJSON_AddNumberToObject(value,"fmattack",.1);
    dsp_params_read(&p,value); assert(p.fm_envelopes==1);
    cJSON_Delete(value);
}

static void test_pitch_envelope_defaults(void) {
    DspParams p;
    cJSON *value=cJSON_Parse("{\"penv\":12}"); assert(value);
    dsp_params_read(&p,value); cJSON_Delete(value);
    assert(p.p[DSP_PATT]==.2 && p.p[DSP_PDEC]==.001 && p.p[DSP_PSUS]==1);
    assert(p.p[DSP_PREL]==.001 && p.p[DSP_PANCHOR]==1);
    assert(fabs(dsp_pitch_multiplier(&p,0,1)-.5)<1e-12);
    assert(fabs(dsp_pitch_multiplier(&p,.1,1)-sqrt(.5))<1e-12);
    assert(dsp_pitch_multiplier(&p,.2,1)==1);
    value=cJSON_Parse("{\"pdecay\":0.1}"); assert(value);
    dsp_params_read(&p,value); cJSON_Delete(value);
    assert(p.p[DSP_PENV]==1 && p.p[DSP_PATT]==.001 && p.p[DSP_PDEC]==.1);
    assert(p.p[DSP_PSUS]==.001 && p.p[DSP_PANCHOR]==.001 && p.p[DSP_PREL]==.01);
    value=cJSON_Parse("{\"penv\":-7,\"pattack\":0,\"pdecay\":0.2,\"psustain\":0,\"panchor\":0}"); assert(value);
    dsp_params_read(&p,value); cJSON_Delete(value);
    assert(p.p[DSP_PATT]==.001 && p.p[DSP_PANCHOR]==0);
    assert(dsp_pitch_multiplier(&p,0,.5)==1);
    assert(fabs(dsp_pitch_multiplier(&p,.001,.5)-pow(2,-7/12.0))<1e-12);
    assert(fabs(dsp_pitch_multiplier(&p,.101,.5)-pow(2,-3.5/12))<1e-12);
    DspState state; dsp_init(&state,&p); dsp_oscillator_onset(&state,.5);
    assert(state.pitch_initial_frames==94);
    assert(fabs(state.pitch_quantum_offset-34/SR)<1e-15);
    assert(dsp_frequency(&state,440,0,.5)==440);
    assert(fabs(dsp_frequency(&state,440,94/SR,.5)-440*dsp_pitch_multiplier(&p,94/SR,.5))<1e-12);
    assert(!state.pitch_initial_frames);
    dsp_init(&state,&p); dsp_oscillator_onset(&state,.274);
    assert(state.pitch_initial_frames==76);
    assert(fabs(state.pitch_onset_fraction-.6/SR)<1e-14);
}

static void test_exponential_pitch(void) {
    DspParams p; dsp_params_init(&p); p.enabled=1;
    p.p[DSP_PCURVE]=1; p.p[DSP_PENV]=12; p.p[DSP_PANCHOR]=.5;
    p.p[DSP_PATT]=.1; p.p[DSP_PDEC]=.2; p.p[DSP_PSUS]=.5; p.p[DSP_PREL]=.2;
    /* AudioParam ramps across a sign change hold their start value. */
    assert(fabs(dsp_pitch_multiplier(&p,.05,1)-pow(2,-.5))<1e-12);
    assert(fabs(dsp_pitch_multiplier(&p,.1,1)-pow(2,.5))<1e-12);
    /* Decimal segment rounding makes this endpoint slightly negative, so
       this decay also crosses zero and holds until the endpoint. */
    assert(fabs(dsp_pitch_multiplier(&p,.2,1)-pow(2,.5))<1e-12);
    assert(fabs(dsp_pitch_multiplier(&p,.5,1)-1)<1e-12);
    assert(dsp_pitch_multiplier(&p,1.1,1)==1);
    assert(fabs(dsp_pitch_multiplier(&p,1.2,1)-pow(2,-.5))<1e-12);
    /* With exact binary durations a zero ramp endpoint becomes .001 cents. */
    p.p[DSP_PATT]=.125; p.p[DSP_PDEC]=.25;
    double midpoint=sqrt(600*.001);
    assert(fabs(dsp_pitch_multiplier(&p,.25,1)-pow(2,midpoint/1200))<1e-12);
    /* Short gates clip endpoints linearly before exponential interpolation. */
    p.p[DSP_PANCHOR]=1; p.p[DSP_PATT]=.5;
    double endpoint=-1200+(1200+.001)*.1;
    double cents=-sqrt(1200*-endpoint);
    assert(fabs(dsp_pitch_multiplier(&p,.025,.05)-pow(2,cents/1200))<1e-12);
}

int main(void) {
    test_exponential_pitch();
    test_pitch_envelope_defaults();
    /* Public Superdough .room(1) supplies no duration override: the first
       shared room uses generate()'s two-second default. */
    cJSON *room_defaults=cJSON_Parse("{\"room\":1}"); assert(room_defaults);
    DspParams room_params; dsp_params_read(&room_params,room_defaults);
    assert(room_params.p[DSP_SIZE]==2 && room_params.p[DSP_ROOMFADE]==.1);
    assert(room_params.p[DSP_ROOMLP]==15000 && room_params.p[DSP_ROOMDIM]==1000);
    assert(convolution_prepare_reverb(room_params.p[DSP_SIZE],
        room_params.p[DSP_ROOMFADE],room_params.p[DSP_ROOMLP],room_params.p[DSP_ROOMDIM]));
    cJSON_Delete(room_defaults);
    /* Generated-room sequences vary per call: consecutive distinct rooms use
       disjoint stream segments (no reseeding to identical kernels), while
       repeats hit the cache. */
    convolution_reverb_reset_stream();
    const ConvKernel *first=convolution_prepare_reverb(2,.1,15000,1000);
    const ConvKernel *second=convolution_prepare_reverb(1,.1,15000,1000);
    assert(first && second && first!=second);
    assert(memcmp(first->head,second->head,sizeof(first->head)));
    assert(convolution_prepare_reverb(2,.1,15000,1000)==first);
    assert(convolution_prepare_reverb(1,.1,15000,1000)==second);
    test_standalone_file_event();
    double alias_naive = 0, alias_filtered = 0;
    for (int i = 0; i < 256; i++) {
        double phase = 2*M_PI*i*3/16;
        double t = phase/(2*M_PI);
        double reference = sin(2*M_PI*i/16);
        alias_naive += (2*(t-floor(t+.5)))*reference;
        alias_filtered += dsp_oscillator(0,phase,SR*3/16,.5)*reference;
    }
    assert(fabs(alias_filtered) < fabs(alias_naive)*.6);
    assert(fabs(dsp_oscillator(2,0,440,.5)) < 1e-9);
    assert(fabs(dsp_oscillator(2,M_PI/2,440,.5)-1) < 1e-9);
    test_filter_envelopes();
    /* WebAudio low/high-pass resonance is in dB, rather than linear Q. */
    assert(fabs(rms("{\"cutoff\":1000,\"resonance\":10}",1000)-sqrt(5)) < .01);
    assert(fabs(rms("{\"hcutoff\":1000,\"hresonance\":10}",1000)-sqrt(5)) < .01);
    /* Attack alone holds the destination instead of decaying back. */
    cJSON *bass = cJSON_Parse("{\"cutoff\":800,\"lpenv\":-4,\"lpattack\":0.1}");
    assert(bass);
    DspParams bass_params;
    dsp_params_read(&bass_params,bass);
    assert(bass_params.p[DSP_LPF+4] == .001);
    assert(bass_params.p[DSP_LPF+5] == 1);
    cJSON_Delete(bass);
    assert(rms("{\"cutoff\":800,\"lpenv\":-4,\"lpattack\":0.1}",2000) >
           rms("{\"cutoff\":50}",2000)*10);
    assert(rms("{\"hcutoff\":1000}",100) < rms("{\"hcutoff\":1000}",5000)*.1);
    assert(rms("{\"cutoff\":1000}",5000) < rms("{\"cutoff\":1000}",100)*.1);
    assert(rms("{\"bandf\":1000,\"bandq\":4}",100) < rms("{\"bandf\":1000,\"bandq\":4}",1000)*.1);
    assert(rms("{\"postgain\":0}",1000) == 0);
    assert(rms("{\"compressor\":-30,\"compressorRatio\":20}",1000) < .1);
    /* Bandlimited saw versus the truncated Fourier series of the same ramp.
       440 Hz keeps about 50 harmonics under Nyquist. PolyBLEP should sit
       closer to that series than the naive saw. */
    {
        double poly_err = 0, naive_err = 0;
        const int harmonics = (int)(SR * .5 / 440);
        for (int i = 0; i < 512; i++) {
            double phase = 2 * M_PI * 440 * i / SR;
            double t = phase / (2 * M_PI); t -= floor(t);
            double shifted = t + .5; shifted -= floor(shifted);
            double series = 0;
            for (int k = 1; k <= harmonics; k++) series += sin(2 * M_PI * k * shifted) / k;
            series *= -2 / M_PI;
            double naive = 2 * shifted - 1;
            double poly = dsp_oscillator(0, phase, 440, .5);
            poly_err += (poly - series) * (poly - series);
            naive_err += (naive - series) * (naive - series);
        }
        assert(poly_err < naive_err * .5);
    }
    assert(fabs(dsp_envelope(.05,1,.1,.2,.5,.2)-.5) < 1e-9);
    assert(fabs(dsp_envelope(.3,1,.1,.2,.5,.2)-.5) < 1e-9);
    assert(dsp_envelope(1.3,1,.1,.2,.5,.2) == 0);
    (void)rms("{\"cutoff\":1000,\"lpenv\":8,\"hcutoff\":100,\"bandf\":2000,\"vowel\":\"a\",\"coarse\":8,\"crush\":4,\"distort\":8,\"phaserrate\":2}",200);

    cJSON *hap = cJSON_Parse("{\"begin\":0.25,\"end\":0.5,\"value\":{\"note\":\"fs4\",\"s\":\"supersaw\",\"hcutoff\":400,\"pan\":0,\"fmi\":2,\"fmh\":1.5,\"begin\":0.2,\"loop\":1}}");
    assert(hap);
    AudioEvent event;
    assert(js_audio_event(hap,10,2,&event));
    assert(event.time == 10.5 && event.duration == .5);
    assert(fabs(event.freq-midi_to_freq(66)) < 1e-9);
    assert(event.inst == INST_SUPERSAW && event.pan == -1);
    assert(event.gain == .8 && event.att == .001 && event.dec == .05 && event.sus == .6 && event.rel == .01);
    assert(event.dsp.p[DSP_HPF] == 400 && event.dsp.p[DSP_BEGIN] == .2);
    assert(event.dsp.p[DSP_FMBASE] == 2 && event.dsp.p[DSP_FMBASE+1] == 1.5);
    cJSON_Delete(hap);
    hap = cJSON_Parse("{\"begin\":0,\"end\":1,\"value\":{\"gain\":0}}");
    assert(js_audio_event(hap,0,2,&event) && event.gain==0 && event.dsp.enabled);
    cJSON_Delete(hap);
    assert(!js_audio_event(NULL,0,2,&event));

    assert(track_load_from_code("const voice = note('c4'); voice.hpf(400).gain(0)._scope().pianoroll()"));
    assert(track_count == 1 && tracks[0].javascript);
    assert(tracks[0].visual_requests == 3);
    JsProgram *playing = tracks[0].javascript;
    unsigned generation = event_queue_generation();
    assert(!track_load_from_code("const voice = ;"));
    assert(tracks[0].javascript == playing && event_queue_generation() == generation);
    javascript_retain(playing);
    assert(track_load_from_code("note(\"c4 e4\").gain(0)"));
    assert(tracks[0].javascript && tracks[0].javascript!=playing);
    cJSON *haps = javascript_query(playing,0,1);
    assert(cJSON_GetArraySize(haps) == 1);
    cJSON_Delete(haps); javascript_free(playing);
    event_queue_clear();
    memset(&event,0,sizeof(event));
    event.generation = generation;
    event_queue_push(&event);
    assert(event_queue_depth() == 0);
    event.generation = event_queue_generation();
    event_queue_push(&event);
    assert(event_queue_depth() == 1);
    event_queue_clear();
    puts("native DSP/Strudel audio controls ok");
    test_inactive_fast_paths();
    test_fm_source_semantics();
    test_supersaw();
    test_source_controls();
    test_periodic_limits();
    return 0;
}
