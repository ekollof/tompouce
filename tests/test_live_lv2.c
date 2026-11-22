/* Scheduler-owned live LV2 instrument: bounded thread, ring, note/CC queue
   and control-port updates. The self-contained MIDI fixture is loaded from
   LV2_PATH. */
#include "live_lv2.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
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
        usleep(1000); /* let the render thread refill its bounded ring */
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
    assert(live_lv2_parameter_count(slot)>=1);
    int gain_index=-1;
    for (int i=0;i<live_lv2_parameter_count(slot);i++) {
        char name[64]=""; float min=0,max=0,value=0;
        assert(live_lv2_parameter_info(slot,i,name,&min,&max,&value));
        if (!strcasecmp(name,"Gain")) {
            gain_index=i;
            assert(min==0 && max==1 && value==1);
        }
    }
    assert(gain_index>=0);
    usleep(80000);
    assert(mix_energy(2048)<1e-6); /* silence before any note */

    double now=live_lv2_time(slot)+.02;
    assert(live_lv2_note(slot,now,.15,0,57,127,-1,0));
    usleep(80000);
    double first_energy=mix_energy(44100/4);
    assert(first_energy>1);

    /* The note-off lands at 0.15 s; after draining the ring the instrument is
       silent again. */
    assert(mix_energy(44100/2)<1e-6);

    /* A long note keeps sounding while a named control changes mid-note. */
    now=live_lv2_time(slot)+.02;
    assert(live_lv2_note(slot,now,.6,0,57,127,-1,0));
    usleep(80000);
    double full=mix_energy(44100/5);
    assert(full>10);
    assert(live_lv2_control(slot,live_lv2_time(slot),"Gain",.25f));
    usleep(30000);
    mix_energy(44100/10); /* transition through the ring target */
    double quiet=mix_energy(44100/10);
    assert(quiet>0 && quiet<full*.6);
    assert(live_lv2_control(slot,live_lv2_time(slot),"Gain",1.0f));
    usleep(30000);
    mix_energy(44100/10);
    double loud=mix_energy(44100/10);
    assert(loud>quiet*2);
    mix_energy(44100/2);

    /* Installed presets resolve by label and apply their declared port values
       without retriggering the held note. */
    now=live_lv2_time(slot)+.02;
    assert(live_lv2_note(slot,now,1.0,0,57,127,-1,0));
    usleep(80000);
    double preset_full=mix_energy(44100/5);
    assert(preset_full>10);
    assert(live_lv2_preset(slot,live_lv2_time(slot),"Quiet"));
    usleep(30000);
    mix_energy(44100/10);
    double preset_quiet=mix_energy(44100/10);
    assert(preset_quiet>0 && preset_quiet<preset_full*.6);
    assert(!live_lv2_preset(-1,0,"Quiet"));
    assert(!live_lv2_preset(slot,0,""));
    mix_energy(44100);
    assert(live_lv2_control(slot,live_lv2_time(slot),"Gain",1.0f));

    /* CC 74 = 0 before note-on silences the fixture; CC 74 = 127 restores it. */
    now=live_lv2_time(slot)+.02;
    assert(live_lv2_note(slot,now,.1,0,57,127,74,0));
    usleep(80000);
    assert(mix_energy(22050)<1e-6);
    now=live_lv2_time(slot)+.02;
    assert(live_lv2_note(slot,now,.1,0,57,127,74,127));
    usleep(80000);
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
    assert(!live_lv2_control(slot,0,"NoSuchParameter",1));
    assert(!live_lv2_control(-1,0,"Gain",1));
    live_lv2_shutdown();
    puts("live LV2 ring, note/CC, mid-note controls and bounded queues ok");
    return 0;
}
