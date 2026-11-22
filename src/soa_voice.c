#include "soa_voice.h"
#include "common.h"
#include <math.h>
#include <string.h>

static double clamp_sample(double x) {
    return isfinite(x) ? (x < -64 ? -64 : x > 64 ? 64 : x) : 0;
}

void soa_voice_bank_clear(SoaVoiceBank *bank) {
    memset(bank,0,sizeof(*bank));
}

double soa_voice_filter_frequency(const SoaVoiceLane *lane, double age) {
    if (!lane) return 0;
    return dsp_filter_frequency_values(lane->filter_cutoff,lane->filter_env,
        lane->filter_anchor,lane->filter_a,lane->filter_d,lane->filter_s,
        lane->filter_r,age,lane->gate);
}

static void store_filter(DspBiquad4 *dst, int lane, const DspBiquad *f) {
    dst->b0[lane]=f->b0; dst->b1[lane]=f->b1; dst->b2[lane]=f->b2;
    dst->a1[lane]=f->a1; dst->a2[lane]=f->a2;
    dst->z1[lane]=f->z1; dst->z2[lane]=f->z2;
}

int soa_voice_bank_add(SoaVoiceBank *bank, const SoaVoiceLane *lane) {
    if (!bank || !lane) return -1;
    for (int index=0;index<SOA_VOICE_LANES;index++) {
        SoaVoiceLane *dst=&bank->lanes[index];
        if (dst->active) continue;
        *dst=*lane;
        dst->active=1; dst->ended=0;
        int group=index/4, sub=index%4;
        store_filter(&bank->filter_l[group],sub,&lane->biquad_l);
        DspBiquad right=lane->biquad_r;
        if (!lane->stereo_filter) {
            right=lane->biquad_l;
            right.z1=right.z2=0;
        }
        store_filter(&bank->filter_r[group],sub,&right);
        bank->active_group[group]=1;
        return index;
    }
    return -1;
}

void soa_voice_bank_remove(SoaVoiceBank *bank, int lane) {
    if (!bank || lane<0 || lane>=SOA_VOICE_LANES) return;
    int group=lane/4, sub=lane%4;
    memset(&bank->lanes[lane],0,sizeof(bank->lanes[lane]));
    memset(&bank->filter_l[group].b0[sub],0,sizeof(double));
    memset(&bank->filter_l[group].b1[sub],0,sizeof(double));
    memset(&bank->filter_l[group].b2[sub],0,sizeof(double));
    memset(&bank->filter_l[group].a1[sub],0,sizeof(double));
    memset(&bank->filter_l[group].a2[sub],0,sizeof(double));
    memset(&bank->filter_l[group].z1[sub],0,sizeof(double));
    memset(&bank->filter_l[group].z2[sub],0,sizeof(double));
    memset(&bank->filter_r[group].b0[sub],0,sizeof(double));
    memset(&bank->filter_r[group].b1[sub],0,sizeof(double));
    memset(&bank->filter_r[group].b2[sub],0,sizeof(double));
    memset(&bank->filter_r[group].a1[sub],0,sizeof(double));
    memset(&bank->filter_r[group].a2[sub],0,sizeof(double));
    memset(&bank->filter_r[group].z1[sub],0,sizeof(double));
    memset(&bank->filter_r[group].z2[sub],0,sizeof(double));
    bank->active_group[group]=0;
    for (int i=0;i<4;i++) if (bank->lanes[group*4+i].active) {
        bank->active_group[group]=1;
        break;
    }
}

static void update_coefficients(SoaVoiceBank *bank, int group, int sub,
                                const SoaVoiceLane *lane, double age) {
    if (lane->filterless) return;
    DspBiquad filter;
    dsp_filter_q(&filter,lane->filter_type,
        soa_voice_filter_frequency(lane,age),lane->filter_q);
    bank->filter_l[group].b0[sub]=filter.b0;
    bank->filter_l[group].b1[sub]=filter.b1;
    bank->filter_l[group].b2[sub]=filter.b2;
    bank->filter_l[group].a1[sub]=filter.a1;
    bank->filter_l[group].a2[sub]=filter.a2;
    if (lane->stereo_filter) {
        bank->filter_r[group].b0[sub]=filter.b0;
        bank->filter_r[group].b1[sub]=filter.b1;
        bank->filter_r[group].b2[sub]=filter.b2;
        bank->filter_r[group].a1[sub]=filter.a1;
        bank->filter_r[group].a2[sub]=filter.a2;
    }
}

static int process_sample_lane(SoaVoiceLane *lane, int sub, double *in_l,
                               double *in_r) {
    if (lane->age>=lane->gate+lane->rel) {
        lane->ended=1; lane->active=0; return 0;
    }
    int at=(int)lane->pos;
    if (lane->pos<0.0 || at>=lane->smplen || !isfinite(lane->pos)) {
        lane->ended=1; lane->active=0; return 0;
    }
    double fraction=lane->pos-at;
    double a_l=lane->buf[(size_t)at*2];
    double b_l=at+1<lane->smplen?lane->buf[(size_t)(at+1)*2]:a_l;
    double a_r=lane->buf[(size_t)at*2+1];
    double b_r=at+1<lane->smplen?lane->buf[(size_t)(at+1)*2+1]:a_r;
    double left=a_l+(b_l-a_l)*fraction;
    double right=a_r+(b_r-a_r)*fraction;
    if (lane->age>=lane->gate) lane->released=1;
    double env=dsp_envelope(lane->age,lane->gate,lane->att,lane->dec,
        lane->sus,lane->rel);
    /* Scalar order is (sample*amp)*env, not sample*(amp*env). */
    in_l[sub]=left*lane->amp*env;
    in_r[sub]=lane->channels==2?right*lane->amp*env:left*lane->amp*env;
    lane->pos+=lane->step;
    lane->age+=1.0/SR;
    if (lane->pos<0.0 || lane->pos>=lane->smplen) {
        lane->ended=1;
        lane->active=0;
    }
    return 1;
}

static int process_supersaw_lane(SoaVoiceLane *lane, int sub, double time,
                                 double *in_l, double *in_r) {
    if (!lane->released && time>=lane->note_off_time) {
        lane->released=1;
        lane->release_start_level=lane->env;
        lane->release_time=time;
    }
    int end=lane->released && time-lane->release_time>=lane->env_rel;
    double env=dsp_envelope(lane->age,lane->gate,lane->env_att,
        lane->env_dec,lane->env_sus,lane->env_rel);
    lane->env=env;
    double freq=dsp_frequency(lane->dsp,
        lane->freq*(lane->stretch>0?lane->stretch:1),lane->age,lane->gate);
    if (time+1e-10>=lane->source_begin)
        dsp_supersaw(lane->dsp,freq);
    else
        lane->dsp->supersaw_l=lane->dsp->supersaw_r=0;
    double amp=lane->amp*.3;
    in_l[sub]=lane->dsp->supersaw_l*env*amp;
    in_r[sub]=lane->dsp->supersaw_r*env*amp;
    lane->age+=1.0/SR;
    if (end) { lane->ended=1; lane->active=0; }
    return 1;
}

static int process_synth_lane(SoaVoiceLane *lane, int sub, double time,
                              double *in_l) {
    if (!lane->released && time>=lane->note_off_time) {
        lane->released=1;
        lane->release_start_level=lane->env;
        lane->release_time=time;
    }
    int end=lane->released && time-lane->release_time>=lane->env_rel;
    double env=dsp_envelope(lane->age,lane->gate,lane->env_att,
        lane->env_dec,lane->env_sus,lane->env_rel);
    lane->env=env;
    double sample;
    switch (lane->kind) {
        case SOA_KIND_SINE: sample=sin(lane->phase); break;
        case SOA_KIND_SAW: sample=dsp_oscillator(0,lane->phase,lane->freq,.5); break;
        case SOA_KIND_SQUARE: sample=dsp_oscillator(1,lane->phase,lane->freq,
            lane->pw>=0?lane->pw:.5); break;
        default: sample=dsp_oscillator(2,lane->phase,lane->freq,.5); break;
    }
    sample*=lane->wave_gain;
    double smp=sample*env*lane->amp;
    smp*=.3;
    in_l[sub]=smp;
    lane->age+=1.0/SR;
    lane->phase+=lane->phase_inc;
    if (lane->phase>2*M_PI || lane->phase<-2*M_PI)
        lane->phase=fmod(lane->phase,2*M_PI);
    if (end) { lane->ended=1; lane->active=0; }
    return 1;
}

void soa_voice_bank_process(SoaVoiceBank *bank, unsigned frames,
                            const double *times, double *out_l, double *out_r) {
    if (!bank || !times || !out_l || !out_r || frames>SOA_VOICE_FRAMES) return;
    memset(out_l,0,frames*SOA_VOICE_LANES*sizeof(double));
    memset(out_r,0,frames*SOA_VOICE_LANES*sizeof(double));
    for (int index=0;index<SOA_VOICE_LANES;index++)
        bank->lanes[index].rendered=0;
    for (int group=0;group<SOA_VOICE_GROUPS;group++) {
        if (!bank->active_group[group]) continue;
        for (unsigned frame=0;frame<frames;frame++) {
            double in_l[4]={0}, in_r[4]={0};
            double result_l[4]={0}, result_r[4]={0};
            unsigned rendered=0, stereo=0;
            for (int sub=0;sub<4;sub++) {
                int index=group*4+sub;
                SoaVoiceLane *lane=&bank->lanes[index];
                if (!lane->active) continue;
                /* The scalar sample path passes the pre-increment age to the
                   filter; the oscillator path has already advanced. */
                double filter_age=lane->kind==SOA_KIND_SAMPLE?
                    lane->age:lane->age+1.0/SR;
                int ok;
                if (lane->kind==SOA_KIND_SAMPLE) {
                    ok=process_sample_lane(lane,sub,in_l,in_r);
                    if (ok && lane->channels==2) stereo=1;
                } else if (lane->kind==SOA_KIND_SUPERSAW) {
                    ok=process_supersaw_lane(lane,sub,times[frame],in_l,in_r);
                    if (ok) stereo=1;
                } else {
                    ok=process_synth_lane(lane,sub,times[frame],in_l);
                }
                if (!ok) continue;
                rendered|=1u<<sub;
                lane->rendered|=1u<<frame;
                if ((lane->tick&31u)==0)
                    update_coefficients(bank,group,sub,lane,filter_age);
                lane->tick++;
            }
            dsp_biquad4_process(&bank->filter_l[group],in_l,result_l,1);
            if (stereo)
                dsp_biquad4_process(&bank->filter_r[group],in_r,result_r,1);
            else
                for (int sub=0;sub<4;sub++) result_r[sub]=result_l[sub];
            for (int sub=0;sub<4;sub++) {
                if (!(rendered&(1u<<sub))) continue;
                int index=group*4+sub;
                int lane_stereo=(bank->lanes[index].kind==SOA_KIND_SAMPLE &&
                    bank->lanes[index].channels==2) ||
                    bank->lanes[index].kind==SOA_KIND_SUPERSAW;
                out_l[(size_t)frame*SOA_VOICE_LANES+index]=clamp_sample(result_l[sub]);
                out_r[(size_t)frame*SOA_VOICE_LANES+index]=
                    clamp_sample(lane_stereo?result_r[sub]:result_l[sub]);
            }
        }
    }
}
