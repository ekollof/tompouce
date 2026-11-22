/* Exactness of the from-allocation SoA sample/ADSR/LPF bank. */
#include "../src/soa_voice.c"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static uint32_t random_state=0xbb67ae85u;
static double random_value(void) {
    random_state=random_state*1664525u+1013904223u;
    return (double)(random_state>>8)/16777216.0-0.5;
}

static void scalar_reference(const DspParams *control, const float *pcm,
    double position, double step, double amp, double age, double gate,
    double att, double dec, double sus, double rel, int channels, int frames,
    unsigned tick, double *left, double *right) {
    DspState dsp, dsp_right;
    dsp_init(&dsp,control); dsp_init(&dsp_right,control);
    dsp.tick=tick; dsp_right.tick=tick;
    for (int frame=0;frame<frames;frame++) {
        double env=dsp_envelope(age,gate,att,dec,sus,rel);
        if (age>=gate+rel) { left[frame]=right[frame]=0; continue; }
        if (position<0.0 || (int)position>=frames) { left[frame]=right[frame]=0; continue; }
        int at=(int)position;
        double fraction=position-at;
        double a_l=pcm[(size_t)at*2];
        double b_l=at+1<frames?pcm[(size_t)(at+1)*2]:a_l;
        double a_r=pcm[(size_t)at*2+(channels==2?1:0)];
        double b_r=at+1<frames?pcm[(size_t)(at+1)*2+(channels==2?1:0)]:a_r;
        double l=a_l+(b_l-a_l)*fraction;
        double r=a_r+(b_r-a_r)*fraction;
        left[frame]=dsp_process(&dsp,l*amp*env,age,gate);
        right[frame]=channels==2?dsp_process(&dsp_right,r*amp*env,age,gate):
            left[frame];
        position+=step; age+=1.0/SR;
    }
}

static void check(int channels, double gate, double lpe, unsigned frames) {
    DspParams control; dsp_params_init(&control); control.enabled=1;
    control.p[DSP_LPF]=1200; control.p[DSP_LPQ]=2; control.p[DSP_LPE]=lpe;
    float *pcm=malloc(frames*2*sizeof(float)); assert(pcm);
    for (unsigned i=0;i<frames*2;i++) pcm[i]=(float)random_value();
    double *want_l=malloc(frames*sizeof(double));
    double *want_r=malloc(frames*sizeof(double));
    double *got_l=calloc(frames*SOA_VOICE_LANES,sizeof(double));
    double *got_r=calloc(frames*SOA_VOICE_LANES,sizeof(double));
    assert(want_l && want_r && got_l && got_r);
    double pos=1.5, step=.75, amp=.2, age=0, att=.005, dec=.05, sus=.5, rel=.1;
    scalar_reference(&control,pcm,pos,step,amp,age,gate,att,dec,sus,rel,
        channels,frames,0,want_l,want_r);
    SoaVoiceBank bank; soa_voice_bank_clear(&bank);
    assert(soa_voice_bank_add(&bank,&control,pcm,pos,step,amp,age,gate,
        att,dec,sus,rel,channels,frames,0)==0);
    soa_voice_bank_process(&bank,frames,got_l,got_r);
    for (unsigned frame=0;frame<frames;frame++) {
        assert(got_l[frame*SOA_VOICE_LANES]==want_l[frame]);
        assert(got_r[frame*SOA_VOICE_LANES]==want_r[frame]);
    }
    free(pcm); free(want_l); free(want_r); free(got_l); free(got_r);
}

int main(void) {
    check(1,.9,0,64);
    check(2,.9,0,64);
    check(1,.9,12,64);
    check(2,.9,12,64);
    puts("SoA voice bank exactness ok");
    return 0;
}
