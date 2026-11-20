/* Confined LV2 worker: plugin code runs in a child, PCM is bounded. */
#include "lv2_worker.h"
#include "js_audio.h"
#include "sampler.h"
#include <cjson/cJSON.h>
#include <assert.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *plugin_uri(void) {
    const char *override=getenv("TOMPOUCE_LV2_TEST_PLUGIN");
    return override && *override ? override :
        "http://kxstudio.sf.net/carla/plugins/audiogain_s";
}

static char engine_sound[512];
static const char *plugin_uri_for_engine(void) {
    snprintf(engine_sound,sizeof(engine_sound),"lv2:%s",plugin_uri());
    return engine_sound;
}

/* MIDI plumbing: a plugin with an atom MIDI input accepts a bounded message
   and still processes audio without the worker dying. */
static void test_midi(void) {
    char error[512]="";
    const char *uri="http://kxstudio.sf.net/carla/plugins/midichanfilter";
    Lv2Worker *worker=lv2_worker_start(uri,44100,error,sizeof(error));
    if (!worker) { printf("SKIP LV2 MIDI: %s\n",error[0]?error:"unavailable"); return; }
    float in[LV2_WORKER_FRAMES*LV2_WORKER_CHANNELS]={0};
    float out[LV2_WORKER_FRAMES*LV2_WORKER_CHANNELS]={0};
    const unsigned char note_on[]={0x90,60,100}, note_off[]={0x80,60,0};
    assert(lv2_worker_send_midi(worker,note_on,3,error,sizeof(error)));
    assert(lv2_worker_process(worker,in,out,LV2_WORKER_FRAMES,error,sizeof(error)));
    assert(lv2_worker_send_midi(worker,note_off,3,error,sizeof(error)));
    assert(lv2_worker_process(worker,in,out,LV2_WORKER_FRAMES,error,sizeof(error)));
    /* Oversized messages are rejected before reaching the plugin. */
    const unsigned char oversized[8]={0};
    assert(!lv2_worker_send_midi(worker,oversized,8,error,sizeof(error)));
    lv2_worker_stop(worker);
    puts("LV2 MIDI note on/off plumbing ok");
}

/* A deterministic instrument reference render: the LSP oscillator becomes an
   owned PCM buffer through the same worker protocol. Strudel.cc has no LV2
   counterpart, so this pinned hash is the reference gate for the instrument
   side, alongside the gain effect render below. */
static void test_instrument_reference(void) {
    const char *uri = "http://lsp-plug.in/plugins/lv2/oscillator_mono";
    char error[512] = "";
    Lv2Worker *worker = lv2_worker_start(uri,44100,error,sizeof(error));
    if (!worker) { printf("SKIP LV2 instrument: %s\n",error[0]?error:"unavailable"); return; }
    const int frames = 13231;
    float *pcm = calloc((size_t)frames*2,sizeof(float));
    assert(pcm);
    double phase=0, step=2*M_PI*220.0/44100.0;
    for (int i=0;i<frames;i++) {
        double fade=fmin(1,i/(.005*44100.0))*exp(-i/(.5*44100.0));
        pcm[i*2]=(float)(.25*sin(phase)*fade);
        pcm[i*2+1]=pcm[i*2]; phase+=step;
    }
    assert(lv2_worker_set_control(worker,0,.5f,error,sizeof(error)));
    int ok=1;
    for (int at=0;at<frames && ok;at+=LV2_WORKER_FRAMES) {
        int count=frames-at<LV2_WORKER_FRAMES?frames-at:LV2_WORKER_FRAMES;
        ok=lv2_worker_process(worker,pcm+at*2,pcm+at*2,(unsigned)count,error,sizeof(error));
    }
    lv2_worker_stop(worker);
    assert(ok);
    double energy=0; uint64_t hash=1469598103934665603ull;
    for (int i=0;i<frames*2;i++) {
        energy+=fabs(pcm[i]);
        uint32_t bits; memcpy(&bits,&pcm[i],sizeof(bits));
        for (int b=0;b<4;b++) { hash^=(bits>>(b*8))&0xff; hash*=1099511628211ull; }
    }
    free(pcm);
    assert(energy>0);
    assert(hash==13122609104998660811ull && frames==13231);
    puts("LV2 instrument reference render ok");
}

/* Engine integration: an lv2: sound is prepared off the audio callback and
   becomes a normal owned playback sample. */
static void test_engine_resolution(void) {
    cJSON *value=cJSON_CreateObject();
    cJSON_AddStringToObject(value,"s",plugin_uri_for_engine());
    cJSON_AddNumberToObject(value,"gain",.5);
    AudioEvent ev;
    memset(&ev,0,sizeof(ev));
    dsp_params_init(&ev.dsp); ev.dsp.enabled=1;
    ev.freq=220; ev.duration=.2; ev.smp=-1; ev.wt=-1; ev.smp_speed=1;
    int ok=js_audio_resolve_sample(&ev,value);
    cJSON_Delete(value);
    if (!ok) {
        printf("LV2 engine resolution unavailable\n");
        return;
    }
    assert(ev.smp>=0);
    float *pcm=NULL; int frames=0, channels=0;
    smp_get_stereo(ev.smp,&pcm,&frames,&channels);
    assert(pcm && frames>0 && channels==2);
    double energy=0; uint64_t hash=1469598103934665603ull;
    for (int i=0;i<frames*2;i++) {
        energy+=fabs(pcm[i]);
        uint32_t bits; memcpy(&bits,&pcm[i],sizeof(bits));
        for (int b=0;b<4;b++) { hash^=(bits>>(b*8))&0xff; hash*=1099511628211ull; }
    }
    assert(energy>0);
    /* Pinned native reference render for the default gain plugin at 44.1 kHz.
       An env override uses a different plugin, so the hash only applies to the
       default fixture. Strudel.cc has no LV2 counterpart; this deterministic
       PCM hash is the reference gate for the LV2 path. */
    if (!getenv("TOMPOUCE_LV2_TEST_PLUGIN"))
        assert(hash==4044680287945718928ull && frames==13231);
    printf("LV2 engine resolution ok: %d frames, energy %.4f, hash %llu\n",
        frames,energy,(unsigned long long)hash);
}

int main(void) {
    test_midi();
    test_engine_resolution();
    test_instrument_reference();
    char error[512]="";
    Lv2Worker *worker=lv2_worker_start(plugin_uri(),44100,error,sizeof(error));
    if (!worker) {
        printf("LV2 worker unavailable: %s\n",error[0]?error:"unknown");
        return 0;
    }
    assert(lv2_worker_pid(worker)>0);
    char limits[256];
    snprintf(limits,sizeof(limits),"/proc/%d/limits",(int)lv2_worker_pid(worker));
    FILE *file=fopen(limits,"r");
    if (file) {
        char line[256]; int core_zero=0, address_bound=0;
        while (fgets(line,sizeof(line),file)) {
            if (strstr(line,"Max core file size") && strstr(line," 0 ")) core_zero=1;
            if (strstr(line,"Max address space") && strstr(line,"2147483648")) address_bound=1;
        }
        fclose(file);
        assert(core_zero && address_bound);
    }
    float input[LV2_WORKER_FRAMES*LV2_WORKER_CHANNELS];
    float output[LV2_WORKER_FRAMES*LV2_WORKER_CHANNELS];
    for (unsigned i=0;i<LV2_WORKER_FRAMES*LV2_WORKER_CHANNELS;i++)
        input[i]=(float)(.1*sin(i*0.01));
    /* Port 0 of audiogain_s is its gain control, default 0. Out-of-range
       indices and non-finite values are rejected before reaching the child. */
    assert(!lv2_worker_set_control(worker,9999,1.0f,error,sizeof(error)));
    assert(!lv2_worker_set_control(worker,0,NAN,error,sizeof(error)));
    assert(lv2_worker_set_control(worker,0,1.0f,error,sizeof(error)));
    assert(lv2_worker_process(worker,input,output,LV2_WORKER_FRAMES,error,sizeof(error)));
    double energy=0;
    for (unsigned i=0;i<LV2_WORKER_FRAMES*LV2_WORKER_CHANNELS;i++) {
        assert(isfinite(output[i]));
        energy+=fabs(output[i]);
    }
    assert(energy>0);
    lv2_worker_stop(worker);

    /* A missing plugin must fail startup instead of loading arbitrary code. */
    worker=lv2_worker_start("http://invalid.example/no-such-plugin",44100,error,sizeof(error));
    assert(!worker && error[0]);
    /* A dead worker must report failure rather than hanging. */
    worker=lv2_worker_start(plugin_uri(),44100,error,sizeof(error));
    if (worker) {
        kill(lv2_worker_pid(worker),SIGKILL);
        usleep(50000);
        assert(!lv2_worker_process(worker,input,output,128,error,sizeof(error)));
        lv2_worker_stop(worker);
    }
    puts("LV2 worker confinement, bounded PCM and failure handling ok");
    return 0;
}
