#include <assert.h>
#include <string.h>
#include <sys/stat.h>
#include "../src/audio.c"
#include "js_audio.h"
#include "scheduler.h"
#include "prep_pool.h"
#include <unistd.h>
#include <time.h>

static int sample_fixture(void) {
    /* Eight stereo PCM16 frames with opposite channel polarities. */
    unsigned char wav[76] = {
        'R','I','F','F',68,0,0,0,'W','A','V','E','f','m','t',' ',
        16,0,0,0,1,0,2,0,68,172,0,0,16,177,2,0,4,0,16,0,
        'd','a','t','a',32,0,0,0
    };
    char temporary[] = "/tmp/tompouce-audio-XXXXXX";
    char path[128];
    int fd = mkstemp(temporary);
    assert(fd >= 0);
    for (int i = 44; i < 76; i += 4) {
        wav[i+1] = 32; wav[i+3] = 192;
    }
    assert(write(fd, wav, sizeof(wav)) == (ssize_t)sizeof(wav));
    assert(close(fd) == 0);
    int n = snprintf(path, sizeof(path), "%s.wav", temporary);
    assert(n > 0 && (size_t)n < sizeof(path));
    assert(rename(temporary, path) == 0);
    int id = smp_acquire_abs(path, 36);
    assert(unlink(path) == 0);
    assert(id >= 0);
    return id;
}

static Voice *start_sample(AudioEvent *event) {
    for (int i=0;i<MAX_VOICES;i++) stretch_release(voices[i].stretch_buffer);
    memset(voices, 0, sizeof(voices));
    /* These source/DSP checks render from onset; timestamp waiting is covered
       separately by the live scheduler/callback harness. */
    audio_time=fmax(audio_time,event->time);
    audio_trigger_event(event);
    assert(voices[0].active);
    return &voices[0];
}

static void test_sample_pitch_quantum(void) {
    Voice v={0}; DspParams params; dsp_params_init(&params); params.enabled=1;
    params.p[DSP_PENV]=12; params.p[DSP_PATT]=.1;
    dsp_init(&v.dsp,&params);
    v.start_time=50.25/SR; v.gate_end=1; v.sample_pitch_quantum=-1;
    /* A partial first quantum still reads pre-onset detune zero. */
    assert(sample_pitch_ratio(&v,51/SR)==1);
    assert(sample_pitch_ratio(&v,127/SR)==1);
    /* First evaluation clamps the initial -1200-cent event to this boundary. */
    double expected=.5;
    assert(sample_pitch_ratio(&v,128/SR)==expected);
    assert(sample_pitch_ratio(&v,255/SR)==expected);
    double next=pow(2,-1+128/(.1*SR-(128-50.25)));
    assert(fabs(sample_pitch_ratio(&v,256/SR)-next)<1e-12);
    params.p[DSP_PENV]=0; dsp_init(&v.dsp,&params);
    assert(sample_pitch_ratio(&v,512/SR)==1);
}

static void test_sample_connection_lifetime(void) {
    AudioEvent event={.smp=sample_fixture(),.smp_speed=1,.duration=2,.rel=.01};
    dsp_params_init(&event.dsp); event.dsp.enabled=1;
    assert(sample_connection_duration(&event,44100)==1);
    event.smp_speed=8; event.fx_release=1.5;
    assert(sample_connection_duration(&event,44100)==.125);
    event.smp_gate=1;
    assert(sample_connection_duration(&event,44100)==.125);
    event.smp_speed=-2;
    assert(sample_connection_duration(&event,44100)==.5);
    event.dsp.p[DSP_LOOP]=1;
    assert(fabs(sample_connection_duration(&event,44100)-2.02)<1e-12);
    event.dsp.p[DSP_LOOP]=0; event.smp_gate=0; event.smp_speed=1;
    event.dsp.p[DSP_PENV]=12; event.dsp.p[DSP_PATT]=event.dsp.p[DSP_PDEC]=.001;
    event.dsp.p[DSP_PSUS]=1; event.dsp.p[DSP_PANCHOR]=0;
    /* First quantum is unity; the remaining source PCM runs at double rate. */
    double predicted=sample_connection_duration(&event,44100);
    assert(fabs(predicted-(128+(44100-128)/2.0)/SR)<1e-12);
    event.dsp.p[DSP_PENV]=0; event.dsp.p[DSP_VIB]=5; event.smp_speed=.001;
    /* Extremely long modulation hits the bounded conservative fallback. */
    assert(fabs(sample_connection_duration(&event,44100)-1000.02)<1e-9);
}

static void test_sample_windows(void) {
    float output[32];
    AudioEvent event = {0};
    event.inst = INST_TRI; event.smp = sample_fixture();
    event.smp_speed = 1; event.gain = .1; event.sus = 1;
    event.duration = 10/SR; event.rel = 4/SR;
    dsp_params_init(&event.dsp); event.dsp.enabled = 1;
    event.dsp.p[DSP_BEGIN] = .5; event.dsp.p[DSP_END] = .75;
    Voice *sample = start_sample(&event);
    assert(sample->smp_channels == 2 && sample->dsp_right.control.enabled);
    assert(fabs(sample->gate_end-sample->start_time-2/SR) < 1e-12);
    pa_callback(NULL, output, 1, NULL, 0, NULL);
    assert(sample->active && sample->smppos == 5);
    assert(fabs(output[0]-.025) < 1e-8 && fabs(output[1]+.05) < 1e-8);
    pa_callback(NULL, output, 1, NULL, 0, NULL);
    assert(sample->active && sample->smppos == 6);
    pa_callback(NULL, output, 1, NULL, 0, NULL);
    assert(sample->active && sample->released && sample->smppos == 7);
    pa_callback(NULL, output, 1, NULL, 0, NULL);
    assert(!sample->active && sample->smppos == 8);

    /* An explicit clip/release uses the hap duration, even past end. */
    event.smp_gate = 1;
    sample = start_sample(&event);
    assert(fabs(sample->gate_end-sample->start_time-10/SR) < 1e-12);
    pa_callback(NULL, output, 3, NULL, 0, NULL);
    assert(sample->active && !sample->released && sample->smppos == 7);

    /* Reversing the buffer keeps begin/end relative to playback direction. */
    event.smp_gate = 0; event.smp_speed = -1;
    event.dsp.p[DSP_BEGIN] = .25; event.dsp.p[DSP_END] = .5;
    sample = start_sample(&event);
    assert(sample->smppos == 5);
    pa_callback(NULL, output, 3, NULL, 0, NULL);
    assert(sample->active && sample->released && sample->smppos == 2);

    /* Loops play the prefix before loopBegin, then keep looping in release. */
    event.smp_speed = 1; event.smp_gate = 1;
    event.duration = 3/SR; event.rel = 2/SR;
    event.dsp.p[DSP_BEGIN] = 0; event.dsp.p[DSP_END] = .25;
    event.dsp.p[DSP_LOOP] = 1;
    event.dsp.p[DSP_LOOPBEGIN] = .25; event.dsp.p[DSP_LOOPEND] = .5;
    sample = start_sample(&event);
    pa_callback(NULL, output, 1, NULL, 0, NULL);
    assert(sample->active && sample->smppos == 1);
    pa_callback(NULL, output, 3, NULL, 0, NULL);
    assert(sample->active && sample->released && sample->smppos == 2);
    pa_callback(NULL, output, 3, NULL, 0, NULL);
    assert(!sample->active);

    /* Stereo effects must initialize and process both channels. */
    event.dsp.p[DSP_LOOP] = 0;
    event.dsp.p[DSP_LPF] = 800; event.dsp.p[DSP_LPQ] = 10;
    sample = start_sample(&event);
    pa_callback(NULL, output, 1, NULL, 0, NULL);
    assert(sample->dsp_right.control.p[DSP_LPF] == 800);
    assert(sample->dsp.filter[0].z1 != 0 && sample->dsp_right.filter[0].z1 != 0);
    assert(fabs(output[1]+2*output[0]) < 1e-8);

    /* Legacy sample slices retain their hard end. */
    event.dsp.enabled = 0; event.smp_end = .5;
    sample = start_sample(&event);
    pa_callback(NULL, output, 4, NULL, 0, NULL);
    assert(!sample->active && sample->smppos == 4);
}

/* The deployed renderer applies the FX chain's default gain before the
   chain and the main gain after it. Native keeps them separate so nonlinear
   stages see the source gain order. */
static void test_fx_graph_gain(void) {
    static const char *base =
        "{\"begin\":0,\"end\":1,\"value\":{\"note\":69,\"s\":\"sine\","
        "\"gain\":0.3,\"clip\":0.1,"
        "\"_fxSteps\":[{\"steps\":[]}],%s}}";
    char text[512];
    const char *cases[] = {
        "\"_nativeProgram\":{\"kind\":\"graph\",\"effect\":false}",
        "\"_nativeProgram\":{\"kind\":\"graph\",\"effect\":true}",
        "\"x\":0"
    };
    const double expected_chain[] = {1, 0.8, 0.8};
    for (int i=0;i<3;i++) {
        snprintf(text,sizeof(text),base,cases[i]);
        cJSON *hap=cJSON_Parse(text); assert(hap);
        AudioEvent event;
        assert(js_audio_event(hap,0,2,&event));
        assert(fabs(event.gain-0.3)<1e-12);
        assert(fabs(event.fx_chain_gain-expected_chain[i])<1e-12);
        cJSON_Delete(hap);
    }
}

static void test_convolution(void) {
    enum { FRAMES = 513, INPUT = 777 };
    float impulse[FRAMES*2], signal[INPUT*2];
    for (int i = 0; i < FRAMES; i++) {
        impulse[i*2] = (float)(sin(i*1.73)*.03);
        impulse[i*2+1] = (float)(cos(i*.91)*.02);
    }
    for (int i = 0; i < INPUT*2; i++) signal[i] = (float)(sin(i*.177)*.2);
    ConvKernel *kernel = convolution_kernel(impulse,FRAMES,2,0);
    ConvState *state = calloc(1,sizeof(*state));
    assert(kernel && state);
    assert(convolution_state_reserve(state,kernel->parts));
    convolution_set(state,kernel);
    for (int frame = 0; frame < INPUT+FRAMES; frame++) {
        double left, right;
        convolution_process(state,frame < INPUT ? signal[frame*2] : 0,
            frame < INPUT ? signal[frame*2+1] : 0,&left,&right);
        double expected[2] = {0};
        for (int tap = 0; tap < FRAMES; tap++) {
            int source = frame-tap;
            if (source < 0 || source >= INPUT) continue;
            for (int ch = 0; ch < 2; ch++) expected[ch] += signal[source*2+ch]*impulse[tap*2+ch];
        }
        assert(fabs(left-expected[0]) < 1e-6 && fabs(right-expected[1]) < 1e-6);
    }
    convolution_set(state,NULL);
    convolution_set(state,kernel);
    double left, right;
    convolution_process(state,0,0,&left,&right);
    assert(left == 0 && right == 0);
    convolution_set(state,NULL); convolution_kernel_free(kernel);
    float delta[2] = {1,1};
    kernel = convolution_kernel(delta,1,1,1);
    assert(kernel);
    convolution_set(state,kernel);
    convolution_process(state,1,-2,&left,&right);
    assert(fabs(left-.00125) < 1e-9 && fabs(right+.0025) < 1e-9);
    convolution_kernel_free(kernel); convolution_state_free(state); free(state);
}

static void test_stretch(void) {
    cJSON *hap = cJSON_Parse("{\"begin\":0,\"end\":1,\"value\":{\"s\":\"piano\",\"speed\":0.5,\"stretch\":1}}");
    AudioEvent event;
    assert(hap && js_audio_event(hap,0,2,&event));
    assert(event.stretch == 2 && event.smp_speed == .5);
    cJSON_Delete(hap);
    hap=cJSON_Parse("{\"begin\":0,\"end\":1,\"value\":{\"s\":\"piano\",\"stretch\":-4}}");
    assert(hap && !js_audio_event(hap,0,2,&event)); cJSON_Delete(hap);
    int frames = 22050;
    char path[]="/tmp/tompouce-stretch-XXXXXX.wav";
    int fd=mkstemps(path,4); assert(fd>=0);
    unsigned char header[44]={'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,
        1,0,2,0,68,172,0,0,16,177,2,0,4,0,16,0,'d','a','t','a',0,0,0,0};
    unsigned bytes=(unsigned)frames*4;
    for (int i=0;i<4;i++) { header[4+i]=(unsigned char)((bytes+36)>>(i*8)); header[40+i]=(unsigned char)(bytes>>(i*8)); }
    assert(write(fd,header,sizeof(header))==(ssize_t)sizeof(header));
    for (int i=0;i<frames;i++) {
        int left=(int)(8191*sin(2*M_PI*440*i/SR)),right=-2*left;
        unsigned char sample[4]={(unsigned char)left,(unsigned char)((unsigned)left>>8),(unsigned char)right,(unsigned char)((unsigned)right>>8)};
        assert(write(fd,sample,sizeof(sample))==(ssize_t)sizeof(sample));
    }
    assert(close(fd)==0); int sample=smp_acquire_abs(path,60); assert(sample>=0); assert(unlink(path)==0);
    /* Explicit PCM oracle for the web's reverseBuffer result. Its right
       channel starts with zero, followed by the reversed channel at offset 1. */
    char reverse_path[]="/tmp/tompouce-stretch-reverse-XXXXXX.wav";
    fd=mkstemps(reverse_path,4); assert(fd>=0);
    assert(write(fd,header,sizeof(header))==(ssize_t)sizeof(header));
    float *original=NULL; int original_frames=0,original_channels=0;
    smp_get_stereo(sample,&original,&original_frames,&original_channels);
    assert(original && original_frames==frames && original_channels==2);
    for (int i=0;i<frames;i++) {
        int left=(int)lrint(original[(frames-1-i)*2]*32768);
        int right=i?(int)lrint(original[(frames-i)*2+1]*32768):0;
        unsigned char pcm[4]={(unsigned char)left,(unsigned char)((unsigned)left>>8),
            (unsigned char)right,(unsigned char)((unsigned)right>>8)};
        assert(write(fd,pcm,sizeof(pcm))==(ssize_t)sizeof(pcm));
    }
    assert(!close(fd)); int reverse_sample=smp_acquire_abs(reverse_path,60);
    assert(reverse_sample>=0 && !unlink(reverse_path));
    for (int reverse = 0; reverse < 2; reverse++) {
        memset(&event,0,sizeof(event));
        event.smp=sample; event.smp_speed=reverse?-.5:.5; event.stretch=2;
        event.time=.25; event.gain=.1; event.sus=1; event.rel=.01;
        dsp_params_init(&event.dsp); event.dsp.enabled=1;
        event.stretch_buffer=stretch_prepare(&event); assert(event.stretch_buffer);
        const StretchBuffer *again=stretch_prepare(&event); assert(again==event.stretch_buffer); stretch_release(again);
        AudioEvent filtered=event; filtered.gain=.2; filtered.dsp.p[DSP_LPF]=800;
        again=stretch_prepare(&filtered); assert(again==event.stretch_buffer); stretch_release(again);
        const StretchBuffer *reference=NULL;
        if (reverse) {
            AudioEvent oracle=event; oracle.smp=reverse_sample; oracle.smp_speed=.5;
            reference=stretch_prepare(&oracle); assert(reference);
            assert(stretch_frames(reference)==stretch_frames(event.stretch_buffer));
        }
        Voice *v=start_sample(&event); stretch_release(event.stretch_buffer);
        assert(v->stretch_buffer && stretch_frames(v->stretch_buffer)>40000 && stretch_frames(v->stretch_buffer)<44100);
        double previous = 0; int crossings = 0;
        for (int i = 0; i < frames*2; i++) {
            float output[2]; pa_callback(NULL,output,1,NULL,0,NULL);
            assert(isfinite(output[0]) && isfinite(output[1]));
            if (reference) for (int ch=0;ch<2;ch++) {
                double expected=i<stretch_frames(reference)?stretch_pcm(reference)[i*2+ch]*.1:0;
                assert(fabs(output[ch]-expected)<1e-6);
            } else assert(fabs(output[1]+2*output[0]) < 1e-6);
            if (i > 4410 && i < 39690 && previous <= 0 && output[0] > 0) crossings++;
            previous = output[0];
        }
        assert(!v->active); /* .5 transport doubles duration, not pitch */
        double frequency = crossings/.8;
        assert(frequency > 430 && frequency < 450);
        stretch_release(reference);
    }
    stretch_release(voices[0].stretch_buffer); voices[0].stretch_buffer=NULL;
    event.stretch=18; assert(!stretch_prepare(&event));
    event.stretch=2; event.time=1e308; assert(!stretch_prepare(&event)); event.time=.25;
    event.stretch=2; event.dsp.p[DSP_VIB]=NAN; assert(!stretch_prepare(&event));
    event.dsp.p[DSP_VIB]=0; event.smp_speed=.5; event.smp_gate=1; event.duration=.1;
    event.stretch_buffer=stretch_prepare(&event); assert(event.stretch_buffer);
    /* Only the queue owns this PCM while more than a full cache of variants
       are prepared. After dequeue, the sounding voice must keep it alive. */
    event_queue_clear(); event_queue_push(&event); stretch_release(event.stretch_buffer);
    for (int i=0;i<160;i++) {
        AudioEvent variant=event; variant.time=.5+i/SR;
        const StretchBuffer *buffer=stretch_prepare(&variant); assert(buffer); stretch_release(buffer);
    }
    Voice *v=start_sample(&event); event_queue_clear();
    float output[2048]; pa_callback(NULL,output,1024,NULL,0,NULL);
    double energy=0;
    for (int i=0;i<2048;i++) { assert(isfinite(output[i])); energy+=output[i]*output[i]; }
    assert(energy>1e-5);
    stretch_release(v->stretch_buffer); v->stretch_buffer=NULL;
}

static void test_pattern_scopes(void) {
    memset(voices,0,sizeof(voices));
    memset(pattern_scope_generation,0,sizeof(pattern_scope_generation));
    AudioEvent a = {0}, b;
    a.inst = INST_SINE; a.smp = -1; a.freq = 220; a.gain = .02;
    a.duration = 2; a.att = 0; a.rel = .01; a.sus = 1; a.generation = 123;
    a.scope_mask = 1;
    b = a; b.freq = 880; b.gain = .04; b.scope_mask = 2;
    audio_trigger_event(&a); audio_trigger_event(&b);
    playing = 1;
    float output[512], first[256], second[256], absent[256];
    pa_callback(NULL,output,256,NULL,0,NULL);
    assert(audio_pattern_scope_copy(1,123,first,256) == 256);
    assert(audio_pattern_scope_copy(2,123,second,256) == 256);
    assert(audio_pattern_scope_copy(4,123,absent,256) == 256);
    double energy_a = 0, energy_b = 0, difference = 0;
    for (int i = 0; i < 256; i++) {
        assert(absent[i] == 0);
        energy_a += first[i]*first[i]; energy_b += second[i]*second[i];
        difference += fabs(first[i]-second[i]);
        assert(fabs(first[i]+second[i]-(output[2*i]+output[2*i+1])*.5) < 1e-7);
    }
    assert(energy_a > 1e-5 && energy_b > energy_a && difference > .01);
    float without_scopes[512];
    memset(voices,0,sizeof(voices));
    a.scope_mask = b.scope_mask = 0;
    audio_trigger_event(&a); audio_trigger_event(&b);
    pa_callback(NULL,without_scopes,256,NULL,0,NULL);
    assert(!memcmp(output,without_scopes,sizeof(output))); // Taps do not change audio.
    memset(voices,0,sizeof(voices));
    a.scope_mask = 1; b.scope_mask = 2;
    audio_trigger_event(&a); audio_trigger_event(&b);
    a.generation = 124;
    audio_trigger_event(&a);
    voices[2].amp = 0; // Silent replacement voice in the new generation.
    pa_callback(NULL,output,256,NULL,0,NULL);
    audio_pattern_scope_copy(1,124,first,256);
    for (int i = 0; i < 256; i++) assert(first[i] == 0);
    audio_pattern_scope_copy(2,123,second,256);
    energy_b = 0;
    for (int i = 0; i < 256; i++) energy_b += second[i]*second[i];
    assert(energy_b > 1e-5); // Other pattern remains audible and scoped.
    audio_pattern_scope_copy(1,125,first,256);
    for (int i = 0; i < 256; i++) assert(first[i] == 0); // No stale prior-generation data.
    memset(voices,0,sizeof(voices));
}

static void test_analysers(void) {
    memset(voices,0,sizeof(voices));
    memset(analyser_generation,0,sizeof(analyser_generation));
    for (int i=0;i<N_ANALYSERS;i++)
        atomic_store_explicit(&analyser_fft_size[i],0,memory_order_relaxed);
    AudioEvent a = {0}, b;
    a.inst = INST_SINE; a.smp = -1; a.freq = 220; a.gain = .02;
    a.time = audio_time; a.duration = 2; a.att = 0; a.rel = .01; a.sus = 1;
    a.generation = 321;
    a.analyser_id = 1; a.analyser_fft = 4; /* 2 ** (4 + 5) = 512 */
    b = a; b.freq = 880; b.gain = .04; b.analyser_id = 2; b.analyser_fft = 0;
    audio_trigger_event(&a); audio_trigger_event(&b);
    playing = 1;
    float output[512], first[256], second[256], absent[256];
    pa_callback(NULL,output,256,NULL,0,NULL);
    assert(audio_analyser_copy(1,321,first,256) == 256);
    assert(audio_analyser_copy(2,321,second,256) == 256);
    assert(audio_analyser_copy(3,321,absent,256) == 256);
    assert(audio_analyser_fft_size(1) == 512);
    assert(audio_analyser_fft_size(2) == 32);
    assert(audio_analyser_fft_size(3) == 0);
    double energy_a = 0, energy_b = 0, difference = 0;
    for (int i = 0; i < 256; i++) {
        assert(absent[i] == 0);
        energy_a += first[i]*first[i]; energy_b += second[i]*second[i];
        difference += fabs(first[i]-second[i]);
        assert(fabs(first[i]+second[i]-(output[2*i]+output[2*i+1])*.5) < 1e-7);
    }
    assert(energy_a > 1e-5 && energy_b > energy_a && difference > .01);

    /* Analyser taps do not change the rendered master output. */
    float without_taps[512];
    memset(voices,0,sizeof(voices));
    a.analyser_id = b.analyser_id = 0;
    audio_trigger_event(&a); audio_trigger_event(&b);
    pa_callback(NULL,without_taps,256,NULL,0,NULL);
    assert(!memcmp(output,without_taps,sizeof(output)));

    /* A re-evaluated source discards the old generation's tap, while an
       untouched analyser keeps publishing. */
    memset(voices,0,sizeof(voices));
    a.analyser_id = 1; b.analyser_id = 2;
    a.time = b.time = audio_time;
    audio_trigger_event(&a); audio_trigger_event(&b);
    a.generation = 322;
    a.time = audio_time;
    audio_trigger_event(&a);
    voices[2].amp = 0; /* Silent replacement voice in the new generation. */
    pa_callback(NULL,output,256,NULL,0,NULL);
    audio_analyser_copy(1,322,first,256);
    for (int i = 0; i < 256; i++) assert(first[i] == 0);
    audio_analyser_copy(2,321,second,256);
    energy_b = 0;
    for (int i = 0; i < 256; i++) energy_b += second[i]*second[i];
    assert(energy_b > 1e-5);
    audio_analyser_copy(1,323,first,256);
    for (int i = 0; i < 256; i++) assert(first[i] == 0);

    /* Four-frame staged blocks publish the same taps after routing. */
    memset(voices,0,sizeof(voices));
    memset(analyser_generation,0,sizeof(analyser_generation));
    a.generation = 400; a.analyser_id = 3; a.analyser_fft = 5; /* 1024 */
    a.freq = 440; a.gain = .1; a.time = audio_time;
    audio_trigger_event(&a);
    float warm[2];
    pa_callback(NULL,warm,1,NULL,0,NULL); /* dispatch the onset first */
    float staged[8];
    pa_callback(NULL,staged,4,NULL,0,NULL);
    float tap[64];
    assert(audio_analyser_copy(3,400,tap,64) == 64);
    assert(audio_analyser_fft_size(3) == 1024);
    double staged_energy = 0;
    for (int i = 0; i < 64; i++) staged_energy += tap[i]*tap[i];
    assert(staged_energy > 1e-9);
    assert(tap[63] > 0);

    memset(voices,0,sizeof(voices));
    memset(analyser_generation,0,sizeof(analyser_generation));
    next_voice_onset = HUGE_VAL;
}

/* analyze/fft, LV2 presets/state/parameters are event controls, not DSP
   parameters; scheduling them must not produce the unknown-control warning. */
static void test_analyser_controls_do_not_warn(void) {
    double old_time=audio_time; int old_playing=playing;
    playing=1; audio_set_reference_mode(1); audio_time=0;
    memset(voices,0,sizeof(voices));
    assert(prep_pool_start(0,0));
    event_queue_clear();
    const char *patterns[]={
        "note(60).s('sine').analyze(7).fft(4)",
        "note(60).s('sine').lv2preset('Quiet')",
        "note(60).s('sine').lv2state('state.ttl')",
        "note(60).s('sine').lv2param('cut',.5)",
        NULL};
    for (int i=0;patterns[i];i++) {
        cJSON *metadata=NULL; char error[512];
        JsProgram *program=javascript_compile(patterns[i],120,&metadata,error,
            sizeof(error));
        assert(program); cJSON_Delete(metadata);
        unsigned generation=event_queue_generation();
        assert(js_audio_schedule(program,0,0,generation));
        javascript_free(program);
    }
    int warned=0;
    for (int i=0;i<console_count_get();i++) {
        const char *line=console_line(i);
        if (strstr(line,"control 'analyze' is not implemented") ||
            strstr(line,"control 'fft' is not implemented") ||
            strstr(line,"control 'lv2preset' is not implemented") ||
            strstr(line,"control 'lv2state' is not implemented") ||
            strstr(line,"control 'lv2param_cut' is not implemented")) warned=1;
    }
    assert(!warned);
    event_queue_clear();
    prep_pool_stop();
    audio_set_reference_mode(0); playing=old_playing; audio_time=old_time;
}

/* Generic .lfo() modulation is implemented for gain, filter cutoff, source
   frequency and pan. Other targets keep their static value and warn instead
   of silently misrendering. */
static void test_generic_lfo_warns(void) {
    double old_time=audio_time; int old_playing=playing;
    playing=1; audio_set_reference_mode(1); audio_time=0;
    memset(voices,0,sizeof(voices));
    assert(prep_pool_start(0,0));
    event_queue_clear();
    cJSON *metadata=NULL; char error[512];
    JsProgram *program=javascript_compile(
        "note(60).s('sawtooth').resonance(2)"
        ".lfo({control:'resonance',rate:.5,depth:2})",
        120,&metadata,error,sizeof(error));
    assert(program); cJSON_Delete(metadata);
    assert(js_audio_schedule(program,0,0,event_queue_generation()));
    int warned=0;
    for (int i=0;i<console_count_get();i++)
        if (strstr(console_line(i),
            "LFO modulation is implemented for gain, cutoff, source and pan"))
            warned=1;
    assert(warned);
    event_queue_clear();
    javascript_free(program);
    prep_pool_stop();
    audio_set_reference_mode(0); playing=old_playing; audio_time=old_time;
}

/* Generic cutoff LFOs must use the scalar sample-rate DSP filter path and
   actually change the rendered filter response. */
static void test_generic_cutoff_lfo(void) {
    double old_time=audio_time; int old_playing=playing;
    playing=1; audio_set_reference_mode(1); audio_time=0;
    memset(voices,0,sizeof(voices));
    cJSON *dynamic=cJSON_Parse(
        "{\"begin\":0,\"end\":1,\"value\":{\"note\":60,\"s\":\"sawtooth\","
        "\"cutoff\":500,\"lfo\":{\"0\":{\"control\":\"cutoff\",\"rate\":0.5,"
        "\"depth\":2},\"__ids\":{}}}}");
    cJSON *static_hap=cJSON_Parse(
        "{\"begin\":0,\"end\":1,\"value\":{\"note\":60,\"s\":\"sawtooth\","
        "\"cutoff\":500}}");
    assert(dynamic && static_hap);
    AudioEvent a, b;
    assert(js_audio_event(dynamic,0,1,&a));
    assert(js_audio_event(static_hap,0,1,&b));
    assert(a.dsp.lfo_sample_rate && !b.dsp.lfo_sample_rate);
    audio_trigger_event(&a);
    assert(voices[0].dsp.control.lfo_sample_rate);
    assert(!voices[0].soa_pending);
    float dynamic_pcm[256];
    pa_callback(NULL,dynamic_pcm,128,NULL,0,NULL);
    assert(!voices[0].soa_active);
    memset(voices,0,sizeof(voices)); audio_time=0;
    audio_trigger_event(&b);
    float static_pcm[256];
    pa_callback(NULL,static_pcm,128,NULL,0,NULL);
    assert(memcmp(dynamic_pcm,static_pcm,sizeof(dynamic_pcm))!=0);
    cJSON_Delete(dynamic); cJSON_Delete(static_hap);
    audio_set_reference_mode(0); playing=old_playing; audio_time=old_time;
}

/* A generic pan LFO must move energy between channels over time (deployed
   StereoPanner behavior), not just scale it. */
static void test_generic_pan_lfo(void) {
    double old_time=audio_time; int old_playing=playing;
    playing=1; audio_set_reference_mode(1); audio_time=0;
    memset(voices,0,sizeof(voices));
    cJSON *hap=cJSON_Parse(
        "{\"begin\":0,\"end\":2,\"value\":{\"note\":69,\"s\":\"sine\","
        "\"gain\":0.5,\"pan\":0.5,\"lfo\":{\"0\":{\"control\":\"pan\","
        "\"rate\":0.5,\"depth\":1},\"__ids\":{}}}}");
    assert(hap);
    AudioEvent event;
    assert(js_audio_event(hap,0,2,&event));
    assert(event.lfo[0].tgt==8 && !event.lfo_unsupported);
    audio_trigger_event(&event);
    assert(voices[0].vlfo[0].tgt==8);
    float first[512], second[512];
    pa_callback(NULL,first,128,NULL,0,NULL);
    /* Advance one half LFO period (rate .5 Hz): pan swings from -.5 at the
       onset to +.5, mirroring the deployed triangle. */
    for (int i=0;i<344;i++) { float tmp[256]; pa_callback(NULL,tmp,128,NULL,0,NULL); }
    pa_callback(NULL,second,128,NULL,0,NULL);
    double l0=0, r0=0, l1=0, r1=0;
    for (int i=0;i<128;i++) {
        l0+=fabs(first[2*i]); r0+=fabs(first[2*i+1]);
        l1+=fabs(second[2*i]); r1+=fabs(second[2*i+1]);
    }
    /* Onset quantum holds the static center pan (matching the deployed
       first-block hold); the later window is right-heavy. */
    assert(l0/(r0+1e-9)>0.75 && l0/(r0+1e-9)<1.33 && l1/(r1+1e-9)<0.75);
    memset(voices,0,sizeof(voices)); audio_time=0;
    cJSON_Delete(hap);
    audio_set_reference_mode(0); playing=old_playing; audio_time=old_time;
}

/* A generic envelope must not modulate the voice: the deployed envelope
   worklet never starts (connectEnvelope omits begin/end), so native keeps
   env and static renders bit-identical. */
static void test_generic_gain_env(void) {
    double old_time=audio_time; int old_playing=playing;
    playing=1; audio_set_reference_mode(1); audio_time=0;
    memset(voices,0,sizeof(voices));
    cJSON *hap=cJSON_Parse(
        "{\"begin\":0,\"end\":2,\"value\":{\"note\":69,\"s\":\"sine\","
        "\"gain\":0.5,\"env\":{\"0\":{\"control\":\"gain\",\"attack\":0.5,"
        "\"decay\":0.1,\"sustain\":1,\"release\":0.1},\"__ids\":{}}}}");
    assert(hap);
    AudioEvent event;
    assert(js_audio_event(hap,0,2,&event));
    assert(event.env[0].tgt==2 && event.env_unsupported);
    audio_trigger_event(&event);
    float dynamic_pcm[256];
    pa_callback(NULL,dynamic_pcm,128,NULL,0,NULL);
    memset(voices,0,sizeof(voices)); audio_time=0;
    cJSON *static_hap=cJSON_Parse(
        "{\"begin\":0,\"end\":2,\"value\":{\"note\":69,\"s\":\"sine\","
        "\"gain\":0.5}}");
    assert(static_hap);
    AudioEvent static_event;
    assert(js_audio_event(static_hap,0,2,&static_event));
    audio_trigger_event(&static_event);
    float static_pcm[256];
    pa_callback(NULL,static_pcm,128,NULL,0,NULL);
    assert(!memcmp(dynamic_pcm,static_pcm,sizeof(dynamic_pcm)));
    memset(voices,0,sizeof(voices)); audio_time=0;
    cJSON_Delete(hap); cJSON_Delete(static_hap);
    audio_set_reference_mode(0); playing=old_playing; audio_time=old_time;
}

/* A slow missing sample must not discard later stacked sources whose onsets
   cross during its preparation. This observer simulates that delay. */
static void slow_missing_prep(const AudioEvent *event) {
    if (event && !strcmp(event->token,"nosuchsample"))
        audio_time=event->time+.05;
}

/* A missing sample in a stack must not suppress its sibling oscillators.
   This exercises the scheduler's preparation/commit path directly. */
static void test_failed_source_siblings(void) {
    double old_time=audio_time; int old_playing=playing;
    playing=1; audio_set_reference_mode(1); audio_time=0;
    memset(voices,0,sizeof(voices));
    assert(prep_pool_start(0,0));
    event_queue_clear();
    cJSON *metadata=NULL; char error[512];
    JsProgram *program=javascript_compile(
        "s(\"nosuchsample,sine,triangle\").gain(.1)",120,&metadata,error,sizeof(error));
    assert(program); cJSON_Delete(metadata);
    unsigned generation=event_queue_generation();
    js_audio_set_prep_observer(slow_missing_prep);
    assert(js_audio_schedule(program,0,0,generation));
    js_audio_set_prep_observer(NULL);
    /* The failed sample leaves an eviction event; both sibling oscillators
       must still be queued, starting at the delayed preparation time. */
    assert(event_queue_depth()==3);
    event_queue_clear();
    memset(voices,0,sizeof(voices));
    javascript_free(program);
    prep_pool_stop();
    audio_set_reference_mode(0); playing=old_playing; audio_time=old_time;
}

/* The compact render list must stay sorted by voice slot, because slot order
   fixes the floating-point mixing order, and must drop/re-add slots on
   retirement and reuse. */
static void test_active_voice_list(void) {
    int old_playing=playing;
    playing=1; audio_set_reference_mode(1);
    memset(voices,0,sizeof(voices));
    active_voice_reset();
    assert(active_count==0);
    AudioEvent event={.inst=INST_SINE,.smp=-1,.wt=-1,.freq=440,.gain=.01,
        .time=audio_time,.duration=1,.rel=.01,.midi_chan=-1,.midi_note=-1};
    for (int i=0;i<4;i++) audio_trigger_event(&event);
    assert(active_count==4);
    for (unsigned i=0;i<active_count;i++) assert(active_slots[i]==(int)i);
    voices[1].active=0;
    active_voice_compact();
    assert(active_count==3);
    assert(active_slots[0]==0 && active_slots[1]==2 && active_slots[2]==3);
    assert(!active_listed[1]);
    audio_trigger_event(&event);
    assert(active_count==4);
    for (unsigned i=0;i<active_count;i++) assert(active_slots[i]==(int)i);
    for (int i=0;i<VOICE_SLOTS;i++) voices[i].active=0;
    active_voice_compact();
    assert(active_count==0);
    memset(voices,0,sizeof(voices));
    active_voice_reset();
    audio_set_reference_mode(0); playing=old_playing;
}

/* The pinned roller2 helper mixes a missing custom bstab sample with two
   oscillators. Native must reject only that hap and keep both sibling voices
   queued, matching the web's missing-sound behavior and the generic sibling
   test above. */
static void test_switchangel_roller2_missing_sample(void) {
    char root[]="/tmp/tompouce-roller2-XXXXXX", config[512], path[700];
    assert(mkdtemp(root));
    const char *previous=getenv("XDG_CONFIG_HOME");
    char *saved=previous?strdup(previous):NULL;
    assert(!setenv("XDG_CONFIG_HOME",root,1));
    snprintf(config,sizeof(config),"%s/tompouce",root);
    assert(!mkdir(config,0700));
    snprintf(path,sizeof(path),"%s/prebake.strudel",config);
    const char *fixture=
        "register('acidenv',(x,pat)=>pat.lpf(100).lpenv(x*9).lps(.2).lpd(.12).lpq(2));\n"
        "window.roller2=(acidenv=.5)=>s(\"bstab:1:.6,supersaw:1:.6,white:0:.3\")\n"
        " .detune(rand).begin(\"<0 .02>/4\").room(.5).acidenv(acidenv).lpq(0)\n"
        " .postgain(1.2).diode([2.5,.6]).sustain(.45).dec(.2);\n";
    FILE *file=fopen(path,"w"); assert(file); assert(fputs(fixture,file)>=0); assert(!fclose(file));
    double old_time=audio_time; int old_playing=playing;
    playing=1; audio_set_reference_mode(1); audio_time=0;
    memset(voices,0,sizeof(voices));
    assert(prep_pool_start(0,0));
    event_queue_clear();
    cJSON *metadata=NULL; char error[512];
    JsProgram *program=javascript_compile("roller2()",120,&metadata,error,sizeof(error));
    if (!program) fprintf(stderr,"roller2 compile: %s\n",error);
    assert(program); cJSON_Delete(metadata);
    int logged=console_count_get();
    unsigned generation=event_queue_generation();
    assert(js_audio_schedule(program,0,0,generation));
    int warned=0;
    for (int i=logged;i<console_count_get();i++) {
        const char *line=console_line(i);
        if (line && strstr(line,"sample unavailable")) warned=1;
    }
    assert(warned);
    assert(event_queue_depth()==3);
    char queue[4096];
    int qn=scheduler_debug_queue(queue,sizeof(queue),8);
    assert(qn>0 && strstr(queue,"tok=supersaw") && strstr(queue,"tok=white"));
    assert(!strstr(queue,"tok=bstab"));
    event_queue_clear();
    javascript_free(program);
    prep_pool_stop();
    audio_set_reference_mode(0); playing=old_playing; audio_time=old_time;
    if (saved) { setenv("XDG_CONFIG_HOME",saved,1); free(saved); }
    else unsetenv("XDG_CONFIG_HOME");
    unlink(path); rmdir(config); rmdir(root);
    puts("Switch Angel roller2 rejects only bstab and keeps supersaw/white");
}

/* Live phase randomization cannot be sample-locked: the site worklet calls
   Math.random() and native reseeds from time/PID. Both are uniform; this
   measures the native distribution and records the statistical-only ceiling
   in docs/audio-comparison.md. Seeded wt-sample-phaserand gates cover exact
   draw ordering separately. */
static void test_wavetable_phase_random_distribution(void) {
    uint32_t saved=wt_random_state;
    wt_random_state=0x12345678u;
    const int draws=100000;
    double sum=0, squares=0, low=1, high=0;
    for (int i=0;i<draws;i++) {
        double value=wt_random_next();
        assert(value>=0 && value<1);
        sum+=value;
        squares+=value*value;
        if (value<low) low=value;
        if (value>high) high=value;
    }
    double mean=sum/draws;
    double variance=squares/draws-mean*mean;
    fprintf(stderr,"wavetable phase RNG: mean=%.6f variance=%.6f min=%.6f max=%.6f\n",
        mean,variance,low,high);
    assert(mean>.498 && mean<.502);
    assert(variance>.0825 && variance<.0845);
    assert(low<.001 && high>.999);
    wt_random_state=saved;
    puts("wavetable phase RNG is uniform over 100000 live-style draws");
}

static void test_live_scope_attachment(void) {
    const char *code="stack(note(60).s('sine').gain(.1),note(72).s('sine').gain(.2))";
    const char *editor="stack(note(60).s('sine').gain(.1)._scope(),note(72).s('sine').gain(.2))";
    cJSON *metadata=NULL; char error[512];
    JsProgram *program=javascript_compile(code,120,&metadata,error,sizeof(error));
    assert(program); cJSON_Delete(metadata);
    cJSON *haps=javascript_query_audio(program,0,1,.5);
    assert(cJSON_GetArraySize(haps)==2);
    memset(voices,0,sizeof(voices));
    AudioEvent events[2];
    for (int i=0;i<2;i++) {
        assert(js_audio_event(cJSON_GetArrayItem(haps,i),0,2,&events[i]));
        events[i].generation=701; audio_trigger_event(&events[i]);
    }
    float output[512], samples[256]; playing=1;
    pa_callback(NULL,output,256,NULL,0,NULL);
    /* Attach after rendering: the already sounding voices and history have
       their branch tags, so no note/recompile/retrigger is needed. */
    javascript_request_visuals(program,editor); javascript_update_visuals(program,0,0,2);
    unsigned mask=javascript_live_scope_mask(program,editor,(int)(strstr(editor,"_scope")-editor),VISUAL_SCOPE);
    assert(mask && (events[0].scope_mask&mask) && !(events[1].scope_mask&mask));
    assert(audio_pattern_scope_copy(mask,701,samples,256)==256);
    double energy=0;
    for (int i=0;i<256;i++) energy+=samples[i]*samples[i];
    assert(energy>1e-5 && voices[0].active && voices[1].active);
    cJSON_Delete(haps); javascript_free(program); memset(voices,0,sizeof(voices));
}

/* Pattern FX filters/pans are post-source stages. A single FX filter must
   match the source filter path, and an FX pan must match source pan. */
static void render_code(const char *code, float *output) {
    cJSON *metadata=NULL; char error[512];
    JsProgram *program=javascript_compile(code,120,&metadata,error,sizeof(error));
    assert(program); cJSON_Delete(metadata);
    cJSON *haps=javascript_query_audio(program,0,1,.5);
    assert(haps && cJSON_GetArraySize(haps)==1);
    AudioEvent event;
    assert(js_audio_event(cJSON_GetArrayItem(haps,0),0,2,&event));
    memset(voices,0,sizeof(voices)); active_voice_reset(); soa_bank_reset();
    event_queue_clear(); audio_time=0; playing=1;
    audio_trigger_event(&event);
    pa_callback(NULL,output,256,NULL,0,NULL);
    cJSON_Delete(haps); javascript_free(program);
}

static void test_pattern_fx_chain_pcm(void) {
    int old_playing=playing, old_reference=audio_reference_mode();
    float source_filter[512], fx_filter[512], source_pan[512], fx_pan[512];
    audio_set_reference_mode(1);
    render_code("note(69).s('sine').gain(.32).clip(.1).lpf(800)",
        source_filter);
    render_code("note(69).s('sine').gain(.4).clip(.1).FX(lpf(800))",
        fx_filter);
    for (int i=0;i<512;i++)
        assert(fabs(source_filter[i]-fx_filter[i])<1e-12);
    render_code("note(69).s('sine').gain(.32).clip(.1).pan(.5)",
        source_pan);
    render_code("note(69).s('sine').gain(.4).clip(.1).FX(pan(.5))",
        fx_pan);
    for (int i=0;i<512;i++)
        assert(fabs(source_pan[i]-fx_pan[i])<1e-12);
    memset(voices,0,sizeof(voices)); active_voice_reset(); soa_bank_reset();
    event_queue_clear(); playing=old_playing;
    audio_set_reference_mode(old_reference);
}

static void test_fx_room_routing(void) {
    Voice fx={0}, normal={0};
    fx.active=normal.active=1;
    fx.route_cached=normal.route_cached=1;
    fx.route_dsp=normal.route_dsp=1;
    fx.route_room=normal.route_room=.8;
    fx.route_room_size=normal.route_room_size=3;
    fx.route_ll=normal.route_ll=1;
    fx.route_rr=normal.route_rr=1;
    fx.route_side_one=normal.route_side_one=1;
    fx.route_left=normal.route_left=1;
    fx.route_right=normal.route_right=1;
    fx.route_dry=normal.route_dry=0;
    fx.fx_room_present=1;
    VoiceMix mix={0};
    route_voice(&fx,1,-2,&mix);
    assert(mix.fx_room && mix.room_l==0 && mix.room_r==0);
    assert(fabs(mix.fx_room_l-.8)<1e-12);
    assert(fabs(mix.fx_room_ratio+2)<1e-12);
    mix=(VoiceMix){0};
    route_voice(&normal,1,-2,&mix);
    assert(!mix.fx_room && fabs(mix.room_l-.8)<1e-12 &&
        fabs(mix.room_r+1.6)<1e-12);
    Voice panned={0};
    panned.has_pan=1;
    panned.pan=0;
    assert(fabs(fx_room_gain(&panned)-1.423)<1e-12);
    panned.pan=.5;
    assert(fabs(fx_room_gain(&panned)-1.86)<1e-12);
    panned.pan=1;
    assert(fabs(fx_room_gain(&panned)-2.012)<1e-12);
}

static void test_fx_distort_fold(void) {
    Voice voice={0};
    voice.active=1;
    voice.amp=1;
    voice.fx_chain_gain=1;
    voice.route_cached=1;
    voice.route_ll=voice.route_rr=1;
    voice.route_side_one=1;
    voice.route_dry=1;
    voice.route_left=voice.route_right=1;
    voice.fx_stage_count=1;
    voice.fx_stages[0]=(AudioFxStage){AUDIO_FX_DISTORT,1,1,1};
    VoiceMix mix={0};
    route_voice(&voice,.096,.096,&mix);
    double t=expm1(1);
    double n=(1+.5*t)*.096;
    double folded=1-fabs(fmod(n+1,4)-2);
    assert(fabs(mix.left-folded)<1e-12);
    assert(fabs(mix.right-folded)<1e-12);
}

static void test_supersaw_callback(void) {
    memset(voices,0,sizeof(voices)); playing=1;
    AudioEvent event={.inst=INST_SUPERSAW,.smp=-1,.freq=440,.gain=.2,
        .time=audio_time,.duration=1,.att=.001,.dec=.05,.sus=.6,.rel=.01};
    dsp_params_init(&event.dsp); event.dsp.enabled=1;
    audio_trigger_event(&event); audio_trigger_event(&event);
    assert(voices[0].dsp.supersaw_voices==5 && voices[1].dsp.supersaw_voices==5);
    for (unsigned i=0;i<5;i++)
        assert(voices[0].dsp.saw_phase[i]!=voices[1].dsp.saw_phase[i]);
    double phase=voices[0].dsp.saw_phase[0],energy=0,stereo=0;
    float pcm[512]; pa_callback(NULL,pcm,256,NULL,0,NULL);
    for (int i=0;i<256;i++) {
        assert(isfinite(pcm[i*2]) && isfinite(pcm[i*2+1]));
        energy+=pcm[i*2]*pcm[i*2]+pcm[i*2+1]*pcm[i*2+1];
        stereo+=fabs(pcm[i*2]-pcm[i*2+1]);
    }
    assert(energy>0 && stereo>0 && voices[0].dsp.saw_phase[0]!=phase);
    assert(voices[0].soa_active && voices[0].soa_lane>=0);
    assert(soa_bank.lanes[voices[0].soa_lane].kind==SOA_KIND_SUPERSAW);
    memset(voices,0,sizeof(voices));
}

/* Production admission must render the same PCM as the scalar supersaw
   path from the same trigger state. */
static double bench_hh_callback(int voices_requested) {
    memset(voices,0,sizeof(voices)); active_voice_reset(); soa_bank_reset();
    event_queue_clear(); audio_time=0; next_voice_onset=HUGE_VAL; voice_onset_serial=0;
    AudioEvent hh={.inst=INST_HH,.smp=-1,.wt=-1,.freq=0,.gain=.1,
        .time=0,.duration=1,.rel=.01,.midi_chan=-1,.midi_note=-1};
    for (int i=0;i<voices_requested;i++) audio_trigger_event(&hh);
    float out[512];
    struct timespec a,b; assert(!clock_gettime(CLOCK_MONOTONIC,&a));
    pa_callback(NULL,out,256,NULL,0,NULL);
    assert(!clock_gettime(CLOCK_MONOTONIC,&b));
    memset(voices,0,sizeof(voices)); active_voice_reset(); soa_bank_reset();
    event_queue_clear();
    return (b.tv_sec-a.tv_sec)*1e6+(b.tv_nsec-a.tv_nsec)/1e3;
}

/* Procedural HH keeps a shared rand()/high-pass stream in slot order. A lane
   cannot preserve that stream without changing PCM, so measure its actual
   share before considering a separate reference design. */
static void bench_procedural_hh(void) {
    int old=playing; playing=1; audio_set_reference_mode(1);
    double one=bench_hh_callback(1), many=bench_hh_callback(64);
    printf("HH scalar callback 256 frames: 1 voice %.1f us, 64 voices %.1f us (%.2f us/voice)\n",
        one,many,(many-one)/63);
    playing=old; audio_set_reference_mode(0);
}

/* Scalar stereo samples that the SoA bank does not admit use the persistent
   two-lane filter path; it must match two scalar dsp_process calls exactly. */
static void test_stereo_filter_playback(void) {
    float stereo[128];
    for (int i=0;i<128;i++) stereo[i]=(float)((i*29%101)-50)/97.0f;
    DspParams params; dsp_params_init(&params); params.enabled=1;
    params.p[DSP_LPF]=2400; params.p[DSP_LPQ]=1.2;
    float scalar[8], persistent[8];
    double old_time=audio_time; int old_playing=playing;
    int old_reference=audio_reference_mode();
    playing=1; audio_set_reference_mode(1);
    for (int mode=0;mode<2;mode++) {
        memset(voices,0,sizeof(voices)); active_voice_reset(); soa_bank_reset();
        Voice *v=&voices[0];
        v->active=1; active_voice_add(0);
        v->smpbuf=stereo; v->smplen=64; v->smp_channels=2;
        v->smpstep=1; v->smp_end=1; v->smp_gate=1;
        v->gate_end=v->note_off_time=10; v->start_time=0;
        v->amp=.1; v->vsus=1; v->vrel=.01;
        v->delay_send=0; v->route_delay=0; v->route_room=0; v->route_dsp=0;
        dsp_init(&v->dsp,&params); dsp_init(&v->dsp_right,&params);
        v->stereo_filter_type=-1;
        if (mode) {
            assert(prepare_stereo_filter(v));
            assert(v->stereo_filter_type==0);
        }
        audio_time=0; event_queue_clear();
        pa_callback(NULL,mode?persistent:scalar,4,NULL,0,NULL);
    }
    assert(!memcmp(scalar,persistent,sizeof(scalar)));
    memset(voices,0,sizeof(voices)); active_voice_reset(); soa_bank_reset();
    audio_time=old_time; playing=old_playing;
    audio_set_reference_mode(old_reference);
}

/* Callback-level cost of the persistent stereo filter on a looping sample
   voice (the SoA bank does not admit loops). Both arms render the same state. */
static void bench_stereo_filter_callback(void) {
    enum { FRAMES=4096, BLOCKS=256 };
    float *stereo=malloc(FRAMES*2*sizeof(float));
    assert(stereo);
    for (int i=0;i<FRAMES*2;i++) stereo[i]=(float)((i*29%101)-50)/97.0f;
    DspParams params; dsp_params_init(&params); params.enabled=1;
    params.p[DSP_LPF]=2400; params.p[DSP_LPQ]=1.2; params.p[DSP_LOOP]=1;
    int old_playing=playing, old_reference=audio_reference_mode();
    double old_time=audio_time;
    float pcm[512];
    double elapsed[2]={0,0};
    for (int mode=0;mode<2;mode++) {
        memset(voices,0,sizeof(voices)); active_voice_reset(); soa_bank_reset();
        Voice *v=&voices[0];
        v->active=1; active_voice_add(0);
        v->smpbuf=stereo; v->smplen=FRAMES; v->smp_channels=2;
        v->smpstep=1; v->smp_end=1; v->smp_gate=1;
        v->gate_end=v->note_off_time=1000; v->start_time=0;
        v->amp=.1; v->vsus=1; v->vrel=.01;
        v->delay_send=0; v->route_delay=0; v->route_room=0; v->route_dsp=0;
        dsp_init(&v->dsp,&params); dsp_init(&v->dsp_right,&params);
        v->stereo_filter_type=-1;
        if (mode) assert(prepare_stereo_filter(v));
        playing=1; audio_set_reference_mode(1); audio_time=0;
        struct timespec a,b;
        assert(!clock_gettime(CLOCK_MONOTONIC,&a));
        for (int block=0;block<BLOCKS;block++)
            pa_callback(NULL,pcm,256,NULL,0,NULL);
        assert(!clock_gettime(CLOCK_MONOTONIC,&b));
        elapsed[mode]=(b.tv_sec-a.tv_sec)*1e3+(b.tv_nsec-a.tv_nsec)/1e6;
    }
    printf("stereo filter callback: scalar %.3f ms, persistent %.3f ms (%.2fx)\n",
        elapsed[0],elapsed[1],elapsed[1]>0?elapsed[0]/elapsed[1]:0);
    memset(voices,0,sizeof(voices)); active_voice_reset(); soa_bank_reset();
    audio_time=old_time; playing=old_playing;
    audio_set_reference_mode(old_reference); free(stereo);
}

static void test_supersaw_soa_equivalence(void) {
    double old_time=audio_time; int old_playing=playing;
    playing=1; audio_set_reference_mode(1);
    AudioEvent event={.inst=INST_SUPERSAW,.smp=-1,.freq=440,.gain=.2,
        .time=0,.duration=1,.att=.001,.dec=.05,.sus=.6,.rel=.01,
        .midi_chan=-1,.midi_note=-1};
    dsp_params_init(&event.dsp); event.dsp.enabled=1;
    float scalar[8], lane[8];
    memset(voices,0,sizeof(voices)); active_voice_reset(); soa_bank_reset();
    event_queue_clear(); audio_time=0; next_voice_onset=HUGE_VAL; voice_onset_serial=0;
    audio_trigger_event(&event);
    voices[0].soa_pending=0; /* scalar reference */
    pa_callback(NULL,scalar,4,NULL,0,NULL);
    memset(voices,0,sizeof(voices)); active_voice_reset(); soa_bank_reset();
    event_queue_clear(); audio_time=0; next_voice_onset=HUGE_VAL; voice_onset_serial=0;
    event.time=0; audio_trigger_event(&event);
    assert(voices[0].soa_pending);
    pa_callback(NULL,lane,4,NULL,0,NULL);
    assert(voices[0].soa_active);
    for (int i=0;i<8;i++) assert(fabs(scalar[i]-lane[i])<1e-12);
    memset(voices,0,sizeof(voices)); active_voice_reset(); soa_bank_reset();
    event_queue_clear(); audio_time=old_time; playing=old_playing; audio_set_reference_mode(0);
}

static void test_supersaw_quantum_start(void) {
    const double onsets[]={0,50.25/SR,128/SR,.274};
    for (unsigned i=0;i<sizeof(onsets)/sizeof(*onsets);i++) {
        memset(voices,0,sizeof(voices)); audio_time=0; playing=1;
        AudioEvent event={.inst=INST_SUPERSAW,.smp=-1,.freq=440,.gain=.2,
            .time=onsets[i],.duration=1,.att=.001,.dec=.05,.sus=.6,.rel=.01};
        dsp_params_init(&event.dsp); event.dsp.enabled=1; event.dsp.p[DSP_PENV]=7;
        audio_trigger_event(&event);
        assert(!voices[0].dsp.pitch_initial_frames);
        double phase=voices[0].dsp.saw_phase[0],energy=0;
        unsigned first=i==3?12160:128;
        assert(fabs(voices[0].source_begin-first/SR)<1e-12);
        float out[2];
        for (unsigned frame=0;frame<first+8;frame++) {
            pa_callback(NULL,out,1,NULL,0,NULL);
            if (frame<first) {
                assert(out[0]==0 && out[1]==0);
                assert(voices[0].dsp.saw_phase[0]==phase);
            } else energy+=out[0]*out[0]+out[1]*out[1];
        }
        assert(energy>1e-8);
    }
    memset(voices,0,sizeof(voices));
}

static void test_pulse_quantum_boundary(void) {
    double previous=audio_time;audio_time=0;playing=1;
    memset(voices,0,sizeof(voices));
    AudioEvent event={.inst=INST_PULSE,.smp=-1,.freq=440,.gain=.2,
        .time=128.0/SR,.duration=1,.att=.001,.dec=.05,.sus=.6,.rel=.01};
    dsp_params_init(&event.dsp);event.dsp.enabled=1;
    audio_trigger_event(&event);
    /* Float32 begin rounds down, so the web starts at frame 128, not 256. */
    assert(voices[0].source_begin==128.0/SR);
    float pcm[514];pa_callback(NULL,pcm,257,NULL,0,NULL);
    assert(voices[0].dsp.pulse_frame==129);
    memset(voices,0,sizeof(voices));audio_time=previous;
}

static void test_shared_delay_eviction_and_reparam(void) {
    int orbit=N_ORBITS-2;
    double old_dt=orbit_dt[orbit], old_fb=orbit_fb[orbit];
    int old_created=web_delay_created[orbit];
    double old_time=audio_time; int old_playing=playing;
    playing=1; audio_set_reference_mode(1);
    memset(voices,0,sizeof(voices));
    next_voice_onset=HUGE_VAL; voice_onset_serial=0;
    orbits_ensure();
    assert(ensure_orbit_delay(orbit));
    memset(dbuf[orbit],0,ORBIT_DELAY*2*sizeof(float));
    memset(feedback_quantum[orbit],0,256*sizeof(float));
    orbit_dt[orbit]=64/SR; orbit_fb[orbit]=.5; web_delay_created[orbit]=1;

    AudioEvent source={.time=0,.inst=INST_SINE,.freq=440,.smp=-1,.wt=-1,
        .gain=.1,.duration=100/SR,.rel=0,.orbit=orbit,.delay_send=1,
        .midi_chan=-1,.midi_note=-1,.delay_prepared=1,.room_prepared=1};
    audio_trigger_event(&source);
    float block[256];
    pa_callback(NULL,block,100,NULL,0,NULL);

    /* Fill the current-source pool, forcing the delayed source into a retiring
       graph. Its already-connected shared delay line must remain intact. */
    AudioEvent silent={.time=audio_time,.inst=INST_SINE,.freq=100,.smp=-1,.wt=-1,
        .gain=0,.duration=1,.rel=0,.midi_chan=-1,.midi_note=-1};
    for (int i=0;i<MAX_VOICES;i++) audio_trigger_event(&silent);
    int retired=0;
    for (int i=MAX_VOICES;i<VOICE_SLOTS;i++)
        if (voices[i].active && voices[i].freq==440) retired=1;
    assert(retired);
    /* A new source reconnects to the same orbit while the shared tail is active.
       Its dry onset ends before the measured tail window, so the retained delay
       samples must still be present. */
    AudioEvent reconnect={.time=audio_time,.inst=INST_SINE,.freq=220,.smp=-1,.wt=-1,
        .gain=.1,.duration=10/SR,.rel=0,.orbit=orbit,.delay_send=0,
        .midi_chan=-1,.midi_note=-1,.delay_prepared=1,.room_prepared=1};
    audio_trigger_event(&reconnect);
    pa_callback(NULL,block,128,NULL,0,NULL);
    double tail=0;
    for (int i=64;i<128;i++) tail+=fabs(block[i*2])+fabs(block[i*2+1]);
    assert(tail>1e-6);

    /* Reconnect the shared delay to a new read time while the old line is
       live. Existing samples remain and are read at the new offset. */
    AudioEvent reparam={.time=audio_time,.orbit=orbit,.delay_send=1,
        .delay_time=128/SR,.delay_fb=.5,.midi_chan=-1,.midi_note=-1};
    dsp_params_init(&reparam.dsp); reparam.dsp.enabled=1;
    assert(audio_prepare_delay(&reparam));
    pa_callback(NULL,block,1,NULL,0,NULL);
    assert(fabs(block[0])+fabs(block[1])>1e-7);

    memset(dbuf[orbit],0,ORBIT_DELAY*2*sizeof(float));
    memset(feedback_quantum[orbit],0,256*sizeof(float));
    orbit_dt[orbit]=old_dt; orbit_fb[orbit]=old_fb;
    web_delay_created[orbit]=old_created;
    memset(voices,0,sizeof(voices)); next_voice_onset=HUGE_VAL;
    audio_time=old_time; playing=old_playing;
    audio_set_reference_mode(0);
}

static void test_shared_room_eviction(void) {
    int orbit=N_ORBITS-3;
    double old_time=audio_time; int old_playing=playing;
    playing=1; audio_set_reference_mode(1);
    memset(voices,0,sizeof(voices));
    next_voice_onset=HUGE_VAL; voice_onset_serial=0;
    orbits_ensure();
    assert(ensure_orbit_reverb(orbit));
    memset(comb[orbit],0,2*4*16384*sizeof(float));
    memset(allpass[orbit],0,2*2*2048*sizeof(float));

    AudioEvent source={.time=0,.inst=INST_SINE,.freq=440,.smp=-1,.wt=-1,
        .gain=.1,.duration=100/SR,.rel=0,.orbit=orbit,
        .midi_chan=-1,.midi_note=-1,.room_prepared=1,.delay_prepared=1};
    dsp_params_init(&source.dsp); source.dsp.enabled=1;
    source.dsp.p[DSP_ROOM]=1; source.dsp.p[DSP_SIZE]=.5; source.dsp.p[DSP_DRY]=0;
    audio_trigger_event(&source);
    float block[8192];
    pa_callback(NULL,block,100,NULL,0,NULL);

    /* Evict the source into a retiring graph, then render long enough for the
       shared fallback reverb's delays to flush. */
    AudioEvent silent={.time=audio_time,.inst=INST_SINE,.freq=100,.smp=-1,.wt=-1,
        .gain=0,.duration=1,.rel=0,.midi_chan=-1,.midi_note=-1};
    for (int i=0;i<MAX_VOICES;i++) audio_trigger_event(&silent);
    int retired=0;
    for (int i=MAX_VOICES;i<VOICE_SLOTS;i++)
        if (voices[i].active && voices[i].freq==440) retired=1;
    assert(retired);
    pa_callback(NULL,block,4096,NULL,0,NULL);
    double tail=0;
    for (int i=1200;i<4096;i++) tail+=fabs(block[i*2])+fabs(block[i*2+1]);
    assert(tail>1e-8);

    memset(comb[orbit],0,2*4*16384*sizeof(float));
    memset(allpass[orbit],0,2*2*2048*sizeof(float));
    memset(voices,0,sizeof(voices)); next_voice_onset=HUGE_VAL;
    audio_time=old_time; playing=old_playing;
    audio_set_reference_mode(0);
}

static void test_shared_fx_routing_and_live_edit(void) {
    int orbit=9;
    double old_time=audio_time; int old_playing=playing;
    playing=1; audio_set_reference_mode(1);
    memset(voices,0,sizeof(voices));
    next_voice_onset=HUGE_VAL; voice_onset_serial=0;
    orbits_ensure();
    double old_dt=orbit_dt[orbit], old_fb=orbit_fb[orbit];
    int old_created=web_delay_created[orbit];
    assert(ensure_orbit_delay(orbit));
    memset(dbuf[orbit],0,ORBIT_DELAY*2*sizeof(float));
    memset(feedback_quantum[orbit],0,256*sizeof(float));
    web_delay_created[orbit]=1;
    orbit_dt[orbit]=.001; orbit_fb[orbit]=.5;

    cJSON *delay=cJSON_Parse(
        "{\"begin\":0,\"end\":0.5,\"value\":{\"note\":60,\"s\":\"sine\","
        "\"gain\":0.1,\"orbit\":9,\"_fxSteps\":[{\"steps\":["
        "{\"op\":\"delay\",\"args\":[1]},"
        "{\"op\":\"delaytime\",\"args\":[0.001]},"
        "{\"op\":\"delayfeedback\",\"args\":[0.5]}]}]}}");
    assert(delay);
    AudioEvent first;
    assert(js_audio_event(delay,0,1,&first));
    assert(fabs(first.delay_send-1)<1e-9 && fabs(first.delay_time-.001)<1e-9);
    audio_trigger_event(&first);
    float block[512];
    pa_callback(NULL,block,128,NULL,0,NULL);

    /* A live edit selects the room route on the same orbit. The existing
       shared delay line must remain connected while the new event starts. */
    cJSON *room=cJSON_Parse(
        "{\"begin\":0,\"end\":0.5,\"value\":{\"note\":64,\"s\":\"sine\","
        "\"gain\":0.1,\"orbit\":9,\"_fxSteps\":[{\"steps\":["
        "{\"op\":\"room\",\"args\":[0.4]},{\"op\":\"size\",\"args\":[3]},"
        "{\"op\":\"lpf\",\"args\":[500]},{\"op\":\"pan\",\"args\":[0.2]},"
        "{\"op\":\"coarse\",\"args\":[3]},{\"op\":\"shape\",\"args\":[0.4]},"
        "{\"op\":\"distort\",\"args\":[2]},{\"op\":\"chorus\",\"args\":[0.3]},"
        "{\"op\":\"fmi\",\"args\":[2]},{\"op\":\"irspeed\",\"args\":[0.5]},"
        "{\"op\":\"irbegin\",\"args\":[0.2]},{\"op\":\"ir\",\"args\":[\"impulse\"]}]}]}}");
    assert(room);
    AudioEvent second;
    assert(js_audio_event(room,0,1,&second));
    assert(fabs(second.dsp.p[DSP_ROOM]-.4)<1e-9);
    assert(fabs(second.dsp.p[DSP_LPF]-500)<1e-9);
    assert(second.has_pan && fabs(second.pan+.6)<1e-9);
    assert(fabs(second.dsp.p[DSP_COARSE]-3)<1e-9);
    assert(fabs(second.dsp.p[DSP_SHAPE]-.4)<1e-9);
    assert(fabs(second.dsp.p[DSP_DISTORT]-2)<1e-9);
    assert(fabs(second.dsp.p[DSP_CHORUS]-.3)<1e-9);
    assert(fabs(second.dsp.p[DSP_FMBASE]-2)<1e-9);
    assert(fabs(second.room_speed-.5)<1e-9);
    assert(fabs(second.room_begin-.2)<1e-9);
    assert(!strcmp(second.fx_ir,"impulse"));
    audio_trigger_event(&second);
    pa_callback(NULL,block,256,NULL,0,NULL);
    double energy=0;
    for(int i=0;i<256;i++) energy+=fabs(block[i*2])+fabs(block[i*2+1]);
    assert(energy>1e-6);

    memset(dbuf[orbit],0,ORBIT_DELAY*2*sizeof(float));
    memset(feedback_quantum[orbit],0,256*sizeof(float));
    if (comb[orbit]) memset(comb[orbit],0,2*4*16384*sizeof(float));
    if (allpass[orbit]) memset(allpass[orbit],0,2*2*2048*sizeof(float));
    orbit_dt[orbit]=old_dt; orbit_fb[orbit]=old_fb;
    web_delay_created[orbit]=old_created;
    memset(voices,0,sizeof(voices)); next_voice_onset=HUGE_VAL;
    audio_time=old_time; playing=old_playing;
    audio_set_reference_mode(0);
    cJSON_Delete(delay); cJSON_Delete(room);
}

static void test_output_volume(void) {
    assert(audio_get_output_gain()==1);
    assert(!audio_set_output_gain(NAN) && !audio_set_output_gain(INFINITY));
    assert(!audio_set_output_gain(-.1) && !audio_set_output_gain(1.1));
    assert(audio_get_output_gain()==1);
    playing=1; audio_set_reference_mode(1);
    AudioEvent event={.inst=INST_TRI,.smp=sample_fixture(),.smp_speed=1,.gain=.1,
        .sus=1,.duration=1,.rel=.1,.smp_gate=1};
    dsp_params_init(&event.dsp); event.dsp.enabled=1;
    event.dsp.p[DSP_LOOP]=1;
    Voice *voice=start_sample(&event);
    float unity[256],half[256],muted[256],restored[256];
    pa_callback(NULL,unity,128,NULL,0,NULL);
    assert(voice->active);
    double age=voice->age;
    assert(audio_set_output_gain(.5));
    pa_callback(NULL,half,128,NULL,0,NULL);
    assert(voice->active && voice->age>age);
    assert(audio_set_output_gain(0));
    pa_callback(NULL,muted,128,NULL,0,NULL);
    assert(voice->active);
    assert(audio_set_output_gain(1));
    pa_callback(NULL,restored,128,NULL,0,NULL);
    double energy=0;
    for(int i=0;i<256;i++) {
        assert(fabs(half[i]-unity[i]*.5)<1e-7);
        assert(muted[i]==0);
        assert(fabs(restored[i]-unity[i])<1e-7);
        energy+=unity[i]*unity[i];
    }
    assert(energy>1e-6);
    /* A real delay tail continues through mute after the source gate ends. */
    int orbit=N_ORBITS-1;
    double old_dt=orbit_dt[orbit],old_fb=orbit_fb[orbit];
    event.orbit=orbit; event.time=audio_time; event.duration=128/SR; event.rel=0;
    event.delay_send=1; event.delay_prepared=1;
    orbit_dt[orbit]=128/SR; orbit_fb[orbit]=.5;
    start_sample(&event);
    pa_callback(NULL,unity,128,NULL,0,NULL);
    assert(audio_set_output_gain(0));
    pa_callback(NULL,muted,128,NULL,0,NULL);
    for(int i=0;i<256;i++)assert(muted[i]==0);
    assert(audio_set_output_gain(1));
    pa_callback(NULL,restored,128,NULL,0,NULL);
    energy=0;for(int i=0;i<256;i++)energy+=restored[i]*restored[i];
    assert(energy>1e-6);
    memset(dbuf[orbit],0,ORBIT_DELAY*2*sizeof(float));
    memset(feedback_quantum[orbit],0,256*sizeof(float));
    orbit_dt[orbit]=old_dt;orbit_fb[orbit]=old_fb;
    memset(voices,0,sizeof(voices));
    playing=0;audio_set_reference_mode(0);audio_time=0;
}

/* Voices tagged with a named target feed only that target's buffer; the
   default bus stays silent so its established PCM is untouched. */
static void test_named_output_routing(void) {
    memset(voices,0,sizeof(voices));
    AudioEvent event={.inst=INST_SINE,.smp=-1,.freq=220,.gain=.02,
        .duration=2,.att=0,.rel=.01,.sus=1,.out_target=1};
    playing=1;
    audio_trigger_event(&event);
    float target[512*2]={0}, default_out[512*2]={0};
    out_target_buffers[1]=target;
    pa_callback(NULL,default_out,512,NULL,0,NULL);
    out_target_buffers[1]=NULL;
    double target_energy=0, default_energy=0;
    for (int i=0;i<512*2;i++) {
        target_energy+=fabs(target[i]); default_energy+=fabs(default_out[i]);
    }
    assert(target_energy>0);
    assert(default_energy==0);
    memset(voices,0,sizeof(voices));
}

int main(void) {
    test_named_output_routing();
    /* Never-used orbits must stay bit-exactly silent: the reverb and delay
       gates skip work only when their state is provably zero. Runs first so
       no later test has dirtied a delay line or reverb network. */
    {
        float idle[512];
        for (int block = 0; block < 8; block++) {
            pa_callback(NULL,idle,256,NULL,0,NULL);
            for (int i = 0; i < 512; i++) assert(idle[i] == 0.0f);
        }
        audio_time = 0; /* restore the timing baseline later tests assume */
    }
    test_output_volume();
    test_shared_delay_eviction_and_reparam();
    test_shared_room_eviction();
    test_shared_fx_routing_and_live_edit();
    /* Exercise the actual callback with opposite-polarity stereo channels.
       Averaging to mono would cancel this signal. No audio device is needed. */
    float stereo[] = {.25f,-.5f,.125f,.375f};
    Voice *v = &voices[0];
    v->stereo_filter_type = -1;
    v->active = 1; v->smpbuf = stereo; v->smplen = 2;
    v->smp_channels = 2; v->smpstep = 1; v->smp_end = 1;
    v->smp_gate = 1; v->gate_end = v->note_off_time = 1;
    v->amp = .1; v->vsus = 1; v->vrel = .01;
    DspParams params;
    dsp_params_init(&params); params.enabled = 1;
    dsp_init(&v->dsp,&params); dsp_init(&v->dsp_right,&params);
    playing = 1;
    audio_set_reference_mode(1);
    float output[1024];
    pa_callback(NULL,output,2,NULL,0,NULL);
    assert(output[0] > .02 && output[1] < -.04);
    assert(fabs(output[0]-.025) < 1e-8 && fabs(output[1]+.05) < 1e-8);

    Voice pan_voice = {0};
    dsp_init(&pan_voice.dsp,&params);
    VoiceMix mix = {0};
    route_voice(&pan_voice,1,1,&mix);
    assert(mix.left==1 && mix.right==1); /* No pan means unity upmix. */
    pan_voice.has_pan=1; pan_voice.route_cached=0; memset(&mix,0,sizeof(mix));
    route_voice(&pan_voice,1,1,&mix);
    assert(fabs(mix.left-sqrt(.5)) < 1e-9 && fabs(mix.right-sqrt(.5)) < 1e-9);
    pan_voice.smp_channels = 2; pan_voice.route_cached=0;
    memset(&mix,0,sizeof(mix));
    route_voice(&pan_voice,1,.25,&mix);
    assert(fabs(mix.left-1) < 1e-9 && fabs(mix.right-.25) < 1e-9);
    pan_voice.pan = 1; pan_voice.route_cached=0; memset(&mix,0,sizeof(mix));
    route_voice(&pan_voice,1,.25,&mix);
    assert(fabs(mix.left) < 1e-9 && fabs(mix.right-1.25) < 1e-9);

    memset(voices,0,sizeof(voices));
    cJSON *hap = cJSON_Parse("{\"begin\":0,\"end\":1,\"value\":{\"note\":60,\"s\":\"sine\",\"attack\":0.1,\"sustain\":1}}");
    assert(hap);
    AudioEvent event;
    assert(js_audio_event(hap,0,1,&event));
    audio_trigger_event(&event);
    pa_callback(NULL,output,512,NULL,0,NULL);
    assert(fabs(voices[0].env-511.0/SR/.1) < 1e-9);
    assert(voices[0].smp_channels == 0);
    cJSON_Delete(hap);

    memset(voices,0,sizeof(voices));
    orbits_ensure();
    orbit_dt[4] = 32 / SR; orbit_fb[4] = 0;
    assert(ensure_orbit_delay(4)); /* white-box voice setup bypasses triggering */
    float impulse[2] = {1,1};
    voices[0].active = 1; active_voice_add(0); voices[0].orbit = 4;
    voices[0].smpbuf = impulse; voices[0].smplen = 1; voices[0].smp_channels = 1;
    voices[0].smpstep = 1; voices[0].smp_end = 1; voices[0].amp = .5;
    voices[0].delay_send = 1; voices[0].note_off_time = 10; voices[0].gate_end = 10;
    float delayed[80];
    pa_callback(NULL,delayed,40,NULL,0,NULL);
    assert(fabs(delayed[0]-.5) < 1e-5 && fabs(delayed[2]) < 1e-5);
    assert(fabs(delayed[32*2]-.5) < 1e-4);

    memset(voices,0,sizeof(voices));
    DspParams held; dsp_params_init(&held); held.enabled = 1;
    voices[1].active = 1; active_voice_add(1); voices[1].orbit = 2; voices[1].inst = INST_SINE;
    voices[1].phase = M_PI/2; voices[1].amp = 1; voices[1].vsus = 1;
    voices[1].note_off_time = 10; voices[1].gate_end = 10; voices[1].age = 1;
    dsp_init(&voices[1].dsp,&held);
    float before[2], after[2];
    pa_callback(NULL,before,1,NULL,0,NULL);
    assert(fabs(before[0]) > .1);
    AudioEvent duck = {0};
    duck.inst = INST_SINE; duck.gain = .01; duck.duration = 1; duck.orbit = 0;
    duck.duck_mask = 1u << 2; duck.duck_depth = 1; duck.duck_attack = 1;
    audio_trigger_event(&duck);
    pa_callback(NULL,after,1,NULL,0,NULL);
    assert(fabs(after[0]) < fabs(before[0]) * .05);

    cJSON *bus = cJSON_Parse("{\"begin\":0,\"end\":1,\"value\":{\"note\":60,\"s\":\"sawtooth\",\"orbit\":3,\"delay\":0.4,\"duckorbit\":\"1:5\",\"duckattack\":0.25,\"duckdepth\":0.5}}");
    assert(bus);
    AudioEvent routed;
    assert(js_audio_event(bus,0,2,&routed));
    assert(routed.orbit == 3 && routed.delay_send == .4);
    assert(routed.duck_mask == 0); /* Web target keys do not expand colon lists. */
    assert(fabs(routed.duck_attack-.25) < 1e-9 && fabs(routed.duck_depth-.5) < 1e-9);
    cJSON *array_bus=cJSON_Parse("{\"begin\":0,\"end\":1,\"value\":{\"note\":60,\"duckorbit\":[1,2],\"duckonset\":[0.02,0.04],\"duckattack\":[0.1,0.2],\"duckdepth\":[0.25,1]}}");
    AudioEvent array_event; assert(array_bus && js_audio_event(array_bus,0,2,&array_event));
    assert(array_event.duck_values && array_event.duck_mask==((1u<<1)|(1u<<2)));
    assert(array_event.ducks[1].onset==.02 && array_event.ducks[1].attack==.1 && array_event.ducks[1].depth==.25);
    assert(array_event.ducks[2].onset==.04 && array_event.ducks[2].attack==.2 && array_event.ducks[2].depth==1);
    cJSON_Delete(array_bus);
    cJSON *clip_zero=cJSON_Parse("{\"begin\":0,\"end\":1,\"value\":{\"note\":60,\"clip\":0,\"gain\":0,\"duckorbit\":2}}");
    assert(clip_zero && js_audio_event(clip_zero,0,2,&array_event));
    assert(array_event.duration==2 && array_event.gain==0 && array_event.duck_mask==(1u<<2));
    cJSON_Delete(clip_zero);
    cJSON_Delete(bus);

    memset(voices, 0, sizeof(voices));
    AudioEvent dbg = {0};
    dbg.inst = INST_SAW;
    dbg.smp = -1;
    dbg.freq = 220;
    dbg.gain = 0.5;
    dbg.duration = 0.4;
    dbg.dsp.enabled = 1;
    dsp_params_init(&dbg.dsp);
    dbg.dsp.enabled = 1;
    dbg.dsp.p[DSP_LPF] = 800;
    dbg.dsp.p[DSP_LPQ] = 20;
    dbg.dsp.p[DSP_BEGIN] = 0.25;
    dbg.dsp.p[DSP_END] = 0.5;
    dbg.dsp.p[DSP_FTYPE] = 0;
    snprintf(dbg.token, sizeof(dbg.token), "dbgsaw");
    audio_trigger_event(&dbg);
    char snap[8192];
    int sn = audio_debug_snapshot(snap, sizeof(snap), 4);
    assert(sn > 0 && (size_t)sn < sizeof(snap));
    assert(strstr(snap, "pool="));
    assert(strstr(snap, "fx drive="));
    assert(strstr(snap, "cutoff=1.000"));
    assert(strstr(snap, "tok=dbgsaw"));
    assert(strstr(snap, "begin=0.250"));
    assert(strstr(snap, "end=0.500"));
    assert(strstr(snap, "cut=800.000"));
    assert(strstr(snap, "q=20.000"));
    assert(strstr(snap, "ft=0"));
    assert(audio_debug_snapshot(NULL, 8, 1) < 0);
    assert(audio_debug_snapshot(snap, sizeof(snap), 0) > 0);

    AudioEvent queued = {0};
    queued.inst = INST_SQUARE;
    queued.smp = -1;
    queued.time = 99;
    queued.duration = 0.1;
    queued.freq = 440;
    queued.gain = 0.2;
    queued.dsp.enabled = 1;
    dsp_params_init(&queued.dsp);
    queued.dsp.enabled = 1;
    queued.dsp.p[DSP_LPF] = 1500;
    snprintf(queued.token, sizeof(queued.token), "qnote");
    event_queue_clear();
    event_queue_push(&queued);
    char qbuf[2048];
    int qn = scheduler_debug_queue(qbuf, sizeof(qbuf), 4);
    assert(qn > 0);
    assert(strstr(qbuf, "queue 1 showing 1"));
    assert(strstr(qbuf, "tok=qnote"));
    assert(strstr(qbuf, "cut=1500.000"));
    assert(strstr(qbuf, "when=99.000"));
    assert(scheduler_debug_queue(NULL, 8, 1) < 0);
    event_queue_clear();

    test_sample_windows();
    test_fx_graph_gain();
    test_sample_connection_lifetime();
    test_sample_pitch_quantum();
    test_convolution();
    test_stretch();
    test_pattern_scopes();
    test_analysers();
    test_live_scope_attachment();
    test_pattern_fx_chain_pcm();
    test_fx_room_routing();
    test_fx_distort_fold();
    test_supersaw_callback();
    test_stereo_filter_playback();
    bench_stereo_filter_callback();
    test_supersaw_quantum_start();
    test_pulse_quantum_boundary();
    test_supersaw_soa_equivalence();
    bench_procedural_hh();
    test_failed_source_siblings();
    test_switchangel_roller2_missing_sample();
    test_wavetable_phase_random_distribution();
    test_analyser_controls_do_not_warn();
    test_generic_lfo_warns();
    test_generic_cutoff_lfo();
    test_generic_pan_lfo();
    test_generic_gain_env();
    test_generic_gain_env();
    test_active_voice_list();

    puts("native callback, stereo/pan/ADSR, stretch and partitioned convolution ok");
    return 0;
}
