/* Shared phase-vocoder core, moved verbatim out of stretch.c so the
   offline preparation and the live per-voice path use one implementation.
   Original sources: superdough 1.2.6 PhaseVocoderProcessor/OLAProcessor
   (Strudel contributors, AGPL-3.0-or-later), originally olvb/phaze
   (Unlicense); real FFT adapted from fft.js, Fedor Indutny 2017 (MIT). */
#include "vocoder.h"
#include "stretch_fft_simd.h"
#include "vocoder_tails_simd.h"
#include <pthread.h>
#include <math.h>
#include <string.h>

static float voc_hann[VOCODE_FFT];
static VocoderComplex voc_twiddle[VOCODE_FFT/2];
static unsigned voc_reverse[VOCODE_FFT];
static unsigned voc_real_reverse[VOCODE_FFT/2];
static pthread_once_t voc_tables_once = PTHREAD_ONCE_INIT;

static void vocoder_init_tables(void) {
    for (unsigned i=0;i<VOCODE_FFT;i++) {
        voc_hann[i]=(float)(.5*(1-cos(2*M_PI*i/VOCODE_FFT)));
        unsigned x=i, reversed=0;
        for (int bit=0;bit<11;bit++) { reversed=(reversed<<1)|(x&1); x>>=1; }
        voc_reverse[i]=reversed;
        if (i<VOCODE_FFT/2) voc_twiddle[i]=(VocoderComplex){cos(2*M_PI*i/VOCODE_FFT),-sin(2*M_PI*i/VOCODE_FFT)};
        if (i<VOCODE_FFT/2) {
            unsigned reversed_pairs=0;
            for (unsigned shift=0;shift<10;shift+=2) reversed_pairs|=((i>>shift)&3)<<(8-shift);
            voc_real_reverse[i]=reversed_pairs;
        }
    }
}

void vocoder_tables_init(void) {
    pthread_once(&voc_tables_once, vocoder_init_tables);
}

static VocoderComplex vocoder_multiply(VocoderComplex a,VocoderComplex b) {
    return (VocoderComplex){a.re*b.re-a.im*b.im,a.re*b.im+a.im*b.re};
}

void vocoder_real_fft(VocoderComplex *out, const float *input) {
    vocoder_tables_init();
    for (unsigned i=0;i<VOCODE_FFT/2;i++) {
        double a=input[voc_real_reverse[i]],b=input[voc_real_reverse[i]+VOCODE_FFT/2];
        out[i*2]=(VocoderComplex){a+b,0}; out[i*2+1]=(VocoderComplex){a-b,0};
    }
    for (unsigned step=VOCODE_FFT/4;step>=2;step/=4) {
        unsigned length=VOCODE_FFT*2/step,q=length/4;
        for (unsigned base=0;base<VOCODE_FFT;base+=length) for (unsigned j=0,k=0;j<=q/2;j++,k+=step/2) {
            unsigned a=base+j,b=a+q,c=b+q,d=c+q;
            VocoderComplex mb=vocoder_multiply(out[b],voc_twiddle[k]),mc=vocoder_multiply(out[c],voc_twiddle[2*k]),md=vocoder_multiply(out[d],voc_twiddle[3*k]);
            VocoderComplex t0={out[a].re+mc.re,out[a].im+mc.im},t1={out[a].re-mc.re,out[a].im-mc.im};
            VocoderComplex t2={mb.re+md.re,mb.im+md.im},t3={mb.re-md.re,mb.im-md.im};
            out[a]=(VocoderComplex){t0.re+t2.re,t0.im+t2.im}; out[b]=(VocoderComplex){t1.re+t3.im,t1.im-t3.re};
            if (!j) { out[c]=(VocoderComplex){t0.re-t2.re,t0.im-t2.im}; continue; }
            if (j==q/2) continue;
            out[base+q-j]=(VocoderComplex){t1.re-t3.im,-t1.im-t3.re};
            out[base+2*q-j]=(VocoderComplex){t0.re-t2.re,-t0.im+t2.im};
        }
    }
}

void vocoder_fft(VocoderComplex *data, int inverse) {
    vocoder_tables_init();
    stretch_fft((double*)data,inverse,(const double*)voc_twiddle,voc_reverse);
}

void vocoder_transform(const float *input, float *sum, double factor,
    unsigned cursor, VocoderComplex *freq, VocoderComplex *shifted) {
    float magnitudes[VOCODE_FFT/2+1]; int peaks[VOCODE_FFT/2+1]; int count=0;
    float windowed[VOCODE_FFT];
    vocoder_window(windowed,input,voc_hann);
    vocoder_real_fft(freq,windowed); memset(shifted,0,VOCODE_FFT*sizeof(*shifted));
    vocoder_magnitudes(magnitudes,freq);
    for (int i=2;i<VOCODE_FFT/2-1;i++) {
        float mag=magnitudes[i];
        if (magnitudes[i-1]>=mag || magnitudes[i-2]>=mag || magnitudes[i+1]>=mag || magnitudes[i+2]>=mag) continue;
        peaks[count++]=i; i++;
    }
    for (int i=0;i<count;i++) {
        int peak=peaks[i],target=(int)floor(peak*factor+.5);
        if (target>VOCODE_FFT/2+1) break;
        int start=i?peak-(peak-peaks[i-1])/2:0;
        int end=i+1<count?peak+(peaks[i+1]-peak+1)/2:VOCODE_FFT;
        double angle=2*M_PI*(target-peak)*cursor/VOCODE_FFT;
        double re=cos(angle),im=sin(angle);
        for (int bin=start;bin<end;bin++) {
            int dest=target+bin-peak;
            if (dest>VOCODE_FFT/2) break;
            if (dest<0) continue;
            shifted[dest].re+=freq[bin].re*re-freq[bin].im*im;
            shifted[dest].im+=freq[bin].im*re+freq[bin].re*im;
        }
    }
    vocoder_mirror(shifted);
    vocoder_fft(shifted,1);
    vocoder_ola(sum,shifted,voc_hann);
}

/* Streaming per-voice vocoder. The caller stages 128 input samples,
   the transform runs when a block completes, then the caller consumes
   128 output samples; input and output stay on the absolute 128-sample
   grid (cursor = block % 2048, matching the deployed timeCursor which
   starts at zero and steps per quantum), matching the baked offline
   path sample for sample on identical input. State starts zeroed like a
   freshly created worklet node; the first (possibly partial) block is
   zero-padded via the skip count. */
void live_vocoder_init(LiveVocoder *v, long first_block, int skip,
    double mapped_factor, int channels) {
    memset(v, 0, sizeof(*v));
    v->block = first_block;
    v->factor = mapped_factor;
    v->channels = channels > 1 ? 2 : 1;
    v->fill = skip >= 0 && skip < VOCODE_HOP ? skip : 0;
    v->out_pos = 0;
    v->avail = 0;
}

void live_vocoder_set_factor(LiveVocoder *v, double mapped_factor) {
    v->factor = mapped_factor;
}

static void live_vocoder_process(LiveVocoder *v) {
    int channels = v->channels > 1 ? 2 : 1;
    for (int channel = 0; channel < channels; channel++) {
        memmove(v->in[channel], v->in[channel]+VOCODE_HOP,
            (VOCODE_FFT-VOCODE_HOP)*sizeof(float));
        memcpy(v->in[channel]+VOCODE_FFT-VOCODE_HOP, v->staged[channel],
            VOCODE_HOP*sizeof(float));
        unsigned cursor = (unsigned)(((v->block % VOCODE_FFT)+VOCODE_FFT)%VOCODE_FFT);
        vocoder_transform(v->in[channel], v->sum[channel], v->factor,
            cursor, v->freq, v->shifted);
        memcpy(v->out[channel], v->sum[channel], VOCODE_HOP*sizeof(float));
        memmove(v->sum[channel], v->sum[channel]+VOCODE_HOP,
            (VOCODE_FFT-VOCODE_HOP)*sizeof(float));
        memset(v->sum[channel]+VOCODE_FFT-VOCODE_HOP, 0, VOCODE_HOP*sizeof(float));
    }
    if (channels == 1) memcpy(v->out[1], v->out[0], VOCODE_HOP*sizeof(float));
    v->block += VOCODE_HOP;
    v->fill = 0;
    v->out_pos = 0;
    v->avail = VOCODE_HOP;
}

void live_vocoder_push(LiveVocoder *v, float left, float right) {
    if (v->fill < 0 || v->fill >= VOCODE_HOP) return;
    v->staged[0][v->fill] = left;
    v->staged[1][v->fill] = right;
    if (++v->fill >= VOCODE_HOP) live_vocoder_process(v);
}

int live_vocoder_avail(const LiveVocoder *v) {
    return v->avail - v->out_pos;
}

float live_vocoder_out_l(const LiveVocoder *v) {
    return v->out[0][v->out_pos];
}

float live_vocoder_out_r(const LiveVocoder *v) {
    return v->out[1][v->out_pos];
}

void live_vocoder_pop(LiveVocoder *v) {
    if (v->out_pos < v->avail) {
        v->out_pos++;
        if (v->out_pos >= v->avail) {
            v->out_pos = 0;
            v->avail = 0;
        }
    }
}
