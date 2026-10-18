#include <assert.h>
#include <string.h>
#include "../src/audio.c"
#include "js_audio.h"
#include "scheduler.h"
#include <unistd.h>

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
    audio_trigger_event(event);
    assert(voices[0].active);
    return &voices[0];
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
    convolution_kernel_free(kernel); free(state);
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
    for (int reverse = 0; reverse < 2; reverse++) {
        memset(&event,0,sizeof(event));
        event.smp=sample; event.smp_speed=reverse?-.5:.5; event.stretch=2;
        event.time=.25; event.gain=.1; event.sus=1; event.rel=.01;
        dsp_params_init(&event.dsp); event.dsp.enabled=1;
        event.stretch_buffer=stretch_prepare(&event); assert(event.stretch_buffer);
        const StretchBuffer *again=stretch_prepare(&event); assert(again==event.stretch_buffer); stretch_release(again);
        AudioEvent filtered=event; filtered.gain=.2; filtered.dsp.p[DSP_LPF]=800;
        again=stretch_prepare(&filtered); assert(again==event.stretch_buffer); stretch_release(again);
        Voice *v=start_sample(&event); stretch_release(event.stretch_buffer);
        assert(v->stretch_buffer && stretch_frames(v->stretch_buffer)>40000 && stretch_frames(v->stretch_buffer)<44100);
        double previous = 0; int crossings = 0;
        for (int i = 0; i < frames*2; i++) {
            float output[2]; pa_callback(NULL,output,1,NULL,0,NULL);
            assert(isfinite(output[0]) && fabs(output[1]+2*output[0]) < 1e-6);
            if (i > 4410 && i < 39690 && previous <= 0 && output[0] > 0) crossings++;
            previous = output[0];
        }
        assert(!v->active); /* .5 transport doubles duration, not pitch */
        double frequency = crossings/.8;
        assert(frequency > 430 && frequency < 450);
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

int main(void) {
    /* Exercise the actual callback with opposite-polarity stereo channels.
       Averaging to mono would cancel this signal. No audio device is needed. */
    float stereo[] = {.25f,-.5f,.125f,.375f};
    Voice *v = &voices[0];
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
    pan_voice.has_pan=1; memset(&mix,0,sizeof(mix));
    route_voice(&pan_voice,1,1,&mix);
    assert(fabs(mix.left-sqrt(.5)) < 1e-9 && fabs(mix.right-sqrt(.5)) < 1e-9);
    pan_voice.smp_channels = 2;
    memset(&mix,0,sizeof(mix));
    route_voice(&pan_voice,1,.25,&mix);
    assert(fabs(mix.left-1) < 1e-9 && fabs(mix.right-.25) < 1e-9);
    pan_voice.pan = 1; memset(&mix,0,sizeof(mix));
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
    float impulse[2] = {1,1};
    voices[0].active = 1; voices[0].orbit = 4;
    voices[0].smpbuf = impulse; voices[0].smplen = 1; voices[0].smp_channels = 1;
    voices[0].smpstep = 1; voices[0].smp_end = 1; voices[0].amp = .5;
    voices[0].delay_send = 1; voices[0].note_off_time = 10; voices[0].gate_end = 10;
    float delayed[80];
    pa_callback(NULL,delayed,40,NULL,0,NULL);
    assert(fabs(delayed[0]-.5) < 1e-5 && fabs(delayed[2]) < 1e-5);
    assert(fabs(delayed[32*2]-.5) < 1e-4);

    memset(voices,0,sizeof(voices));
    DspParams held; dsp_params_init(&held); held.enabled = 1;
    voices[1].active = 1; voices[1].orbit = 2; voices[1].inst = INST_SINE;
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
    assert(routed.duck_mask == ((1u<<1)|(1u<<5)));
    assert(fabs(routed.duck_attack-.25) < 1e-9 && fabs(routed.duck_depth-.5) < 1e-9);
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
    test_convolution();
    test_stretch();
    test_pattern_scopes();
    test_live_scope_attachment();

    puts("native callback, stereo/pan/ADSR, stretch and partitioned convolution ok");
    return 0;
}
