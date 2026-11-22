/* Scheduler-owned live LV2 instrument: bounded thread, ring and note/CC
   queue. The self-contained MIDI fixture is loaded from LV2_PATH. */
#include "live_lv2.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static double mix_energy(unsigned frames) {
    float left[128], right[128];
    double energy=0;
    for (unsigned at=0;at<frames;at+=128) {
        unsigned count=frames-at<128?frames-at:128;
        memset(left,0,sizeof(left));
        memset(right,0,sizeof(right));
        live_lv2_mix(left,right,count);
        for (unsigned i=0;i<count;i++)
            energy+=fabs(left[i])+fabs(right[i]);
    }
    return energy;
}

int main(void) {
    char error[256]="";
    int slot=live_lv2_acquire("urn:tompouce:test:midi-synth",44100,0,
        error,sizeof(error));
    if (slot<0) {
        printf("SKIP live LV2: %s\n",error[0]?error:"unavailable");
        return 0;
    }
    usleep(80000);
    assert(mix_energy(2048)<1e-6); /* silence before any note */

    assert(live_lv2_note(slot,0,.15,0,57,127,-1,0));
    usleep(50000);
    assert(mix_energy(44100/4)>1);

    /* The note-off lands at 0.15 s; after draining the ring the instrument is
       silent again. */
    assert(mix_energy(44100/2)<1e-6);

    /* CC 74 = 0 before note-on silences the fixture; CC 74 = 127 restores it. */
    assert(live_lv2_note(slot,0,.1,0,57,127,74,0));
    usleep(50000);
    assert(mix_energy(22050)<1e-6);
    assert(live_lv2_note(slot,0,.1,0,57,127,74,127));
    usleep(50000);
    assert(mix_energy(22050)>1);
    assert(mix_energy(44100/2)<1e-6);

    /* Far-future events fill the bounded queue without touching audio. */
    int accepted=0;
    for (int i=0;i<200;i++)
        accepted+=live_lv2_cc(slot,1000+i,0,1,i%128);
    assert(accepted>0 && accepted<200);

    assert(!live_lv2_note(slot,-1,.1,0,60,100,-1,0));
    assert(!live_lv2_note(slot,0,.1,16,60,100,-1,0));
    assert(!live_lv2_cc(slot,0,0,128,0));
    live_lv2_shutdown();
    puts("live LV2 instrument ring, note/CC queue and silence after note-off ok");
    return 0;
}
