/* A disposable silent output, for actual daemon/device integration checks. */
#define _POSIX_C_SOURCE 200809L
#include "audio_output.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static atomic_ulong rendered;
static void silence(float *out, unsigned long frames) {
    memset(out,0,frames*2*sizeof(*out)); atomic_fetch_add(&rendered,frames);
}
int main(int argc, char **argv) {
    int inspect=argc==3 && !strcmp(argv[2],"--inspect");
    if (argc!=2 && !inspect) return 2;
    for (int i=0;i<(inspect ? 1 : 2);i++) {
        atomic_store(&rendered,0);
        int started=audio_output_start(silence,256);
        if (inspect) {
            /* Keep only this owned stream alive while the test reads its links. */
            printf("ready backend=%s\n",audio_output_name()); fflush(stdout);
            (void)getchar();
        }
        struct timespec wait={.tv_nsec=200000000}; nanosleep(&wait,NULL);
        unsigned long frames=atomic_load(&rendered);
        printf("backend=%s active=%d frames=%lu\n",audio_output_name(),audio_output_active(),frames);
        int ok=!strcmp(audio_output_name(),argv[1]) && (started ? audio_output_active() && frames>0 : !strcmp(argv[1],"none"));
        audio_output_stop();
        if (!ok) return 1;
    }
    return 0;
}
