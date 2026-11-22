/* Exactness of the from-allocation SoA lanes against the scalar voice paths. */
#include "../src/soa_voice.c"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#define TEST_FRAMES 256

static uint32_t random_state=0xbb67ae85u;
static double random_value(void) {
    random_state=random_state*1664525u+1013904223u;
    return (double)(random_state>>8)/16777216.0-0.5;
}

static void make_times(double *times, unsigned frames) {
    for (unsigned i=0;i<frames;i++) times[i]=i/SR;
}

static void fill_filter(SoaVoiceLane *lane, const DspParams *control) {
    lane->lpf=control->p[DSP_LPF]; lane->lpq=control->p[DSP_LPQ];
    lane->lpe=control->p[DSP_LPE]; lane->lanchor=control->p[DSP_ANCHOR];
    lane->lpa=control->p[DSP_LPA]; lane->lpd=control->p[DSP_LPD];
    lane->lps=control->p[DSP_LPS]; lane->lpr=control->p[DSP_LPR];
}

static void scalar_sample(const DspParams *control, const float *pcm,
    double pos, double step, double amp, double age, double gate, double att,
    double dec, double sus, double rel, int channels, int frames, double *left,
    double *right) {
    DspState dsp, dsp_right;
    dsp_init(&dsp,control); dsp_init(&dsp_right,control);
    for (int frame=0;frame<frames;frame++) {
        if (age>=gate+rel) { left[frame]=right[frame]=0; continue; }
        int at=(int)pos;
        if (pos<0.0 || at>=frames) { left[frame]=right[frame]=0; continue; }
        double fraction=pos-at;
        double a_l=pcm[(size_t)at*2];
        double b_l=at+1<frames?pcm[(size_t)(at+1)*2]:a_l;
        double a_r=pcm[(size_t)at*2+1];
        double b_r=at+1<frames?pcm[(size_t)(at+1)*2+1]:a_r;
        double l=a_l+(b_l-a_l)*fraction;
        double r=a_r+(b_r-a_r)*fraction;
        double env=dsp_envelope(age,gate,att,dec,sus,rel);
        left[frame]=dsp_process(&dsp,l*amp*env,age,gate);
        right[frame]=channels==2?dsp_process(&dsp_right,r*amp*env,age,gate):
            left[frame];
        pos+=step; age+=1.0/SR;
    }
}

static void scalar_synth(const DspParams *control, int kind, double amp,
    double wave_gain, double freq, double pw, double age, double gate,
    double env_att, double env_dec, double env_sus, double env_rel,
    double note_off, int frames, const double *times, double *out) {
    DspState dsp; dsp_init(&dsp,control);
    double env=0, released=0, release_time=0;
    double phase=0, phase_inc=2*M_PI*freq/SR;
    for (int frame=0;frame<frames;frame++) {
        double t=times[frame];
        if (!released && t>=note_off) { released=1; release_time=t; }
        int end=released && t-release_time>=env_rel;
        env=dsp_envelope(age,gate,env_att,env_dec,env_sus,env_rel);
        double sample;
        if (kind==SOA_KIND_SINE) sample=sin(phase);
        else if (kind==SOA_KIND_SAW) sample=dsp_oscillator(0,phase,freq,.5);
        else if (kind==SOA_KIND_SQUARE)
            sample=dsp_oscillator(1,phase,freq,pw>=0?pw:.5);
        else sample=dsp_oscillator(2,phase,freq,.5);
        sample*=wave_gain;
        double smp=sample*env*amp;
        smp*=.3;
        age+=1.0/SR;
        out[frame]=dsp_process(&dsp,smp,age,gate);
        phase+=phase_inc;
        if (phase>2*M_PI || phase<-2*M_PI) phase=fmod(phase,2*M_PI);
        if (end) break;
    }
}

static SoaVoiceLane sample_lane(const DspParams *control, const float *pcm,
    double pos, double step, double amp, double age, double gate, double att,
    double dec, double sus, double rel, int channels, int frames) {
    SoaVoiceLane lane={0};
    lane.kind=SOA_KIND_SAMPLE; lane.buf=pcm; lane.channels=channels;
    lane.smplen=frames; lane.pos=pos; lane.step=step; lane.amp=amp;
    lane.age=age; lane.gate=gate; lane.att=att; lane.dec=dec;
    lane.sus=sus; lane.rel=rel; lane.raw_rel=rel;
    lane.stereo_filter=channels==2;
    fill_filter(&lane,control);
    return lane;
}

static SoaVoiceLane synth_lane(const DspParams *control, int kind, double amp,
    double wave_gain, double freq, double pw, double age, double gate,
    double env_att, double env_dec, double env_sus, double env_rel,
    double note_off) {
    SoaVoiceLane lane={0};
    lane.kind=kind; lane.amp=amp; lane.wave_gain=wave_gain;
    lane.freq=freq; lane.phase_inc=2*M_PI*freq/SR;
    lane.pw=pw; lane.age=age; lane.gate=gate;
    lane.env_att=env_att; lane.env_dec=env_dec;
    lane.env_sus=env_sus; lane.env_rel=env_rel;
    lane.rel=env_rel; lane.raw_rel=env_rel;
    lane.note_off_time=note_off;
    fill_filter(&lane,control);
    return lane;
}

static void check_samples(void) {
    const double lpes[]={0,12};
    unsigned frames=TEST_FRAMES;
    for (int case_index=0;case_index<4;case_index++) {
        int channels=case_index&1, lpe=case_index>>1;
        DspParams control; dsp_params_init(&control); control.enabled=1;
        control.p[DSP_LPF]=1200; control.p[DSP_LPQ]=2; control.p[DSP_LPE]=lpes[lpe];
        float *pcm=malloc(frames*2*sizeof(float)); assert(pcm);
        for (unsigned i=0;i<frames*2;i++) pcm[i]=(float)random_value();
        double *want_l=calloc(frames,sizeof(double));
        double *want_r=calloc(frames,sizeof(double));
        assert(want_l && want_r);
        scalar_sample(&control,pcm,1.5,.75,.2,0,.9,.005,.05,.5,.1,channels,
            (int)frames,want_l,want_r);
        SoaVoiceLane lane=sample_lane(&control,pcm,1.5,.75,.2,0,.9,.005,.05,
            .5,.1,channels,(int)frames);
        SoaVoiceBank bank; soa_voice_bank_clear(&bank);
        assert(soa_voice_bank_add(&bank,&lane)==0);
        double *times=calloc(frames,sizeof(double));
        double out_l[4*SOA_VOICE_LANES], out_r[4*SOA_VOICE_LANES];
        assert(times);
        make_times(times,frames);
        for (unsigned at=0;at<frames;at+=4) {
            soa_voice_bank_process(&bank,4,times+at,out_l,out_r);
            for (unsigned j=0;j<4;j++) {
                if (out_l[j*SOA_VOICE_LANES]!=want_l[at+j] ||
                    out_r[j*SOA_VOICE_LANES]!=want_r[at+j]) {
                    fprintf(stderr,"sample diff ch=%d lpe=%d frame=%u got=%.17g/%.17g want=%.17g/%.17g\n",
                        channels,lpe,at+j,out_l[j*SOA_VOICE_LANES],
                        out_r[j*SOA_VOICE_LANES],want_l[at+j],want_r[at+j]);
                    return;
                }
            }
        }
        free(pcm); free(want_l); free(want_r); free(times);
    }
}

static void check_synths(void) {
    const int kinds[]={SOA_KIND_SINE,SOA_KIND_SAW,SOA_KIND_SQUARE,SOA_KIND_TRI};
    unsigned frames=TEST_FRAMES;
    double *times=calloc(frames,sizeof(double)); assert(times);
    make_times(times,frames);
    for (unsigned k=0;k<sizeof(kinds)/sizeof(*kinds);k++) {
        DspParams control; dsp_params_init(&control); control.enabled=1;
        control.p[DSP_LPF]=1800; control.p[DSP_LPQ]=1; control.p[DSP_LPE]=7;
        double *want=calloc(frames,sizeof(double)); assert(want);
        scalar_synth(&control,kinds[k],.15,.9,220,.4,0,.4,.002,.08,.6,.005,
            0,(int)frames,times,want);
        SoaVoiceLane lane=synth_lane(&control,kinds[k],.15,.9,220,.4,0,.4,
            .002,.08,.6,.005,0);
        SoaVoiceBank bank; soa_voice_bank_clear(&bank);
        double out_l[4*SOA_VOICE_LANES], out_r[4*SOA_VOICE_LANES];
        assert(soa_voice_bank_add(&bank,&lane)==0);
        for (unsigned at=0;at<frames;at+=4) {
            soa_voice_bank_process(&bank,4,times+at,out_l,out_r);
            for (unsigned j=0;j<4;j++) {
                assert(out_l[j*SOA_VOICE_LANES]==want[at+j]);
                assert(out_r[j*SOA_VOICE_LANES]==want[at+j]);
                if (!bank.lanes[0].active) break;
            }
            if (!bank.lanes[0].active) break;
        }
        free(want);
    }
    free(times);
}

/* Four lanes share a biquad group; removing one must not disturb the others,
   and each lane's output must match its own scalar reference. */
static void check_group_isolation(void) {
    DspParams control; dsp_params_init(&control); control.enabled=1;
    control.p[DSP_LPF]=2000; control.p[DSP_LPQ]=3; control.p[DSP_LPE]=5;
    unsigned frames=64;
    float *pcm=malloc(frames*2*sizeof(float)); assert(pcm);
    for (unsigned i=0;i<frames*2;i++) pcm[i]=(float)random_value();
    double *times=calloc(frames,sizeof(double)); assert(times);
    make_times(times,frames);
    double *want_l[4]={0}, *want_r[4]={0};
    for (int i=0;i<4;i++) {
        want_l[i]=calloc(frames,sizeof(double));
        want_r[i]=calloc(frames,sizeof(double));
        assert(want_l[i] && want_r[i]);
    }
    SoaVoiceLane lanes[4];
    lanes[0]=sample_lane(&control,pcm,.5,.5,.1,0,.5,.01,.1,.5,.1,1,(int)frames);
    lanes[1]=sample_lane(&control,pcm,1.5,1.0,.2,0,.5,.02,.1,.5,.1,2,(int)frames);
    lanes[2]=synth_lane(&control,SOA_KIND_SINE,.1,.8,330,-1,0,.5,.005,.2,.5,
        .15,1.0);
    lanes[3]=synth_lane(&control,SOA_KIND_TRI,.1,.8,440,-1,0,.5,.005,.2,.5,
        .15,1.0);
    scalar_sample(&control,pcm,.5,.5,.1,0,.5,.01,.1,.5,.1,1,(int)frames,
        want_l[0],want_r[0]);
    scalar_sample(&control,pcm,1.5,1.0,.2,0,.5,.02,.1,.5,.1,2,(int)frames,
        want_l[1],want_r[1]);
    scalar_synth(&control,SOA_KIND_SINE,.1,.8,330,-1,0,.5,.005,.2,.5,.15,1.0,
        (int)frames,times,want_l[2]);
    scalar_synth(&control,SOA_KIND_TRI,.1,.8,440,-1,0,.5,.005,.2,.5,.15,1.0,
        (int)frames,times,want_l[3]);
    for (int i=2;i<4;i++)
        for (unsigned f=0;f<frames;f++) want_r[i][f]=want_l[i][f];
    SoaVoiceBank bank; soa_voice_bank_clear(&bank);
    double out_l[4*SOA_VOICE_LANES], out_r[4*SOA_VOICE_LANES];
    for (int i=0;i<4;i++) assert(soa_voice_bank_add(&bank,&lanes[i])==i);
    for (unsigned at=0;at<16;at+=4) {
        soa_voice_bank_process(&bank,4,times+at,out_l,out_r);
        for (unsigned j=0;j<4;j++)
            for (int i=0;i<4;i++) {
                assert(out_l[j*SOA_VOICE_LANES+i]==want_l[i][at+j]);
                assert(out_r[j*SOA_VOICE_LANES+i]==want_r[i][at+j]);
            }
    }
    /* Drop lane 1 and keep rendering the rest. */
    soa_voice_bank_remove(&bank,1);
    for (unsigned at=16;at<32;at+=4) {
        soa_voice_bank_process(&bank,4,times+at,out_l,out_r);
        for (unsigned j=0;j<4;j++) {
            assert(out_l[j*SOA_VOICE_LANES+0]==want_l[0][at+j]);
            assert(out_l[j*SOA_VOICE_LANES+2]==want_l[2][at+j]);
            assert(out_l[j*SOA_VOICE_LANES+3]==want_l[3][at+j]);
        }
    }
    free(pcm); free(times);
    for (int i=0;i<4;i++) { free(want_l[i]); free(want_r[i]); }
}

int main(void) {
    check_samples();
    check_synths();
    check_group_isolation();
    puts("SoA voice bank exactness ok");
    return 0;
}
