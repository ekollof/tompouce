#include "soa_voice.h"
#include "common.h"
#include <math.h>
#include <string.h>

void soa_voice_bank_clear(SoaVoiceBank *bank) {
    memset(bank,0,sizeof(*bank));
}

static void store_filter(DspBiquad4 *dst, int lane, const DspBiquad *f) {
    dst->b0[lane]=f->b0; dst->b1[lane]=f->b1; dst->b2[lane]=f->b2;
    dst->a1[lane]=f->a1; dst->a2[lane]=f->a2;
    dst->z1[lane]=f->z1; dst->z2[lane]=f->z2;
}

int soa_voice_bank_add(SoaVoiceBank *bank, const DspParams *control,
    const float *pcm, double position, double step, double amp,
    double age, double gate, double att, double dec, double sus, double rel,
    int channels, int frames, unsigned tick) {
    if (!bank || !control || !pcm || channels<1 || channels>2 ||
        frames<=0 || step<0) return -1;
    for (int lane=0;lane<SOA_VOICE_LANES;lane++) {
        SoaVoiceLane *v=&bank->lanes[lane];
        if (v->active) continue;
        v->active=1; v->buf=pcm; v->pos=position; v->step=step; v->amp=amp;
        v->age=age; v->gate=gate; v->att=att; v->dec=dec; v->sus=sus; v->rel=rel;
        v->channels=channels; v->smplen=frames; v->tick=tick;
        v->control=*control;
        double hz=dsp_filter_frequency(&v->control,0,age,gate);
        DspBiquad f;
        dsp_filter_coefficients(&v->control,0,hz,&f);
        f.z1=f.z2=0;
        int group=lane/4, sub=lane%4;
        store_filter(&bank->filter_l[group],sub,&f);
        f.z1=f.z2=0;
        if (channels==2) dsp_filter_coefficients(&v->control,0,hz,&f);
        store_filter(&bank->filter_r[group],sub,&f);
        return lane;
    }
    return -1;
}

void soa_voice_bank_process(SoaVoiceBank *bank, unsigned frames,
    double *out_l, double *out_r) {
    if (!bank || !out_l || !out_r) return;
    memset(out_l,0,frames*SOA_VOICE_LANES*sizeof(double));
    memset(out_r,0,frames*SOA_VOICE_LANES*sizeof(double));
    for (int group=0;group<SOA_VOICE_GROUPS;group++) {
        for (unsigned frame=0;frame<frames;frame++) {
            double input_l[4]={0}, input_r[4]={0};
            double result_l[4]={0}, result_r[4]={0};
            int stereo=0;
            for (int sub=0;sub<4;sub++) {
                int index=group*4+sub;
                SoaVoiceLane *v=&bank->lanes[index];
                if (!v->active) continue;
                if (v->age>=v->gate) {
                    /* The scalar sample path marks release at the gate. */
                }
                double env=dsp_envelope(v->age,v->gate,v->att,v->dec,
                    v->sus,v->rel);
                if (v->age>=v->gate+v->rel) { v->active=0; continue; }
                if (v->pos<0.0 || (int)v->pos>=v->smplen) {
                    v->active=0; continue;
                }
                if ((v->tick&31u)==0) {
                    double hz=dsp_filter_frequency(&v->control,0,v->age,v->gate);
                    DspBiquad f;
                    dsp_filter_coefficients(&v->control,0,hz,&f);
                    f.z1=bank->filter_l[group].z1[sub];
                    f.z2=bank->filter_l[group].z2[sub];
                    store_filter(&bank->filter_l[group],sub,&f);
                    if (v->channels==2) {
                        dsp_filter_coefficients(&v->control,0,hz,&f);
                        f.z1=bank->filter_r[group].z1[sub];
                        f.z2=bank->filter_r[group].z2[sub];
                        store_filter(&bank->filter_r[group],sub,&f);
                    }
                }
                int at=(int)v->pos;
                double fraction=v->pos-at;
                double a_l=v->buf[(size_t)at*2];
                double b_l=at+1<v->smplen?v->buf[(size_t)(at+1)*2]:a_l;
                double a_r=v->buf[(size_t)at*2+(v->channels==2?1:0)];
                double b_r=at+1<v->smplen?
                    v->buf[(size_t)(at+1)*2+(v->channels==2?1:0)]:a_r;
                double left=a_l+(b_l-a_l)*fraction;
                double right=a_r+(b_r-a_r)*fraction;
                input_l[sub]=left*v->amp*env;
                input_r[sub]=right*v->amp*env;
                if (v->channels==2) stereo=1;
                v->pos+=v->step; v->age+=1.0/SR; v->tick++;
                if (v->pos<0.0 || v->pos>=v->smplen) v->active=0;
            }
            dsp_biquad4_process(&bank->filter_l[group],input_l,result_l,1);
            if (stereo)
                dsp_biquad4_process(&bank->filter_r[group],input_r,result_r,1);
            else
                memcpy(result_r,result_l,sizeof(result_r));
            for (int sub=0;sub<4;sub++) {
                int index=group*4+sub;
                if (!bank->lanes[index].active && input_l[sub]==0 && input_r[sub]==0)
                    continue;
                out_l[frame*SOA_VOICE_LANES+index]=result_l[sub];
                out_r[frame*SOA_VOICE_LANES+index]=result_r[sub];
            }
        }
    }
}
