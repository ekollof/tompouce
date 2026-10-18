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

int main(void) {
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
    assert(!js_audio_event(hap,0,2,&event));
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
    assert(track_load_from_code("bpm 120\nnotes c4 e4"));
    assert(!tracks[0].javascript);
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
    return 0;
}
