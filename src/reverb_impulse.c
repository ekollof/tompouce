/* Generated IR follows superdough 1.2.6 reverbGen.mjs,
   Copyright 2014 Alan deLespinasse, Apache-2.0; Strudel's wrapper is
   AGPL-3.0-or-later. See thirdparty/native-synth for notices. */
#include "convolution.h"
#include "common.h"
#include <stdint.h>

const ConvKernel *convolution_prepare_reverb(double size,double fade,double lp,double dim) {
    enum { CACHE=16 };
    static struct { double size,fade,lp,dim; ConvKernel *kernel; } cache[CACHE];
    static unsigned used;
    static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
    if (!isfinite(size) || !isfinite(fade) || !isfinite(lp) || !isfinite(dim) || size<=0 || size>10 ||
        fade<0 || fade>10 || lp<0 || lp>SR/2 || dim<0 || dim>SR/2) return NULL;
    long frames=lround(size*1.5*SR), decay=lround(size*SR), fade_frames=lround(fade*SR);
    if (frames<1 || frames>CONV_PARTS*CONV_BLOCK || decay<1) return NULL;
    pthread_mutex_lock(&lock);
    for (unsigned i=0;i<used;i++) if (cache[i].size==size && cache[i].fade==fade && cache[i].lp==lp && cache[i].dim==dim) {
        const ConvKernel *result=cache[i].kernel; pthread_mutex_unlock(&lock); return result;
    }
    if (used==CACHE) { pthread_mutex_unlock(&lock); return NULL; }
    float *impulse=calloc((size_t)frames*2,sizeof(*impulse));
    if (!impulse) { pthread_mutex_unlock(&lock); return NULL; }
    /* Reproducible random input; comparison pages seed their generator too. */
    uint32_t random=0xc0ffeeu;
    double decay_base=pow(.001,1.0/decay);
    for (int ch=0;ch<2;ch++) for (long i=0;i<frames;i++) {
        random=random*1664525u+1013904223u;
        double value=(random/2147483648.0-1)*pow(decay_base,(double)i);
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
    ConvKernel *kernel=convolution_kernel(impulse,(int)frames,2,1); free(impulse);
    if (kernel) {
        cache[used].size=size; cache[used].fade=fade; cache[used].lp=lp; cache[used].dim=dim; cache[used++].kernel=kernel;
    }
    pthread_mutex_unlock(&lock); return kernel;
}
