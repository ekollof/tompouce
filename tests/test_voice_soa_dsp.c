/* Exactness of the four-lane biquad against its portable reference. */
#include "../src/voice_soa_dsp.c"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

static uint32_t random_state=0x6a09e667u;
static double random_value(void) {
    random_state=random_state*1664525u+1013904223u;
    return (double)(random_state>>8)/16777216.0-0.5;
}

#if VOICE_SOA_DSP_HAVE_AVX2
static void compare(unsigned frames) {
    DspBiquad source[4];
    for (unsigned lane=0;lane<4;lane++) {
        source[lane].b0=.1+random_value()*.2;
        source[lane].b1=.05+random_value()*.1;
        source[lane].b2=.01+random_value()*.02;
        source[lane].a1=-.2+random_value()*.1;
        source[lane].a2=.01+random_value()*.01;
        source[lane].z1=random_value()*.01;
        source[lane].z2=random_value()*.01;
    }
    DspBiquad4 a,b;
    dsp_biquad4_init(&a,source); dsp_biquad4_init(&b,source);
    double *input=malloc(frames*4*sizeof(double));
    double *left=malloc(frames*4*sizeof(double));
    double *right=malloc(frames*4*sizeof(double));
    assert(input && left && right);
    for (unsigned i=0;i<frames*4;i++) input[i]=random_value();
    dsp_biquad4_process_c(&a,input,left,frames);
    dsp_biquad4_process_avx2(&b,input,right,frames);
    assert(!memcmp(left,right,frames*4*sizeof(double)));
    assert(!memcmp(&a,&b,sizeof(a)));
    free(input); free(left); free(right);
}

#if VOICE_SOA_DSP_HAVE_AVX2
static void compare2(unsigned frames) {
    DspBiquad source[2];
    for (unsigned lane=0;lane<2;lane++) {
        source[lane].b0=.1+random_value()*.2;
        source[lane].b1=.05+random_value()*.1;
        source[lane].b2=.01+random_value()*.02;
        source[lane].a1=-.2+random_value()*.1;
        source[lane].a2=.01+random_value()*.01;
        source[lane].z1=random_value()*.01;
        source[lane].z2=random_value()*.01;
    }
    DspBiquad2 a,b; dsp_biquad2_init(&a,source); dsp_biquad2_init(&b,source);
    double *input=malloc(frames*2*sizeof(double));
    double *left=malloc(frames*2*sizeof(double));
    double *right=malloc(frames*2*sizeof(double));
    assert(input && left && right);
    for (unsigned i=0;i<frames*2;i++) input[i]=random_value();
    dsp_biquad2_process_c(&a,input,left,frames);
    dsp_biquad2_process_avx2(&b,input,right,frames);
    assert(!memcmp(left,right,frames*2*sizeof(double)));
    assert(!memcmp(&a,&b,sizeof(a)));
    free(input); free(left); free(right);
}
#endif

int main(void) {
    __builtin_cpu_init();
    if (!__builtin_cpu_supports("avx2")) {
        puts("voice SoA DSP skipped (no AVX2)");
        return 0;
    }
    const unsigned sizes[]={1,2,3,7,64,257};
    for (unsigned i=0;i<sizeof(sizes)/sizeof(*sizes);i++) compare(sizes[i]);
    for (unsigned i=0;i<sizeof(sizes)/sizeof(*sizes);i++) compare2(sizes[i]);
    DspBiquad bad[4]={{.b0=NAN,.b1=INFINITY,.b2=1,.a1=0,.a2=0,.z1=0,.z2=0}};
    for (int lane=1;lane<4;lane++) bad[lane]=bad[0];
    DspBiquad4 c,d; dsp_biquad4_init(&c,bad); dsp_biquad4_init(&d,bad);
    double bad_in[8]={NAN,INFINITY,-INFINITY,0,1,2,3,4};
    double bad_l[8],bad_r[8];
    dsp_biquad4_process_c(&c,bad_in,bad_l,2);
    dsp_biquad4_process_avx2(&d,bad_in,bad_r,2);
    assert(!memcmp(bad_l,bad_r,sizeof(bad_l)));
    assert(!memcmp(&c,&d,sizeof(c)));
    enum { BENCH_FRAMES=100000 };
    DspBiquad source[4]={{.b0=.2,.b1=.1,.b2=.01,.a1=-.2,.a2=.01}};
    for (int lane=1;lane<4;lane++) source[lane]=source[0];
    DspBiquad4 a,b; dsp_biquad4_init(&a,source); dsp_biquad4_init(&b,source);
    double *input=malloc(BENCH_FRAMES*4*sizeof(double));
    double *out=malloc(BENCH_FRAMES*4*sizeof(double));
    assert(input && out);
    for (int i=0;i<BENCH_FRAMES*4;i++) input[i]=random_value();
    struct timespec t0,t1,t2;
    clock_gettime(CLOCK_MONOTONIC,&t0);
    dsp_biquad4_process_c(&a,input,out,BENCH_FRAMES);
    clock_gettime(CLOCK_MONOTONIC,&t1);
    dsp_biquad4_process_avx2(&b,input,out,BENCH_FRAMES);
    clock_gettime(CLOCK_MONOTONIC,&t2);
    double scalar=(t1.tv_sec-t0.tv_sec)*1e3+(t1.tv_nsec-t0.tv_nsec)/1e6;
    double vector=(t2.tv_sec-t1.tv_sec)*1e3+(t2.tv_nsec-t1.tv_nsec)/1e6;
    printf("voice SoA biquad: C %.3f ms, AVX2 %.3f ms, %.2fx\n",
        scalar,vector,vector>0?scalar/vector:0);
    enum { BENCH2_FRAMES=200000 };
    DspBiquad source2[2]={{.b0=.2,.b1=.1,.b2=.01,.a1=-.2,.a2=.01}};
    source2[1]=source2[0];
    DspBiquad2 a2,b2; dsp_biquad2_init(&a2,source2); dsp_biquad2_init(&b2,source2);
    double *in2=malloc(BENCH2_FRAMES*2*sizeof(double));
    double *out2=malloc(BENCH2_FRAMES*2*sizeof(double));
    assert(in2 && out2);
    for (int i=0;i<BENCH2_FRAMES*2;i++) in2[i]=random_value();
    struct timespec u0,u1,u2;
    clock_gettime(CLOCK_MONOTONIC,&u0);
    dsp_biquad2_process_c(&a2,in2,out2,BENCH2_FRAMES);
    clock_gettime(CLOCK_MONOTONIC,&u1);
    dsp_biquad2_process_avx2(&b2,in2,out2,BENCH2_FRAMES);
    clock_gettime(CLOCK_MONOTONIC,&u2);
    double c2=(u1.tv_sec-u0.tv_sec)*1e3+(u1.tv_nsec-u0.tv_nsec)/1e6;
    double v2=(u2.tv_sec-u1.tv_sec)*1e3+(u2.tv_nsec-u1.tv_nsec)/1e6;
    printf("stereo biquad: C %.3f ms, AVX2 %.3f ms, %.2fx\n",
        c2,v2,v2>0?c2/v2:0);
    free(in2); free(out2);
    free(input); free(out);
    puts("voice SoA biquad exactness ok");
    return 0;
}
#else
#if VOICE_SOA_DSP_HAVE_AVX2
static void compare2(unsigned frames) {
    DspBiquad source[2];
    for (unsigned lane=0;lane<2;lane++) {
        source[lane].b0=.1+random_value()*.2;
        source[lane].b1=.05+random_value()*.1;
        source[lane].b2=.01+random_value()*.02;
        source[lane].a1=-.2+random_value()*.1;
        source[lane].a2=.01+random_value()*.01;
        source[lane].z1=random_value()*.01;
        source[lane].z2=random_value()*.01;
    }
    DspBiquad2 a,b; dsp_biquad2_init(&a,source); dsp_biquad2_init(&b,source);
    double *input=malloc(frames*2*sizeof(double));
    double *left=malloc(frames*2*sizeof(double));
    double *right=malloc(frames*2*sizeof(double));
    assert(input && left && right);
    for (unsigned i=0;i<frames*2;i++) input[i]=random_value();
    dsp_biquad2_process_c(&a,input,left,frames);
    dsp_biquad2_process_avx2(&b,input,right,frames);
    assert(!memcmp(left,right,frames*2*sizeof(double)));
    assert(!memcmp(&a,&b,sizeof(a)));
    free(input); free(left); free(right);
}
#endif

int main(void) {
    puts("voice SoA DSP unavailable");
    return 0;
}
#endif
