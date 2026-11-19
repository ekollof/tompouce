/* Confined LV2 worker: plugin code runs in a child, PCM is bounded. */
#include "lv2_worker.h"
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

int main(void) {
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
    /* Port 0 of audiogain_s is its gain control, default 0. */
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
