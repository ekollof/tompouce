/* Generated IR follows superdough 1.2.6 reverbGen.mjs,
   Copyright 2014 Alan deLespinasse, Apache-2.0; Strudel's wrapper is
   AGPL-3.0-or-later. See thirdparty/native-synth for notices. */
#include "convolution.h"
#include "common.h"
#include "prep_profile.h"
#include <stdint.h>

/* Process-wide generated-room random stream; see convolution_prepare_reverb.
   Generation misses are serialized by the room lane, so unlocked stream
   steps below never interleave; the locked lookup/hit path is atomic. */
static uint32_t reverb_stream=0xc0ffeeu;

const ConvKernel *convolution_prepare_reverb(double size,double fade,double lp,double dim) {
    enum { CACHE=16 };
    static struct { double size,fade,lp,dim; ConvKernel *kernel; int reserved; } cache[CACHE];
    static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
    /* The web regenerates rooms with fresh randomness per call and keeps no
       cache; comparison pages seed Math.random with this same LCG. Native
       continues one process-wide stream across generations so rooms vary per
       call deterministically instead of reseeding to identical kernels.
       Cache hits advance past the segment a regeneration would have consumed.
       Renderers start every case from the seed; tests reset explicitly.
       Exact tail parity stays out of reach: production randomness is
       unknowable (one chain-ID draw per trigger plus fresh room draws), and
       the eager comparison renderer bounds per-hap capture, so sequences
       stay diagnostic. */
    if (!isfinite(size) || !isfinite(fade) || !isfinite(lp) || !isfinite(dim) || size<=0 || size>10 ||
        fade<0 || fade>10 || lp<0 || lp>SR/2 || dim<0 || dim>SR/2) return NULL;
    long frames=lround(size*1.5*SR), decay=lround(size*SR), fade_frames=lround(fade*SR);
    if (frames<1 || frames>CONV_PARTS*CONV_BLOCK || decay<1) return NULL;
    prep_lock(&lock, PREP_LOCK_REVERB);
    for (unsigned i=0;i<CACHE;i++) if (cache[i].kernel && cache[i].size==size && cache[i].fade==fade &&
        cache[i].lp==lp && cache[i].dim==dim) {
        const ConvKernel *result=cache[i].kernel;
        for (long skip=0;skip<2*frames;skip++) reverb_stream=reverb_stream*1664525u+1013904223u;
        prep_unlock(&lock, PREP_LOCK_REVERB); return result;
    }
    int slot=-1;
    for (unsigned i=0;i<CACHE;i++) if (!cache[i].kernel && !cache[i].reserved) { slot=(int)i; break; }
    if (slot<0) { prep_unlock(&lock, PREP_LOCK_REVERB); return NULL; }
    cache[slot].reserved=1;
    prep_unlock(&lock, PREP_LOCK_REVERB);
    float *impulse=calloc((size_t)frames*2,sizeof(*impulse));
    if (!impulse) {
        prep_lock(&lock, PREP_LOCK_REVERB);
        cache[slot].reserved=0;
        prep_unlock(&lock, PREP_LOCK_REVERB);
        return NULL;
    }
    /* Continuing stream: the web consumes fresh randomness per call. */
    double decay_base=pow(.001,1.0/decay);
    prep_work_begin(PREP_REVERB);
    for (int ch=0;ch<2;ch++) for (long i=0;i<frames;i++) {
        reverb_stream=reverb_stream*1664525u+1013904223u;
        double value=(reverb_stream/2147483648.0-1)*pow(decay_base,(double)i);
        if (i<fade_frames) value*=i/(double)fade_frames;
        impulse[(size_t)i*2+ch]=(float)value;
    }
    if (lp>0) {
        double x1[2]={0},x2[2]={0},y1[2]={0},y2[2]={0};
        for (long i=0;i<frames;i++) {
            /* WebAudio low-pass Q is in dB: the source sets 0.0001dB. */
            double frequency=lp+(dim-lp)*fmin(1,i/(size*SR));
            double omega=2*M_PI*frequency/SR, cosine=cos(omega), alpha=sin(omega)/(2*pow(10,.0001/20));
            double b0=(1-cosine)*.5/(1+alpha), b1=2*b0, a1=-2*cosine/(1+alpha), a2=(1-alpha)/(1+alpha);
            for (int ch=0;ch<2;ch++) {
                double input=impulse[(size_t)i*2+ch], output=b0*input+b1*x1[ch]+b0*x2[ch]-a1*y1[ch]-a2*y2[ch];
                x2[ch]=x1[ch]; x1[ch]=input; y2[ch]=y1[ch]; y1[ch]=output;
                impulse[(size_t)i*2+ch]=(float)output;
            }
        }
    }
    prep_work_end(PREP_REVERB);
    ConvKernel *kernel=convolution_kernel(impulse,(int)frames,2,1); free(impulse);
    prep_lock(&lock, PREP_LOCK_REVERB);
    for (unsigned i=0;i<CACHE;i++) if (cache[i].kernel && cache[i].size==size && cache[i].fade==fade &&
        cache[i].lp==lp && cache[i].dim==dim) {
        const ConvKernel *result=cache[i].kernel;
        cache[slot].reserved=0;
        prep_unlock(&lock, PREP_LOCK_REVERB);
        convolution_kernel_free(kernel);
        return result;
    }
    if (kernel) {
        cache[slot].size=size; cache[slot].fade=fade; cache[slot].lp=lp; cache[slot].dim=dim;
        cache[slot].kernel=kernel; cache[slot].reserved=0;
    } else cache[slot].reserved=0;
    prep_unlock(&lock, PREP_LOCK_REVERB);
    return kernel;
}

void convolution_reverb_reset_stream(void) {
    reverb_stream=0xc0ffeeu;
}
