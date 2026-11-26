#include "dsp.h"
#include "sample_interp.h"
#include "envelope_simd.h"
#include "oscillator_simd.h"
#include "reverb_simd.h"
#include "delay_simd.h"
#include "stretch_fft_simd.h"
#include "stretch_real_fft_simd.h"
#include "visual_fft.h"
#include "voice_soa_dsp.h"
#include "js_audio.h"
#include "audio.h"
#include "track.h"
#include "scheduler.h"
#include "banks.h"
#include "sampler.h"
#include <assert.h>
#include <dirent.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

static int source_event(const char *text,AudioEvent *event) {
    cJSON *hap=cJSON_CreateObject(); assert(hap);
    cJSON_AddNumberToObject(hap,"begin",0);cJSON_AddNumberToObject(hap,"end",1);
    cJSON *value=cJSON_Parse(text); assert(value);
    cJSON_AddItemToObject(hap,"value",value);
    int result=js_audio_event(hap,0,1,event);
    if (result) result=js_audio_resolve_sample(event,value);
    cJSON_Delete(hap);return result;
}

static void test_stereo_filter_path(void) {
    DspParams params; dsp_params_init(&params); params.enabled=1;
    params.p[DSP_LPF]=2400; params.p[DSP_LPQ]=1.2;
    DspState base; dsp_init(&base,&params);
    DspState ref_l=base,ref_r=base,l=base,r=base;
    double age=0;
    for (int i=0;i<4096;i++) {
        double xl=sin(i*.017)*.3, xr=cos(i*.019)*.2;
        double want_l=dsp_process(&ref_l,xl,age,1);
        double want_r=dsp_process(&ref_r,xr,age,1);
        double got_l,got_r;
        assert(dsp_process_stereo_filter(&l,&r,xl,xr,age,1,&got_l,&got_r));
        if (memcmp(&want_l,&got_l,sizeof(want_l)) || memcmp(&want_r,&got_r,sizeof(got_r))) {
            fprintf(stderr,"stereo filter diff frame=%d got=%.17g/%.17g want=%.17g/%.17g\n",
                i,got_l,got_r,want_l,want_r);
            assert(0 && "stereo filter mismatch");
        }
        age+=1.0/SR;
    }
    enum { N=500000 };
    DspState al=base,ar=base,bl=base,br=base;
    double sink=0; volatile double keep=0;
    struct timespec t0,t1,t2;
    clock_gettime(CLOCK_MONOTONIC,&t0);
    for (int i=0;i<N;i++) {
        double xl=sin(i*.017)*.3, xr=cos(i*.019)*.2;
        sink+=dsp_process(&al,xl,i/SR,1)+dsp_process(&ar,xr,i/SR,1);
    }
    clock_gettime(CLOCK_MONOTONIC,&t1);
    for (int i=0;i<N;i++) {
        double xl=sin(i*.017)*.3, xr=cos(i*.019)*.2,yl,yr;
        dsp_process_stereo_filter(&bl,&br,xl,xr,i/SR,1,&yl,&yr);
        sink+=yl+yr;
    }
    clock_gettime(CLOCK_MONOTONIC,&t2);
    keep=sink; (void)keep;
    double c=(t1.tv_sec-t0.tv_sec)*1e3+(t1.tv_nsec-t0.tv_nsec)/1e6;
    double v=(t2.tv_sec-t1.tv_sec)*1e3+(t2.tv_nsec-t1.tv_nsec)/1e6;
    printf("stereo filter path: scalar %.3f ms, SIMD %.3f ms (%.2fx)\n",c,v,c>0?v/c:0);
}

static void test_stereo_filter_persistent(void) {
    DspParams params; dsp_params_init(&params); params.enabled=1;
    params.p[DSP_LPF]=2400; params.p[DSP_LPQ]=1.2;
    DspState base; dsp_init(&base,&params);
    DspState ref_l=base,ref_r=base,l=base,r=base;
    DspBiquad pair[2]={base.filter[0],base.filter[0]};
    DspBiquad2 state; dsp_biquad2_init(&state,pair);
    double age=0;
    for (int i=0;i<4096;i++) {
        double xl=sin(i*.017)*.3, xr=cos(i*.019)*.2;
        double want_l=dsp_process(&ref_l,xl,age,1);
        double want_r=dsp_process(&ref_r,xr,age,1);
        double got_l,got_r;
        assert(dsp_stereo_filter_step(&l,&r,&state,0,xl,xr,age,1,&got_l,&got_r));
        assert(!memcmp(&want_l,&got_l,sizeof(want_l)) &&
            !memcmp(&want_r,&got_r,sizeof(want_r)));
        age+=1.0/SR;
    }
    enum { N=500000 };
    DspState al=base,ar=base,bl=base,br=base;
    DspBiquad bpair[2]={base.filter[0],base.filter[0]};
    DspBiquad2 bstate; dsp_biquad2_init(&bstate,bpair);
    double sink=0; volatile double keep=0;
    struct timespec t0,t1,t2;
    clock_gettime(CLOCK_MONOTONIC,&t0);
    for (int i=0;i<N;i++) {
        double xl=sin(i*.017)*.3, xr=cos(i*.019)*.2;
        sink+=dsp_process(&al,xl,i/SR,1)+dsp_process(&ar,xr,i/SR,1);
    }
    clock_gettime(CLOCK_MONOTONIC,&t1);
    for (int i=0;i<N;i++) {
        double xl=sin(i*.017)*.3, xr=cos(i*.019)*.2,yl,yr;
        dsp_stereo_filter_step(&bl,&br,&bstate,0,xl,xr,i/SR,1,&yl,&yr);
        sink+=yl+yr;
    }
    clock_gettime(CLOCK_MONOTONIC,&t2);
    keep=sink; (void)keep;
    double c=(t1.tv_sec-t0.tv_sec)*1e3+(t1.tv_nsec-t0.tv_nsec)/1e6;
    double v=(t2.tv_sec-t1.tv_sec)*1e3+(t2.tv_nsec-t1.tv_nsec)/1e6;
    printf("persistent stereo filter: scalar %.3f ms, SIMD %.3f ms (%.2fx)\n",c,v,v>0?c/v:0);
}

static void test_distortion_site_formula(void) {
    DspParams params; dsp_params_init(&params); params.enabled=1;
    params.p[DSP_DISTORT]=2;
    DspState state; dsp_init(&state,&params);
    double output=dsp_process(&state,.12,0,1);
    double shape=expm1(2);
    double expected=((1+shape)*.12/(1+shape*.12));
    assert(fabs(output-expected)<1e-12);
}

static void test_parameter_lookup(void) {
    int index=-1; double minimum=0,maximum=0;
    assert(dsp_parameter_lookup("loopBegin",&index,&minimum,&maximum));
    assert(index==DSP_LOOPBEGIN && minimum==0 && maximum==1);
    assert(dsp_parameter_lookup("unison",&index,&minimum,&maximum));
    assert(index==DSP_UNISON && minimum==1 && maximum==DSP_SUPERSAW_VOICES);
    assert(!dsp_parameter_lookup("not-a-control",&index,&minimum,&maximum));
    assert(!dsp_parameter_lookup(NULL,&index,&minimum,&maximum));
}

static void test_envelope4(void) {
    double age[4], gate[4], attack[4], decay[4], sustain[4], release[4];
    double scalar[4], lane[4];
    unsigned state=0x12345678u;
    for (int iteration=0;iteration<10000;iteration++) {
        for (int i=0;i<4;i++) {
            state=state*1664525u+1013904223u;
            age[i]=(state>>8)/16777216.0*2.0;
            state=state*1664525u+1013904223u;
            gate[i]=(state>>8)/16777216.0*1.5;
            state=state*1664525u+1013904223u;
            attack[i]=(state>>16)/65536.0*.3;
            state=state*1664525u+1013904223u;
            decay[i]=(state>>16)/65536.0*.3;
            state=state*1664525u+1013904223u;
            sustain[i]=(state>>8)/16777216.0;
            state=state*1664525u+1013904223u;
            release[i]=(state>>16)/65536.0*.5;
        }
        dsp_envelope4_c(age,gate,attack,decay,sustain,release,scalar);
#if defined(__x86_64__) || defined(__i386__)
        __builtin_cpu_init();
        if (__builtin_cpu_supports("avx2")) {
            dsp_envelope4_avx2(age,gate,attack,decay,sustain,release,lane);
            for (int i=0;i<4;i++)
                assert(memcmp(&scalar[i],&lane[i],sizeof(scalar[i]))==0);
        }
#else
        (void)lane;
#endif
    }
#if defined(__x86_64__) || defined(__i386__)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2")) {
        enum { N=2000000 };
        volatile double sink=0;
        struct timespec t0,t1,t2;
        clock_gettime(CLOCK_MONOTONIC,&t0);
        for (int i=0;i<N;i++) {
            double sum=0;
            for (int lane_index=0;lane_index<4;lane_index++)
                sum+=dsp_envelope((i&1023)/44100.0,.01,.002,.05,.6,.05);
            sink+=sum;
        }
        clock_gettime(CLOCK_MONOTONIC,&t1);
        for (int i=0;i<N;i++) {
            double a[4]={(i&1023)/44100.0,(i&1023)/44100.0,
                (i&1023)/44100.0,(i&1023)/44100.0};
            double g[4]={.01,.01,.01,.01}, at[4]={.002,.002,.002,.002};
            double de[4]={.05,.05,.05,.05}, su[4]={.6,.6,.6,.6};
            double re[4]={.05,.05,.05,.05}, out[4];
            dsp_envelope4_avx2(a,g,at,de,su,re,out);
            sink+=out[0]+out[1]+out[2]+out[3];
        }
        clock_gettime(CLOCK_MONOTONIC,&t2);
        (void)sink;
        double scalar_ms=(t1.tv_sec-t0.tv_sec)*1e3+
            (t1.tv_nsec-t0.tv_nsec)/1e6;
        double lane_ms=(t2.tv_sec-t1.tv_sec)*1e3+
            (t2.tv_nsec-t1.tv_nsec)/1e6;
        printf("four-lane envelope: C %.3f ms, AVX2 %.3f ms (%.2fx)\n",
            scalar_ms,lane_ms,lane_ms>0?scalar_ms/lane_ms:0);
    }
#endif
}


static void test_saw4(void) {
    double phase[4], freq[4], scalar[4], lane[4];
    unsigned state=0x9e3779b9u;
    for (int iteration=0;iteration<10000;iteration++) {
        for (int i=0;i<4;i++) {
            state=state*1664525u+1013904223u;
            phase[i]=(state>>8)/16777216.0*2*M_PI;
            state=state*1664525u+1013904223u;
            freq[i]=20+(state>>8)/16777216.0*20000;
        }
        dsp_saw4_c(phase,freq,scalar);
#if defined(__x86_64__) || defined(__i386__)
        __builtin_cpu_init();
        if (__builtin_cpu_supports("avx2")) {
            dsp_saw4_avx2(phase,freq,lane);
            for (int i=0;i<4;i++) {
                if (memcmp(&scalar[i],&lane[i],sizeof(scalar[i]))) {
                    fprintf(stderr,"saw mismatch i=%d phase=%.17g freq=%.17g C=%.17g AVX=%.17g\n",
                        i,phase[i],freq[i],scalar[i],lane[i]);
                }
                assert(memcmp(&scalar[i],&lane[i],sizeof(scalar[i]))==0);
            }
        }
#else
        (void)lane;
#endif
    }
#if defined(__x86_64__) || defined(__i386__)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2")) {
        enum { N=2000000 };
        volatile double sink=0;
        struct timespec t0,t1,t2;
        double p[4]={.1,.4,1.2,2.6}, f[4]={220,330,440,880}, out[4];
        clock_gettime(CLOCK_MONOTONIC,&t0);
        for (int i=0;i<N;i++) {
            for (int k=0;k<4;k++) sink+=dsp_oscillator(0,p[k],f[k],.5);
        }
        clock_gettime(CLOCK_MONOTONIC,&t1);
        for (int i=0;i<N;i++) {
            dsp_saw4_avx2(p,f,out);
            sink+=out[0]+out[1]+out[2]+out[3];
        }
        clock_gettime(CLOCK_MONOTONIC,&t2);
        (void)sink;
        double scalar_ms=(t1.tv_sec-t0.tv_sec)*1e3+
            (t1.tv_nsec-t0.tv_nsec)/1e6;
        double lane_ms=(t2.tv_sec-t1.tv_sec)*1e3+
            (t2.tv_nsec-t1.tv_nsec)/1e6;
        printf("four-lane saw: C %.3f ms, AVX2 %.3f ms (%.2fx)\n",
            scalar_ms,lane_ms,lane_ms>0?scalar_ms/lane_ms:0);
    }
#endif
}

static void test_square4(void) {
    double phase[4], freq[4], width[4], scalar[4], lane[4];
    unsigned state=0x85ebca6bu;
    for (int iteration=0;iteration<10000;iteration++) {
        for (int i=0;i<4;i++) {
            state=state*1664525u+1013904223u;
            phase[i]=(state>>8)/16777216.0*2*M_PI;
            state=state*1664525u+1013904223u;
            freq[i]=20+(state>>8)/16777216.0*20000;
            state=state*1664525u+1013904223u;
            width[i]=(state>>8)/16777216.0;
        }
        dsp_square4_c(phase,freq,width,scalar);
#if defined(__x86_64__) || defined(__i386__)
        __builtin_cpu_init();
        if (__builtin_cpu_supports("avx2")) {
            dsp_square4_avx2(phase,freq,width,lane);
            for (int i=0;i<4;i++) {
                if (memcmp(&scalar[i],&lane[i],sizeof(scalar[i]))) {
                    fprintf(stderr,"square mismatch i=%d phase=%.17g freq=%.17g width=%.17g C=%.17g AVX=%.17g\n",
                        i,phase[i],freq[i],width[i],scalar[i],lane[i]);
                }
                assert(memcmp(&scalar[i],&lane[i],sizeof(scalar[i]))==0);
            }
        }
#else
        (void)lane;
#endif
    }
#if defined(__x86_64__) || defined(__i386__)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2")) {
        enum { N=2000000 };
        volatile double sink=0;
        struct timespec t0,t1,t2;
        double p[4]={.1,.4,1.2,2.6}, f[4]={220,330,440,880};
        double w[4]={.25,.5,.75,.4}, out[4];
        clock_gettime(CLOCK_MONOTONIC,&t0);
        for (int i=0;i<N;i++) {
            for (int k=0;k<4;k++) sink+=dsp_oscillator(1,p[k],f[k],w[k]);
        }
        clock_gettime(CLOCK_MONOTONIC,&t1);
        for (int i=0;i<N;i++) {
            dsp_square4_avx2(p,f,w,out);
            sink+=out[0]+out[1]+out[2]+out[3];
        }
        clock_gettime(CLOCK_MONOTONIC,&t2);
        (void)sink;
        double scalar_ms=(t1.tv_sec-t0.tv_sec)*1e3+
            (t1.tv_nsec-t0.tv_nsec)/1e6;
        double lane_ms=(t2.tv_sec-t1.tv_sec)*1e3+
            (t2.tv_nsec-t1.tv_nsec)/1e6;
        printf("four-lane square: C %.3f ms, AVX2 %.3f ms (%.2fx)\n",
            scalar_ms,lane_ms,lane_ms>0?scalar_ms/lane_ms:0);
    }
#endif
}

static void test_comb4(void) {
#if defined(__x86_64__) || defined(__i386__)
    enum { LEN=16384, N=2000000 };
    static float lines_c[4][LEN], lines_v[4][LEN];
    float *pc[4]={lines_c[0],lines_c[1],lines_c[2],lines_c[3]};
    float *pv[4]={lines_v[0],lines_v[1],lines_v[2],lines_v[3]};
    unsigned read[4]={0}, write=0;
    unsigned state=0x27d4eb2fu;
    for (int k=0;k<4;k++)
        for (int i=0;i<LEN;i++) {
            state=state*1664525u+1013904223u;
            lines_c[k][i]=lines_v[k][i]=(float)((int)(state>>8)/16777216.0-.5);
        }
    double scalar[4], lane[4];
    for (int iteration=0;iteration<10000;iteration++) {
        for (int k=0;k<4;k++) read[k]=(write+1116u*(unsigned)(k+1))%(unsigned)LEN;
        dsp_comb4_c(pc,read,write,.03125,.84,scalar);
        dsp_comb4_avx2(pv,read,write,.03125,.84,lane);
        for (int k=0;k<4;k++) {
            assert(memcmp(&lines_c[k][write],&lines_v[k][write],
                sizeof(float))==0);
            assert(memcmp(&scalar[k],&lane[k],sizeof(double))==0);
        }
        if (++write>=(unsigned)LEN) write=0;
    }
#if defined(__x86_64__) || defined(__i386__)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2")) {
        volatile double sink=0;
        struct timespec t0,t1,t2;
        clock_gettime(CLOCK_MONOTONIC,&t0);
        for (int i=0;i<N;i++) {
            for (int k=0;k<4;k++) read[k]=(write+1116u*(unsigned)(k+1))%(unsigned)LEN;
            dsp_comb4_c(pc,read,write,.03125,.84,scalar);
            sink+=scalar[0];
            if (++write>=(unsigned)LEN) write=0;
        }
        clock_gettime(CLOCK_MONOTONIC,&t1);
        for (int i=0;i<N;i++) {
            for (int k=0;k<4;k++) read[k]=(write+1116u*(unsigned)(k+1))%(unsigned)LEN;
            dsp_comb4_avx2(pv,read,write,.03125,.84,lane);
            sink+=lane[0];
            if (++write>=(unsigned)LEN) write=0;
        }
        clock_gettime(CLOCK_MONOTONIC,&t2);
        (void)sink;
        double c_ms=(t1.tv_sec-t0.tv_sec)*1e3+
            (t1.tv_nsec-t0.tv_nsec)/1e6;
        double v_ms=(t2.tv_sec-t1.tv_sec)*1e3+
            (t2.tv_nsec-t1.tv_nsec)/1e6;
        printf("four-comb reverb: C %.3f ms, AVX2 %.3f ms (%.2fx)\n",
            c_ms,v_ms,v_ms>0?c_ms/v_ms:0);
    }
#endif
#endif
}

static void delay2_separate_c(float *line, float *quantum, unsigned write,
    unsigned read, unsigned previous, unsigned feedback_pos, int interpolate,
    float fraction, double input_l, double input_r, double feedback,
    double *out_l, double *out_r) {
    enum { LEN=16384 };
    float *q=quantum+2*feedback_pos;
    if (interpolate) {
        line[write]=(float)(input_l+q[0]);
        line[LEN+write]=(float)(input_r+q[1]);
    }
    float l=line[read], r=line[LEN+read];
    if (interpolate) {
        float keep=1.0f-fraction;
        l=l*keep+line[previous]*fraction;
        r=r*keep+line[LEN+previous]*fraction;
        q[0]=(float)(l*feedback);
        q[1]=(float)(r*feedback);
    } else {
        q[0]=q[1]=0;
        line[write]=(float)(input_l+l*feedback);
        line[LEN+write]=(float)(input_r+r*feedback);
    }
    *out_l=l; *out_r=r;
}

static void test_delay2(void) {
#if defined(__SSE2__)
    enum { LEN=16384, N=1000000 };
    static float line_c[2*LEN], line_v[2*LEN], line_s[2*LEN];
    static float quantum_c[256], quantum_v[256], quantum_s[256];
    unsigned state=0x165667b1u;
    for (int i=0;i<2*LEN;i++) {
        state=state*1664525u+1013904223u;
        line_c[i]=line_v[i]=(float)((int)(state>>8)/16777216.0-.5);
    }
    for (int i=0;i<LEN;i++) {
        line_s[i]=line_c[2*i];
        line_s[LEN+i]=line_c[2*i+1];
    }
    for (int i=0;i<256;i++) {
        state=state*1664525u+1013904223u;
        quantum_c[i]=quantum_v[i]=quantum_s[i]=
            (float)((int)(state>>8)/16777216.0-.5);
    }
    unsigned write=0, feedback_pos=0;
    const unsigned dl=64;
    double c_l,c_r,v_l,v_r,s_l,s_r;
    for (int mode=0;mode<2;mode++) {
        for (int iteration=0;iteration<5000;iteration++) {
            unsigned read=(write+LEN-dl)%LEN;
            unsigned previous=read?read-1:LEN-1;
            dsp_delay2_c(line_c,quantum_c,write,read,previous,feedback_pos,
                mode,.3,.03125,-.015625,.5,&c_l,&c_r);
            dsp_delay2_sse2(line_v,quantum_v,write,read,previous,feedback_pos,
                mode,.3,.03125,-.015625,.5,&v_l,&v_r);
            delay2_separate_c(line_s,quantum_s,write,read,previous,
                feedback_pos,mode,.3,.03125,-.015625,.5,&s_l,&s_r);
            assert(memcmp(&line_c[2*write],&line_v[2*write],
                2*sizeof(float))==0);
            assert(memcmp(quantum_c+2*feedback_pos,
                quantum_v+2*feedback_pos,2*sizeof(float))==0);
            assert(memcmp(&c_l,&v_l,sizeof(c_l))==0);
            assert(memcmp(&c_r,&v_r,sizeof(c_r))==0);
            assert(memcmp(&line_c[2*write],&line_s[write],
                sizeof(float))==0);
            assert(memcmp(&line_c[2*write+1],&line_s[LEN+write],
                sizeof(float))==0);
            assert(memcmp(quantum_c+2*feedback_pos,
                quantum_s+2*feedback_pos,2*sizeof(float))==0);
            assert(memcmp(&c_l,&s_l,sizeof(c_l))==0);
            assert(memcmp(&c_r,&s_r,sizeof(c_r))==0);
            if (++write>=LEN) write=0;
            feedback_pos=(feedback_pos+1)&127;
        }
        assert(memcmp(line_c,line_v,sizeof(line_c))==0);
        assert(memcmp(quantum_c,quantum_v,sizeof(quantum_c))==0);
    }
    {
        volatile double sink=0;
        struct timespec t0,t1,t2,t3;
        clock_gettime(CLOCK_MONOTONIC,&t0);
        for (int i=0;i<N;i++) {
            unsigned read=(write+LEN-dl)%LEN;
            unsigned previous=read?read-1:LEN-1;
            delay2_separate_c(line_s,quantum_s,write,read,previous,
                feedback_pos,1,.3,.03125,-.015625,.5,&s_l,&s_r);
            sink+=s_l+s_r;
            if (++write>=LEN) write=0;
            feedback_pos=(feedback_pos+1)&127;
        }
        clock_gettime(CLOCK_MONOTONIC,&t1);
        for (int i=0;i<N;i++) {
            unsigned read=(write+LEN-dl)%LEN;
            unsigned previous=read?read-1:LEN-1;
            dsp_delay2_c(line_c,quantum_c,write,read,previous,feedback_pos,
                1,.3,.03125,-.015625,.5,&c_l,&c_r);
            sink+=c_l+c_r;
            if (++write>=LEN) write=0;
            feedback_pos=(feedback_pos+1)&127;
        }
        clock_gettime(CLOCK_MONOTONIC,&t2);
        for (int i=0;i<N;i++) {
            unsigned read=(write+LEN-dl)%LEN;
            unsigned previous=read?read-1:LEN-1;
            dsp_delay2_sse2(line_v,quantum_v,write,read,previous,feedback_pos,
                1,.3,.03125,-.015625,.5,&v_l,&v_r);
            sink+=v_l+v_r;
            if (++write>=LEN) write=0;
            feedback_pos=(feedback_pos+1)&127;
        }
        clock_gettime(CLOCK_MONOTONIC,&t3);
        (void)sink;
        double s_ms=(t1.tv_sec-t0.tv_sec)*1e3+
            (t1.tv_nsec-t0.tv_nsec)/1e6;
        double c_ms=(t2.tv_sec-t1.tv_sec)*1e3+
            (t2.tv_nsec-t1.tv_nsec)/1e6;
        double v_ms=(t3.tv_sec-t2.tv_sec)*1e3+
            (t3.tv_nsec-t2.tv_nsec)/1e6;
        printf("stereo delay: separate C %.3f ms, interleaved C %.3f ms, "
            "SSE2 %.3f ms\n",s_ms,c_ms,v_ms);
    }
#endif
}

static void test_stretch_fft(void) {
#if defined(__x86_64__) || defined(__i386__)
    enum { SIZE=STRETCH_FFT_SIZE, N=2000 };
    static double twiddle[SIZE];
    static unsigned reverse_bits[SIZE];
    static double data_c[2*SIZE], data_v[2*SIZE];
    for (unsigned i=0;i<SIZE;i++) {
        unsigned x=i, reversed=0;
        for (int bit=0;bit<11;bit++) { reversed=(reversed<<1)|(x&1); x>>=1; }
        reverse_bits[i]=reversed;
        if (i<SIZE/2) {
            twiddle[2*i]=cos(2*M_PI*i/SIZE);
            twiddle[2*i+1]=-sin(2*M_PI*i/SIZE);
        }
    }
    unsigned state=0x9e3779b9u;
    for (int inverse=0;inverse<2;inverse++)
        for (int iteration=0;iteration<20;iteration++) {
            for (int i=0;i<2*SIZE;i++) {
                state=state*1664525u+1013904223u;
                data_c[i]=data_v[i]=(int)(state>>8)/16777216.0-.5;
            }
            stretch_fft_c(data_c,inverse,twiddle,reverse_bits);
            stretch_fft_avx2(data_v,inverse,twiddle,reverse_bits);
            assert(memcmp(data_c,data_v,sizeof(data_c))==0);
        }
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2")) {
        volatile double sink=0;
        struct timespec t0,t1,t2;
        for (int i=0;i<2*SIZE;i++) {
            state=state*1664525u+1013904223u;
            data_c[i]=data_v[i]=(int)(state>>8)/16777216.0-.5;
        }
        clock_gettime(CLOCK_MONOTONIC,&t0);
        for (int i=0;i<N;i++) {
            stretch_fft_c(data_c,0,twiddle,reverse_bits);
            stretch_fft_c(data_c,1,twiddle,reverse_bits);
            sink+=data_c[0];
        }
        clock_gettime(CLOCK_MONOTONIC,&t1);
        for (int i=0;i<N;i++) {
            stretch_fft_avx2(data_v,0,twiddle,reverse_bits);
            stretch_fft_avx2(data_v,1,twiddle,reverse_bits);
            sink+=data_v[0];
        }
        clock_gettime(CLOCK_MONOTONIC,&t2);
        (void)sink;
        double c_ms=(t1.tv_sec-t0.tv_sec)*1e3+
            (t1.tv_nsec-t0.tv_nsec)/1e6;
        double v_ms=(t2.tv_sec-t1.tv_sec)*1e3+
            (t2.tv_nsec-t1.tv_nsec)/1e6;
        printf("2048-point FFT pairs: C %.3f ms, AVX2 %.3f ms (%.2fx)\n",
            c_ms,v_ms,v_ms>0?c_ms/v_ms:0);
    }
#endif
}

static void visual_fft_legacy(double *re, double *imag, int n) {
    for (int i=1,j=0;i<n;i++) {
        int bit=n>>1;
        for (;j&bit;bit>>=1) j^=bit;
        j^=bit;
        if (i<j) { double value=re[i]; re[i]=re[j]; re[j]=value; }
    }
    for (int length=2;length<=n;length*=2)
        for (int start=0;start<n;start+=length)
            for (int j=0;j<length/2;j++) {
                double angle=-6.2831853071795864769*j/length;
                double c=cos(angle), s=sin(angle);
                int u=start+j, v=u+length/2;
                double r=c*re[v]-s*imag[v];
                double q=s*re[v]+c*imag[v];
                re[v]=re[u]-r;
                imag[v]=imag[u]-q;
                re[u]+=r;
                imag[u]+=q;
            }
}

static void test_visual_fft(void) {
#if defined(__x86_64__) || defined(__i386__)
    enum { SIZE=4096, N=500 };
    static double legacy_re[SIZE], legacy_im[SIZE];
    static double c_re[SIZE], c_im[SIZE];
    static double v_re[SIZE], v_im[SIZE];
    unsigned state=0xd1b54a35u;
    for (int iteration=0;iteration<20;iteration++) {
        for (int i=0;i<SIZE;i++) {
            state=state*1664525u+1013904223u;
            legacy_re[i]=c_re[i]=v_re[i]=(int)(state>>8)/16777216.0-.5;
            legacy_im[i]=c_im[i]=v_im[i]=0;
        }
        visual_fft_legacy(legacy_re,legacy_im,SIZE);
        visual_fft_c(c_re,c_im,SIZE);
        visual_fft_avx2(v_re,v_im,SIZE);
        assert(memcmp(legacy_re,c_re,sizeof(legacy_re))==0);
        assert(memcmp(legacy_im,c_im,sizeof(legacy_im))==0);
        assert(memcmp(legacy_re,v_re,sizeof(legacy_re))==0);
        assert(memcmp(legacy_im,v_im,sizeof(legacy_im))==0);
    }
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2")) {
        volatile double sink=0;
        struct timespec t0,t1,t2,t3;
        for (int i=0;i<SIZE;i++) {
            state=state*1664525u+1013904223u;
            legacy_re[i]=c_re[i]=v_re[i]=(int)(state>>8)/16777216.0-.5;
            legacy_im[i]=c_im[i]=v_im[i]=0;
        }
        clock_gettime(CLOCK_MONOTONIC,&t0);
        for (int i=0;i<N;i++) {
            visual_fft_legacy(legacy_re,legacy_im,SIZE);
            sink+=legacy_re[0];
        }
        clock_gettime(CLOCK_MONOTONIC,&t1);
        for (int i=0;i<N;i++) {
            visual_fft_c(c_re,c_im,SIZE);
            sink+=c_re[0];
        }
        clock_gettime(CLOCK_MONOTONIC,&t2);
        for (int i=0;i<N;i++) {
            visual_fft_avx2(v_re,v_im,SIZE);
            sink+=v_re[0];
        }
        clock_gettime(CLOCK_MONOTONIC,&t3);
        (void)sink;
        double legacy_ms=(t1.tv_sec-t0.tv_sec)*1e3+
            (t1.tv_nsec-t0.tv_nsec)/1e6;
        double c_ms=(t2.tv_sec-t1.tv_sec)*1e3+
            (t2.tv_nsec-t1.tv_nsec)/1e6;
        double v_ms=(t3.tv_sec-t2.tv_sec)*1e3+
            (t3.tv_nsec-t2.tv_nsec)/1e6;
        printf("visual 4096 FFT: legacy %.3f ms, table C %.3f ms, "
            "AVX2 %.3f ms\n",legacy_ms,c_ms,v_ms);
    }
#endif
}

typedef struct { double re, im; } TestComplex;
static TestComplex test_complex_mul(TestComplex a, TestComplex b) {
    return (TestComplex){a.re*b.re-a.im*b.im,a.re*b.im+a.im*b.re};
}
static void stretch_real_fft_legacy(TestComplex *out, const float *input,
                                    const TestComplex *twiddle,
                                    const unsigned *real_reverse) {
    enum { SIZE=STRETCH_REAL_FFT_SIZE };
    for (unsigned i=0;i<SIZE/2;i++) {
        double a=input[real_reverse[i]], b=input[real_reverse[i]+SIZE/2];
        out[i*2]=(TestComplex){a+b,0};
        out[i*2+1]=(TestComplex){a-b,0};
    }
    for (unsigned step=SIZE/4;step>=2;step/=4) {
        unsigned length=SIZE*2/step, q=length/4;
        for (unsigned base=0;base<SIZE;base+=length)
            for (unsigned j=0,k=0;j<=q/2;j++,k+=step/2) {
                unsigned a=base+j, b=a+q, c=b+q, d=c+q;
                TestComplex ma=out[a];
                TestComplex mb=test_complex_mul(out[b],twiddle[k]);
                TestComplex mc=test_complex_mul(out[c],twiddle[2*k]);
                TestComplex md=test_complex_mul(out[d],twiddle[3*k]);
                TestComplex t0={ma.re+mc.re,ma.im+mc.im};
                TestComplex t1={ma.re-mc.re,ma.im-mc.im};
                TestComplex t2={mb.re+md.re,mb.im+md.im};
                TestComplex t3={mb.re-md.re,mb.im-md.im};
                out[a]=(TestComplex){t0.re+t2.re,t0.im+t2.im};
                out[b]=(TestComplex){t1.re+t3.im,t1.im-t3.re};
                if (!j) {
                    out[c]=(TestComplex){t0.re-t2.re,t0.im-t2.im};
                    continue;
                }
                if (j==q/2) continue;
                out[base+q-j]=(TestComplex){t1.re-t3.im,-t1.im-t3.re};
                out[base+2*q-j]=(TestComplex){t0.re-t2.re,-t0.im+t2.im};
            }
    }
}

static void test_stretch_real_fft(void) {
#if defined(__x86_64__) || defined(__i386__)
    enum { SIZE=STRETCH_REAL_FFT_SIZE, HALF=SIZE/2, N=2000 };
    static TestComplex twiddle[HALF];
    static unsigned real_reverse[HALF];
    static float input[SIZE];
    static double data_c[2*SIZE], data_v[2*SIZE], data_l[2*SIZE];
    for (unsigned i=0;i<HALF;i++) {
        twiddle[i]=(TestComplex){cos(2*M_PI*i/SIZE),-sin(2*M_PI*i/SIZE)};
        unsigned reversed_pairs=0;
        for (unsigned shift=0;shift<10;shift+=2)
            reversed_pairs|=((i>>shift)&3)<<(8-shift);
        real_reverse[i]=reversed_pairs;
    }
    unsigned state=0x1b873593u;
    for (int iteration=0;iteration<20;iteration++) {
        for (int i=0;i<SIZE;i++) {
            state=state*1664525u+1013904223u;
            input[i]=(float)((int)(state>>8)/16777216.0-.5);
        }
        memset(data_c,0,sizeof(data_c));
        memset(data_v,0,sizeof(data_v));
        memset(data_l,0,sizeof(data_l));
        stretch_real_fft_legacy((TestComplex*)data_l,input,twiddle,
            real_reverse);
        stretch_real_fft_c(data_c,input,(const double*)twiddle,
            real_reverse);
        stretch_real_fft_avx2(data_v,input,(const double*)twiddle,
            real_reverse);
        assert(memcmp(data_l,data_c,sizeof(data_c))==0);
        assert(memcmp(data_l,data_v,sizeof(data_v))==0);
    }
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2")) {
        volatile double sink=0;
        struct timespec t0,t1,t2;
        clock_gettime(CLOCK_MONOTONIC,&t0);
        for (int i=0;i<N;i++) {
            stretch_real_fft_c(data_c,input,(const double*)twiddle,
                real_reverse);
            sink+=data_c[0];
        }
        clock_gettime(CLOCK_MONOTONIC,&t1);
        for (int i=0;i<N;i++) {
            stretch_real_fft_avx2(data_v,input,(const double*)twiddle,
                real_reverse);
            sink+=data_v[0];
        }
        clock_gettime(CLOCK_MONOTONIC,&t2);
        (void)sink;
        double c_ms=(t1.tv_sec-t0.tv_sec)*1e3+
            (t1.tv_nsec-t0.tv_nsec)/1e6;
        double v_ms=(t2.tv_sec-t1.tv_sec)*1e3+
            (t2.tv_nsec-t1.tv_nsec)/1e6;
        printf("real stretch FFT: C %.3f ms, AVX2 %.3f ms (%.2fx)\n",
            c_ms,v_ms,v_ms>0?c_ms/v_ms:0);
    }
#endif
}

static void test_sample_interp(void) {
    float samples[64];
    for (int i=0;i<64;i++) samples[i]=(float)((i*37%101)-50)/37.0f;
    const double positions[]={0,.01,.25,.5,1.5,7.125,31.0,31.75,63.999};
    for (size_t i=0;i<sizeof(positions)/sizeof(positions[0]);i++) {
        double cl,cr,al,ar;
        sample_interp2_c(samples,32,positions[i],&cl,&cr);
        sample_interp2(samples,32,positions[i],&al,&ar);
        assert(memcmp(&cl,&al,sizeof(cl))==0 && memcmp(&cr,&ar,sizeof(cr))==0);
#if defined(__x86_64__) || defined(__i386__)
        __builtin_cpu_init();
        if (__builtin_cpu_supports("avx2")) {
            double xl,xr;
            sample_interp2_avx2(samples,32,positions[i],&xl,&xr);
            assert(memcmp(&cl,&xl,sizeof(cl))==0 && memcmp(&cr,&xr,sizeof(cr))==0);
        }
#endif
    }
#if defined(__x86_64__) || defined(__i386__)
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2")) {
        enum { N=5000000 };
        double l=0,r=0; volatile double sink=0;
        struct timespec t0,t1,t2;
        clock_gettime(CLOCK_MONOTONIC,&t0);
        for (int i=0;i<N;i++) { sample_interp2_c(samples,32,(i%31000)/1000.0,&l,&r); sink+=l+r; }
        clock_gettime(CLOCK_MONOTONIC,&t1);
        for (int i=0;i<N;i++) { sample_interp2_avx2(samples,32,(i%31000)/1000.0,&l,&r); sink+=l+r; }
        clock_gettime(CLOCK_MONOTONIC,&t2);
        (void)sink;
        double c=(t1.tv_sec-t0.tv_sec)*1e3+(t1.tv_nsec-t0.tv_nsec)/1e6;
        double v=(t2.tv_sec-t1.tv_sec)*1e3+(t2.tv_nsec-t1.tv_nsec)/1e6;
        printf("stereo sample interp: C %.3f ms, AVX2 %.3f ms (%.2fx)\n",c,v,v>0?c/v:0);
    }
#endif
}

static void test_source_controls(void) {
    AudioEvent event;
    assert(source_event("{\"s\":\"pulse\",\"note\":69,\"pwrate\":3}",&event));
    assert(event.inst==INST_PULSE && event.dsp.p[DSP_PWSWEEP]==.3);
    assert(source_event("{\"s\":\"pulse\",\"pwsweep\":0.4}",&event));
    assert(event.dsp.p[DSP_PWRATE]==1);
    assert(source_event("{\"s\":\"sawtooth\",\"n\":1}",&event));
    assert(event.inst==INST_ADDITIVE && event.dsp.periodic && event.dsp.n_partials==1);
    DspState state;dsp_init(&state,&event.dsp);
    assert(fabs(dsp_additive(&state,M_PI/2,440)+1)<1e-6);
    const DspPeriodic *first=event.dsp.periodic;
    assert(source_event("{\"s\":\"sawtooth\",\"n\":1,\"cutoff\":1000}",&event));
    assert(event.dsp.periodic==first); /* Effects do not duplicate source assets. */
    assert(source_event("{\"s\":\"triangle\",\"n\":1}",&event));
    dsp_init(&state,&event.dsp);assert(fabs(dsp_additive(&state,0,440)-1)<1e-6);
    assert(source_event("{\"s\":\"user\",\"partials\":[1],\"phases\":[0.25]}",&event));
    dsp_init(&state,&event.dsp);assert(fabs(dsp_additive(&state,0,440)+1)<1e-6);
    assert(source_event("{\"s\":\"sine\",\"partials\":[0,0,1]}",&event));
    assert(event.inst==INST_SINE && !event.dsp.periodic);
    assert(source_event("{\"s\":\"sawtooth\",\"n\":0}",&event) && event.inst==INST_SAW);
    assert(!source_event("{\"s\":\"sawtooth\",\"n\":-1}",&event));
    assert(source_event("{\"s\":\"square\",\"n\":2047}",&event));
    dsp_init(&state,&event.dsp);
    for (int i=0;i<2048;i++) assert(isfinite(dsp_additive(&state,i*.01,20+i*12)));
    assert(source_event("{\"s\":\"sawtooth\",\"noise\":1}",&event));
    assert(event.dsp.noise_mix==1 && event.dsp.source_noise);
    const float *pink=event.dsp.source_noise;
    assert(source_event("{\"s\":\"pink\"}",&event) && event.dsp.source_noise==pink);
    dsp_init(&state,&event.dsp);
    double initial=dsp_source_noise(&state);
    for (int i=1;i<88200;i++) assert(isfinite(dsp_source_noise(&state)));
    assert(dsp_source_noise(&state)==initial); /* Same two-second web loop. */
    state.source_noise_position=88200-.5;
    assert(fabs(dsp_source_noise(&state)-(pink[88199]+pink[0])*.5)<1e-8);
    assert(state.source_noise_position==.5);
}

static void test_periodic_limits(void) {
    pid_t child=fork();assert(child>=0);
    if (!child) {
        DspParams p;dsp_params_init(&p);p.periodic_kind=4;p.n_partials=1;p.partials[0]=3.14159;
        assert(dsp_prepare_periodic(&p));const DspPeriodic *retained=p.periodic;
        int full=0;
        for (int i=0;i<257;i++) {
            p.periodic=NULL;p.partials[0]=i+10;
            if (!dsp_prepare_periodic(&p)) {full=1;break;}
        }
        assert(full);
        p.partials[0]=3.14159;assert(dsp_prepare_periodic(&p) && p.periodic==retained);
        p.periodic_numeric=0;p.n_partials=33;assert(!dsp_prepare_periodic(&p));
        p.periodic_numeric=1;p.n_partials=2048;assert(!dsp_prepare_periodic(&p));
        _exit(0);
    }
    int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && !WEXITSTATUS(status));
}

static void test_supersaw(void) {
    DspParams params; dsp_params_init(&params);
    assert(params.p[DSP_UNISON]==5 && params.p[DSP_SPREAD]==.6 && params.supersaw_detune==.18);
    params.enabled=1;
    DspState first,second; dsp_init(&first,&params); dsp_init(&second,&params);
    dsp_supersaw_init(&first,1); dsp_supersaw_init(&second,2);
    assert(first.supersaw_voices==5 && first.supersaw_gain==1/sqrt(5));
    assert(fabs(first.supersaw_pan_l-sqrt(.2))<1e-15);
    assert(fabs(first.supersaw_pan_r-sqrt(.8))<1e-15);
    for (unsigned i=0;i<5;i++) {
        assert(first.saw_phase[i]>0 && first.saw_phase[i]<1);
        assert(first.saw_phase[i]!=second.saw_phase[i]);
        if (i) assert(first.saw_phase[i]!=first.saw_phase[i-1]);
    }
    assert(fabs(first.saw_ratio[0]*first.saw_ratio[4]-1)<1e-15);
    params.p[DSP_UNISON]=1; params.p[DSP_SPREAD]=1;
    dsp_init(&first,&params); dsp_supersaw_init(&first,1);
    double phase=first.saw_phase[0];
    assert(first.saw_ratio[0]==1);
    dsp_supersaw(&first,440);
    assert(fabs(first.supersaw_l-(2*phase-1)*sqrt(.5))<1e-15);
    assert(first.supersaw_l==first.supersaw_r);
    params.p[DSP_UNISON]=100;
    dsp_init(&first,&params); dsp_supersaw_init(&first,1);
    assert(first.supersaw_voices==100);
    for (int i=0;i<4096;i++) assert(isfinite(dsp_supersaw(&first,12000)));
    params.p[DSP_UNISON]=1.000001; params.supersaw_detune=9600;
    dsp_init(&first,&params); dsp_supersaw_init(&first,1);
    for (int i=0;i<512;i++) assert(isfinite(dsp_supersaw(&first,12000)));
    for (unsigned i=0;i<first.supersaw_voices;i++) assert(isfinite(first.saw_phase[i]));
    cJSON *hap=cJSON_Parse("{\"begin\":0,\"end\":1,\"value\":{\"s\":\"supersaw\",\"note\":69,\"n\":0.5,\"detune\":12}}");
    assert(hap); AudioEvent event;
    assert(js_audio_event(hap,0,1,&event));
    assert(event.freq==440 && event.dsp.supersaw_detune==12);
    cJSON *value=cJSON_GetObjectItemCaseSensitive(hap,"value");
    cJSON_DeleteItemFromObjectCaseSensitive(value,"detune");
    assert(js_audio_event(hap,0,1,&event) && event.dsp.supersaw_detune==.5);
    cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(value,"s"),"sawtooth");
    cJSON_AddNumberToObject(value,"detune",1200);
    assert(js_audio_event(hap,0,1,&event) && event.freq==440);
    cJSON_Delete(hap);
}

static void test_standalone_file_event(void) {
    pid_t child=fork(); assert(child>=0);
    if (!child) {
        char root[]="/tmp/tompouce-file-event-XXXXXX"; assert(mkdtemp(root));
        assert(!setenv("XDG_CACHE_HOME",root,1) && !setenv("XDG_CONFIG_HOME",root,1));
        assert(!setenv("TOMPOUCE_BANKS",root,1));
        char path[512]; snprintf(path,sizeof(path),"%s/My Drum.wav",root);
        const unsigned char wav[]={ 'R','I','F','F',40,0,0,0,'W','A','V','E','f','m','t',' ',
            16,0,0,0,1,0,1,0,68,172,0,0,136,88,1,0,2,0,16,0,'d','a','t','a',4,0,0,0,0,32,0,192 };
        FILE *file=fopen(path,"wb"); assert(file);
        assert(fwrite(wav,1,sizeof(wav),file)==sizeof(wav)); assert(!fclose(file));
        banks_start_index(root);
        for (int i=0;!banks_ready() && i<500;i++) usleep(10000);
        assert(banks_ready());
        cJSON *metadata=NULL; char error[512];
        JsProgram *program=javascript_compile("s(pure('My Drum.wav'))",120,&metadata,error,sizeof(error)); assert(program);
        js_audio_schedule(program,0,0,event_queue_generation());
        assert(event_queue_depth()==1);
        char queued[2048]; assert(scheduler_debug_queue(queued,sizeof(queued),1)>0);
        assert(strstr(queued,"smp=0 "));
        float *pcm=NULL; int frames=0,channels=0;
        smp_get_stereo(0,&pcm,&frames,&channels);
        assert(pcm && frames==2 && channels==1 && fabs(pcm[0]-.25)<1e-6);
        event_queue_clear(); javascript_free(program); cJSON_Delete(metadata);
        assert(!unlink(path));
        snprintf(path,sizeof(path),"%s/tompouce/banks.ldb",root);
        DIR *dir=opendir(path); assert(dir); struct dirent *entry;
        while ((entry=readdir(dir))) {
            if (!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
            char item[1024]; snprintf(item,sizeof(item),"%s/%s",path,entry->d_name); assert(!unlink(item));
        }
        closedir(dir); assert(!rmdir(path));
        snprintf(path,sizeof(path),"%s/tompouce",root); assert(!rmdir(path)); assert(!rmdir(root));
        _exit(0);
    }
    int status; assert(waitpid(child,&status,0)==child); assert(WIFEXITED(status) && !WEXITSTATUS(status));
}

static double rms(const char *controls, double hz) {
    cJSON *json = cJSON_Parse(controls);
    assert(json);
    DspParams parameters; DspState state;
    dsp_params_read(&parameters,json);
    dsp_init(&state,&parameters);
    cJSON_Delete(json);
    double sum = 0;
    for (int i = 0; i < 44100; i++) {
        double age = i/SR;
        double x = dsp_process(&state,sin(2*M_PI*hz*age),age,2);
        assert(isfinite(x));
        if (i > 4410) sum += x*x;
    }
    return sqrt(sum/39689);
}

static void test_filter_envelopes(void) {
    for (int type = 0; type < 3; type++) {
        for (int sign = -1; sign <= 1; sign += 2) {
            DspParams p;
            dsp_params_init(&p);
            int base = type*7;
            p.p[base] = 1000; p.p[base+2] = sign*2;
            p.p[DSP_ANCHOR] = .5;
            p.p[base+3] = .2; p.p[base+4] = .2;
            p.p[base+5] = .5; p.p[base+6] = .2;
            double start = sign > 0 ? 500 : 2000;
            double peak = sign > 0 ? 2000 : 500;
            assert(fabs(dsp_filter_frequency(&p,type,0,1)-start) < 1e-8);
            assert(fabs(dsp_filter_frequency(&p,type,.1,1)-1000) < 1e-8);
            assert(fabs(dsp_filter_frequency(&p,type,.2,1)-peak) < 1e-8);
            assert(fabs(dsp_filter_frequency(&p,type,.5,1)-1250) < 1e-8);
            assert(fabs(dsp_filter_frequency(&p,type,1.2,1)-start) < 1e-8);
            /* A short note ramps to the interpolated endpoint at gate. */
            assert(fabs(dsp_filter_frequency(&p,type,.1,.1)-1250) < 1e-8);
            assert(fabs(dsp_filter_frequency(&p,type,.3,.1)-start) < 1e-8);
        }
    }
}

/* The cached-activity fast paths must be bit-identical to the full paths.
   controls are immutable per voice, so forcing the flags reproduces what the
   slow path would have computed. */
static void test_inactive_fast_paths(void) {
    DspParams params;
    dsp_params_init(&params);
    params.enabled = 1;
    DspState fast, slow;
    dsp_init(&fast, &params);
    dsp_init(&slow, &params);
    assert(!fast.effects_active && !fast.pitch_active && !fast.fm_active);
    slow.effects_active = 1; slow.pitch_active = 1; slow.fm_active = 1;
    for (int i = 0; i < 4096; i++) {
        double age = i / SR;
        double x = sin(i * .37) * (i % 7 ? .3 : 90);
        assert(dsp_process(&fast, x, age, 1) == dsp_process(&slow, x, age, 1));
        assert(dsp_frequency(&fast, 440, age, 1) == dsp_frequency(&slow, 440, age, 1));
        assert(dsp_frequency(&fast, 1234.5, age, 1) == 1234.5);
        assert(dsp_pitch_multiplier(&params, age, 1) == 1);
    }
    /* Guard rails: post gain and any filter/pitch control must take the full
       path, and the derived flags must notice them. */
    DspParams post; dsp_params_init(&post); post.enabled = 1; post.p[DSP_POST] = 2;
    DspState post_state; dsp_init(&post_state, &post);
    assert(post_state.effects_active);
    DspParams pitch; dsp_params_init(&pitch); pitch.enabled = 1; pitch.p[DSP_PENV] = 12;
    DspState pitch_state; dsp_init(&pitch_state, &pitch);
    assert(pitch_state.pitch_active && !pitch_state.effects_active);
    DspParams fm; dsp_params_init(&fm); fm.enabled = 1; fm.p[DSP_FMBASE] = 2;
    DspState fm_state; dsp_init(&fm_state, &fm);
    assert(fm_state.fm_active);
    puts("inactive DSP fast paths are bit-identical");
}

static void test_fm_source_semantics(void) {
    cJSON *value=cJSON_Parse("{\"fmi\":0.5,\"fmh\":2}"); assert(value);
    DspParams p; dsp_params_read(&p,value);
    assert(!p.fm_envelopes);
    DspState state; dsp_init(&state,&p);
    dsp_oscillator_onset(&state,.274);
    /* First pulled modulator block starts at phase zero. The carrier reads
       76 of its values, then the next full modulator block starts at 128. */
    for (int i=0;i<300;i++) {
        int frame=i<76?i:i+52;
        float signal=(float)sin(2*M_PI*880*frame/SR);
        float modulation=(float)((float)(signal*.5f)*880.f);
        double expected=440*(1+modulation/440.0);
        assert(fabs(dsp_frequency(&state,440,i/SR,.5)-expected)<1e-8);
    }
    cJSON_AddNumberToObject(value,"fmattack",.1);
    dsp_params_read(&p,value); assert(p.fm_envelopes==1);
    assert(p.p[DSP_FMBASE+2]==.1 && p.p[DSP_FMBASE+3]==.001);
    assert(p.p[DSP_FMBASE+4]==1 && p.p[DSP_FMBASE+5]==.01 && p.p[DSP_FMBASE+6]==1);
    dsp_init(&state,&p); state.fm_phase[0]=M_PI/2;
    float modulation=(float)((float)((float)sqrt(.001)*.5f)*880.f);
    assert(fabs(dsp_frequency(&state,440,.05,.5)-440*(1+modulation/440.0))<1e-8);
    cJSON_AddStringToObject(value,"fmenv","linear");
    dsp_params_read(&p,value); dsp_init(&state,&p); state.fm_phase[0]=M_PI/2;
    assert(fabs(dsp_frequency(&state,440,.05,.5)-660)<1e-8);
    cJSON_DeleteItemFromObjectCaseSensitive(value,"fmattack");
    cJSON_AddNumberToObject(value,"fmdecay",.2);
    dsp_params_read(&p,value);
    assert(p.p[DSP_FMBASE+2]==.001 && p.p[DSP_FMBASE+4]==.001);
    cJSON_AddNumberToObject(value,"fmattack2",0);
    cJSON_AddNumberToObject(value,"fmrelease2",0);
    dsp_params_read(&p,value);
    assert(p.fm_envelopes==3 && p.p[DSP_FMBASE+8+2]==.001 && p.p[DSP_FMBASE+8+5]==.01);
    cJSON_Delete(value);
}

static void test_fm_matrix(void) {
    cJSON *value=cJSON_Parse("{\"fmi\":0.5,\"fmi2\":0.25,\"fmh2\":3}"); assert(value);
    DspParams p; dsp_params_read(&p,value); cJSON_Delete(value);
    assert(p.matrix[0][0]==.5 && p.matrix[1][1]==.25 && p.matrix[1][0]==0);
    DspState state; dsp_init(&state,&p);
    assert(state.fm_count==2 && state.fm_order[0]==1 && state.fm_order[1]==0);
    value=cJSON_Parse("{\"fmi2\":0.5}"); assert(value);
    dsp_params_read(&p,value); cJSON_Delete(value); dsp_init(&state,&p);
    assert(!state.fm_active && dsp_frequency(&state,440,0,.5)==440);
    value=cJSON_Parse("{\"fmi80\":-0.25}"); assert(value);
    dsp_params_read(&p,value); cJSON_Delete(value); dsp_init(&state,&p);
    assert(state.fm_count==1 && state.fm_order[0]==7 && p.matrix[7][0]==-.25);
    value=cJSON_Parse("{\"fmi\":0.5,\"fmi2\":0.25,\"fmi12\":0.1}"); assert(value);
    dsp_params_read(&p,value); cJSON_Delete(value); dsp_init(&state,&p);
    assert(state.fm_count==2);
    for (int i=0;i<4096;i++) assert(isfinite(dsp_frequency(&state,440,i/SR,.5)));
}

static void test_fm_wave_and_range(void) {
    cJSON *value=cJSON_Parse("{\"fmi\":1,\"fmh\":100}"); assert(value);
    DspParams p; dsp_params_read(&p,value); cJSON_Delete(value);
    DspState state; dsp_init(&state,&p);
    for (int i=0;i<32;i++) {
        double rendered=dsp_frequency(&state,440,i/SR,.5);
        assert(fabs(rendered)<=SR*.5+1e-9);
        assert(fabs(rendered-440)<1e-3);
    }
    value=cJSON_Parse("{\"fmi\":30,\"fmh\":2}"); assert(value);
    dsp_params_read(&p,value); cJSON_Delete(value); dsp_init(&state,&p);
    int outside=0;
    for (int i=0;i<128;i++) {
        double rendered=dsp_frequency(&state,440,i/SR,.5);
        assert(isfinite(rendered));
        if (fabs(rendered)>SR*.5) outside=1;
    }
    assert(outside);
    value=cJSON_Parse("{\"fmi\":0.5,\"fmh\":2,\"fmwave\":\"sawtooth\"}"); assert(value);
    dsp_params_read(&p,value); cJSON_Delete(value); dsp_init(&state,&p);
    double phase=0; float rate=880;
    for (int i=0;i<16;i++) {
        double wave=dsp_oscillator(0,phase,rate,.5);
        float modulation=(float)((float)wave*.5f)*rate;
        assert(fabs(dsp_frequency(&state,440,i/SR,.5)-440*(1+modulation/440.0))<1e-5);
        phase=fmod(phase+2*M_PI*rate/SR,2*M_PI);
    }
    value=cJSON_Parse("{\"fmi\":0.25,\"fmwave\":\"white\"}"); assert(value);
    dsp_params_read(&p,value); assert(dsp_prepare_fm(&p)); cJSON_Delete(value);
    dsp_init(&state,&p);
    const float *buffer=p.fm_noise[0]; assert(buffer);
    for (int i=0;i<8;i++) {
        float modulation=(float)((float)buffer[i]*.25f)*440.f;
        assert(fabs(dsp_frequency(&state,440,i/SR,.5)-440*(1+modulation/440.0))<1e-5);
    }
    value=cJSON_Parse("{\"fmi2\":0.5,\"fmwave2\":\"pink\",\"fmi12\":1}"); assert(value);
    dsp_params_read(&p,value); assert(dsp_prepare_fm(&p)); cJSON_Delete(value);
    dsp_init(&state,&p);
    for (int i=0;i<256;i++) assert(fabs(dsp_frequency(&state,440,i/SR,.5)-440)<1e-9);
    /* A silent noise buffer feeding an oscillator must remain silent during
       a fractional onset too, rather than introduce a phantom sine or phase
       shift through the noise operator's envelope. */
    static const float silent_noise[88200]={0};
    value=cJSON_Parse("{\"fmi\":0.5,\"fmh\":2,\"fmi2\":0.3,\"fmwave2\":\"pink\",\"fmattack2\":0.04}"); assert(value);
    dsp_params_read(&p,value); cJSON_Delete(value);
    p.fm_noise[1]=silent_noise;
    DspState silent; dsp_init(&silent,&p); dsp_oscillator_onset(&silent,.274);
    p.p[DSP_FMBASE+8]=0;
    dsp_init(&state,&p); dsp_oscillator_onset(&state,.274);
    for (int i=0;i<512;i++)
        assert(fabs(dsp_frequency(&state,440,i/SR,.5)-dsp_frequency(&silent,440,i/SR,.5))<1e-9);
    value=cJSON_Parse("{\"fmi\":0.25,\"fmwave\":\"white\",\"fmattack\":0.1}"); assert(value);
    dsp_params_read(&p,value); cJSON_Delete(value); assert(dsp_prepare_fm(&p));
    dsp_init(&state,&p); dsp_oscillator_onset(&state,64/SR);
    /* Before the envelope starts halfway through the first quantum, the
       noise GainNode retains its default of 1, not the attack's minimum. */
    for (int i=0;i<64;i++) {
        float modulation=(float)(p.fm_noise[0][i]*.25f)*440.f;
        assert(fabs(dsp_frequency(&state,440,i/SR,.5)-440*(1+modulation/440.0))<1e-5);
    }
    assert(dsp_oscillator_nyquist_gain(880)==1 && dsp_oscillator_nyquist_gain(-880)==1);
    assert(dsp_oscillator_nyquist_gain((float)(SR*.5))==0);
    assert(dsp_oscillator_nyquist_gain(-(float)(SR*.5))==0);
    assert(dsp_oscillator_nyquist_gain(30000)==0);
    assert(fabsf(dsp_oscillator_nyquist_gain(21120)-3.f*log2f(22050.f/21120.f))<1e-6f);
    puts("FM waveforms follow the OscillatorNode frequency range");
}

static void test_fm_lfo_traj(void) {
    cJSON *value=cJSON_Parse("{\"fmi\":0.5,\"fmh\":2}"); assert(value);
    DspParams p; dsp_params_read(&p,value); cJSON_Delete(value);
    DspState held, ramped;
    static double traj[16*128];
    /* A null trajectory is the legacy sample-held path. */
    dsp_init(&held,&p); dsp_init(&ramped,&p);
    for (int i=0;i<260;i++) {
        double a=dsp_frequency(&held,440,i/SR,.5);
        double b=dsp_frequency_traj(&ramped,440,i/SR,.5,NULL);
        assert(a==b);
    }
    /* A uniform trajectory row matches the held voice-stepped value,
       locking the FMH row layout. */
    dsp_init(&held,&p); dsp_init(&ramped,&p);
    held.control.gen_lfo[GEN_FMH1]=3.25;
    for (int k=0;k<16*128;k++) traj[k]=0.0;
    for (int n=0;n<128;n++) traj[8*128+n]=3.25;
    for (int i=0;i<260;i++) {
        double a=dsp_frequency(&held,440,i/SR,.5);
        double b=dsp_frequency_traj(&ramped,440,i/SR,.5,traj);
        assert(a==b);
    }
    /* A varying trajectory bends each modulator frame instead of holding
       the refill sample across the quantum. */
    dsp_init(&held,&p); dsp_init(&ramped,&p);
    for (int n=0;n<128;n++) traj[8*128+n]=(double)n;
    int differ=0;
    for (int i=0;i<260;i++) {
        double a=dsp_frequency(&held,440,i/SR,.5);
        double b=dsp_frequency_traj(&ramped,440,i/SR,.5,traj);
        if (a!=b) differ=1;
    }
    assert(differ);
    /* Same layout lock for an operator-target index edge (op2 into op1). */
    value=cJSON_Parse("{\"fmi2\":0.25}"); assert(value);
    dsp_params_read(&p,value); cJSON_Delete(value);
    dsp_init(&held,&p); dsp_init(&ramped,&p);
    held.control.fm_gen_edge[1]=2; ramped.control.fm_gen_edge[1]=2;
    held.control.gen_lfo[GEN_FMI2]=0.0625;
    for (int k=0;k<16*128;k++) traj[k]=0.0;
    for (int n=0;n<128;n++) traj[1*128+n]=0.0625;
    for (int i=0;i<260;i++) {
        double a=dsp_frequency(&held,440,i/SR,.5);
        double b=dsp_frequency_traj(&ramped,440,i/SR,.5,traj);
        assert(a==b);
    }
    puts("FM quantum fill follows the per-frame LFO trajectory");
}

static void test_pitch_envelope_defaults(void) {
    DspParams p;
    cJSON *value=cJSON_Parse("{\"penv\":12}"); assert(value);
    dsp_params_read(&p,value); cJSON_Delete(value);
    assert(p.p[DSP_PATT]==.2 && p.p[DSP_PDEC]==.001 && p.p[DSP_PSUS]==1);
    assert(p.p[DSP_PREL]==.001 && p.p[DSP_PANCHOR]==1);
    assert(fabs(dsp_pitch_multiplier(&p,0,1)-.5)<1e-12);
    assert(fabs(dsp_pitch_multiplier(&p,.1,1)-sqrt(.5))<1e-12);
    assert(dsp_pitch_multiplier(&p,.2,1)==1);
    value=cJSON_Parse("{\"pdecay\":0.1}"); assert(value);
    dsp_params_read(&p,value); cJSON_Delete(value);
    assert(p.p[DSP_PENV]==1 && p.p[DSP_PATT]==.001 && p.p[DSP_PDEC]==.1);
    assert(p.p[DSP_PSUS]==.001 && p.p[DSP_PANCHOR]==.001 && p.p[DSP_PREL]==.01);
    value=cJSON_Parse("{\"penv\":-7,\"pattack\":0,\"pdecay\":0.2,\"psustain\":0,\"panchor\":0}"); assert(value);
    dsp_params_read(&p,value); cJSON_Delete(value);
    assert(p.p[DSP_PATT]==.001 && p.p[DSP_PANCHOR]==0);
    assert(dsp_pitch_multiplier(&p,0,.5)==1);
    assert(fabs(dsp_pitch_multiplier(&p,.001,.5)-pow(2,-7/12.0))<1e-12);
    assert(fabs(dsp_pitch_multiplier(&p,.101,.5)-pow(2,-3.5/12))<1e-12);
    DspState state; dsp_init(&state,&p); dsp_oscillator_onset(&state,.5);
    assert(state.pitch_initial_frames==94);
    assert(fabs(state.pitch_quantum_offset-34/SR)<1e-15);
    assert(dsp_frequency(&state,440,0,.5)==440);
    assert(fabs(dsp_frequency(&state,440,94/SR,.5)-440*dsp_pitch_multiplier(&p,94/SR,.5))<1e-12);
    assert(!state.pitch_initial_frames);
    dsp_init(&state,&p); dsp_oscillator_onset(&state,.274);
    assert(state.pitch_initial_frames==76);
    assert(fabs(state.pitch_onset_fraction-.6/SR)<1e-14);
}

static void test_exponential_pitch(void) {
    DspParams p; dsp_params_init(&p); p.enabled=1;
    p.p[DSP_PCURVE]=1; p.p[DSP_PENV]=12; p.p[DSP_PANCHOR]=.5;
    p.p[DSP_PATT]=.1; p.p[DSP_PDEC]=.2; p.p[DSP_PSUS]=.5; p.p[DSP_PREL]=.2;
    /* AudioParam ramps across a sign change hold their start value. */
    assert(fabs(dsp_pitch_multiplier(&p,.05,1)-pow(2,-.5))<1e-12);
    assert(fabs(dsp_pitch_multiplier(&p,.1,1)-pow(2,.5))<1e-12);
    /* Decimal segment rounding makes this endpoint slightly negative, so
       this decay also crosses zero and holds until the endpoint. */
    assert(fabs(dsp_pitch_multiplier(&p,.2,1)-pow(2,.5))<1e-12);
    assert(fabs(dsp_pitch_multiplier(&p,.5,1)-1)<1e-12);
    assert(dsp_pitch_multiplier(&p,1.1,1)==1);
    assert(fabs(dsp_pitch_multiplier(&p,1.2,1)-pow(2,-.5))<1e-12);
    /* With exact binary durations a zero ramp endpoint becomes .001 cents. */
    p.p[DSP_PATT]=.125; p.p[DSP_PDEC]=.25;
    double midpoint=sqrt(600*.001);
    assert(fabs(dsp_pitch_multiplier(&p,.25,1)-pow(2,midpoint/1200))<1e-12);
    /* Short gates clip endpoints linearly before exponential interpolation. */
    p.p[DSP_PANCHOR]=1; p.p[DSP_PATT]=.5;
    double endpoint=-1200+(1200+.001)*.1;
    double cents=-sqrt(1200*-endpoint);
    assert(fabs(dsp_pitch_multiplier(&p,.025,.05)-pow(2,cents/1200))<1e-12);
}

int main(void) {
    test_exponential_pitch();
    test_pitch_envelope_defaults();
    /* Public Superdough .room(1) supplies no duration override: the first
       shared room uses generate()'s two-second default. */
    cJSON *room_defaults=cJSON_Parse("{\"room\":1}"); assert(room_defaults);
    DspParams room_params; dsp_params_read(&room_params,room_defaults);
    assert(room_params.p[DSP_SIZE]==2 && room_params.p[DSP_ROOMFADE]==.1);
    assert(room_params.p[DSP_ROOMLP]==15000 && room_params.p[DSP_ROOMDIM]==1000);
    assert(convolution_prepare_reverb(room_params.p[DSP_SIZE],
        room_params.p[DSP_ROOMFADE],room_params.p[DSP_ROOMLP],room_params.p[DSP_ROOMDIM]));
    cJSON_Delete(room_defaults);
    /* Generated-room sequences vary per call: consecutive distinct rooms use
       disjoint stream segments (no reseeding to identical kernels), while
       repeats hit the cache. */
    convolution_reverb_reset_stream();
    const ConvKernel *first=convolution_prepare_reverb(2,.1,15000,1000);
    const ConvKernel *second=convolution_prepare_reverb(1,.1,15000,1000);
    assert(first && second && first!=second);
    assert(memcmp(first->head,second->head,sizeof(first->head)));
    assert(convolution_prepare_reverb(2,.1,15000,1000)==first);
    assert(convolution_prepare_reverb(1,.1,15000,1000)==second);
    test_standalone_file_event();
    double alias_naive = 0, alias_filtered = 0;
    for (int i = 0; i < 256; i++) {
        double phase = 2*M_PI*i*3/16;
        double t = phase/(2*M_PI);
        double reference = sin(2*M_PI*i/16);
        alias_naive += (2*(t-floor(t+.5)))*reference;
        alias_filtered += dsp_oscillator(0,phase,SR*3/16,.5)*reference;
    }
    assert(fabs(alias_filtered) < fabs(alias_naive)*.6);
    assert(fabs(dsp_oscillator(2,0,440,.5)) < 1e-9);
    assert(fabs(dsp_oscillator(2,M_PI/2,440,.5)-1) < 1e-9);
    test_filter_envelopes();
    /* WebAudio low/high-pass resonance is in dB, rather than linear Q. */
    assert(fabs(rms("{\"cutoff\":1000,\"resonance\":10}",1000)-sqrt(5)) < .01);
    assert(fabs(rms("{\"hcutoff\":1000,\"hresonance\":10}",1000)-sqrt(5)) < .01);
    /* Attack alone holds the destination instead of decaying back. */
    cJSON *bass = cJSON_Parse("{\"cutoff\":800,\"lpenv\":-4,\"lpattack\":0.1}");
    assert(bass);
    DspParams bass_params;
    dsp_params_read(&bass_params,bass);
    assert(bass_params.p[DSP_LPF+4] == .001);
    assert(bass_params.p[DSP_LPF+5] == 1);
    cJSON_Delete(bass);
    assert(rms("{\"cutoff\":800,\"lpenv\":-4,\"lpattack\":0.1}",2000) >
           rms("{\"cutoff\":50}",2000)*10);
    assert(rms("{\"hcutoff\":1000}",100) < rms("{\"hcutoff\":1000}",5000)*.1);
    assert(rms("{\"cutoff\":1000}",5000) < rms("{\"cutoff\":1000}",100)*.1);
    assert(rms("{\"bandf\":1000,\"bandq\":4}",100) < rms("{\"bandf\":1000,\"bandq\":4}",1000)*.1);
    assert(rms("{\"postgain\":0}",1000) == 0);
    assert(rms("{\"compressor\":-30,\"compressorRatio\":20}",1000) < .1);
    /* Transient with zero amounts is a pure soft clip: strictly below the
       dry sine but well above silence. The node flag follows the control. */
    assert(rms("{\"transient\":0}",1000) < .45);
    assert(rms("{\"transient\":0}",1000) > .35);
    {
        cJSON *tr = cJSON_Parse("{\"transient\":0.8,\"transsustain\":0.5}");
        assert(tr);
        DspParams tp;
        dsp_params_read(&tp,tr);
        assert(tp.transient_on && tp.p[DSP_TRANSIENT] == .8 &&
            tp.p[DSP_TRANSSUSTAIN] == .5);
        cJSON_Delete(tr);
        cJSON *ts = cJSON_Parse("{\"transsustain\":0.5}");
        assert(ts);
        dsp_params_read(&tp,ts);
        assert(!tp.transient_on && tp.p[DSP_TRANSSUSTAIN] == .5);
        cJSON_Delete(ts);
    }
    /* Bandlimited saw versus the truncated Fourier series of the same ramp.
       440 Hz keeps about 50 harmonics under Nyquist. PolyBLEP should sit
       closer to that series than the naive saw. */
    {
        double poly_err = 0, naive_err = 0;
        const int harmonics = (int)(SR * .5 / 440);
        for (int i = 0; i < 512; i++) {
            double phase = 2 * M_PI * 440 * i / SR;
            double t = phase / (2 * M_PI); t -= floor(t);
            double shifted = t + .5; shifted -= floor(shifted);
            double series = 0;
            for (int k = 1; k <= harmonics; k++) series += sin(2 * M_PI * k * shifted) / k;
            series *= -2 / M_PI;
            double naive = 2 * shifted - 1;
            double poly = dsp_oscillator(0, phase, 440, .5);
            poly_err += (poly - series) * (poly - series);
            naive_err += (naive - series) * (naive - series);
        }
        assert(poly_err < naive_err * .5);
    }
    assert(fabs(dsp_envelope(.05,1,.1,.2,.5,.2)-.5) < 1e-9);
    assert(fabs(dsp_envelope(.3,1,.1,.2,.5,.2)-.5) < 1e-9);
    assert(dsp_envelope(1.3,1,.1,.2,.5,.2) == 0);
    (void)rms("{\"cutoff\":1000,\"lpenv\":8,\"hcutoff\":100,\"bandf\":2000,\"vowel\":\"a\",\"coarse\":8,\"crush\":4,\"distort\":8,\"phaserrate\":2}",200);

    cJSON *hap = cJSON_Parse("{\"begin\":0.25,\"end\":0.5,\"value\":{\"note\":\"fs4\",\"s\":\"supersaw\",\"hcutoff\":400,\"pan\":0,\"fmi\":2,\"fmh\":1.5,\"begin\":0.2,\"loop\":1}}");
    assert(hap);
    AudioEvent event;
    assert(js_audio_event(hap,10,2,&event));
    assert(event.time == 10.5 && event.duration == .5);
    assert(fabs(event.freq-midi_to_freq(66)) < 1e-9);
    assert(event.inst == INST_SUPERSAW && event.pan == -1);
    assert(event.gain == .8 && event.att == .001 && event.dec == .05 && event.sus == .6 && event.rel == .01);
    assert(event.dsp.p[DSP_HPF] == 400 && event.dsp.p[DSP_BEGIN] == .2);
    assert(event.dsp.p[DSP_FMBASE] == 2 && event.dsp.p[DSP_FMBASE+1] == 1.5);
    cJSON_Delete(hap);
    hap = cJSON_Parse("{\"begin\":0,\"end\":1,\"value\":{\"s\":\"sine\",\"velocity\":5}}");
    assert(js_audio_event(hap,0,1,&event) && fabs(event.gain-.8*5)<1e-12);
    cJSON_Delete(hap);
    hap = cJSON_Parse("{\"begin\":0,\"end\":1,\"value\":{\"s\":\"sine\",\"gain\":0.4,\"velocity\":2}}");
    assert(js_audio_event(hap,0,1,&event) && fabs(event.gain-0.8)<1e-12);
    cJSON_Delete(hap);
    hap = cJSON_Parse("{\"begin\":0,\"end\":1,\"value\":{\"s\":\"sine\",\"velocity\":0.5}}");
    assert(js_audio_event(hap,0,1,&event) && fabs(event.gain-0.4)<1e-12);
    cJSON_Delete(hap);
    hap = cJSON_Parse("{\"begin\":0,\"end\":1,\"value\":{\"s\":\"sine\",\"velocity\":32}}");
    assert(js_audio_event(hap,0,1,&event) && fabs(event.gain-.8*16)<1e-12);
    cJSON_Delete(hap);
    hap = cJSON_Parse("{\"begin\":0,\"end\":1,\"value\":{\"gain\":0}}");
    assert(js_audio_event(hap,0,2,&event) && event.gain==0 && event.dsp.enabled);
    cJSON_Delete(hap);
    assert(!js_audio_event(NULL,0,2,&event));

    assert(track_load_from_code("const voice = note('c4'); voice.hpf(400).gain(0)._scope().pianoroll()"));
    assert(track_count == 1 && tracks[0].javascript);
    assert(tracks[0].visual_requests == 3);
    JsProgram *playing = tracks[0].javascript;
    unsigned generation = event_queue_generation();
    assert(!track_load_from_code("const voice = ;"));
    assert(tracks[0].javascript == playing && event_queue_generation() == generation);
    javascript_retain(playing);
    assert(track_load_from_code("note(\"c4 e4\").gain(0)"));
    assert(tracks[0].javascript && tracks[0].javascript!=playing);
    cJSON *haps = javascript_query(playing,0,1);
    assert(cJSON_GetArraySize(haps) == 1);
    cJSON_Delete(haps); javascript_free(playing);
    event_queue_clear();
    memset(&event,0,sizeof(event));
    event.generation = generation;
    event_queue_push(&event);
    assert(event_queue_depth() == 0);
    event.generation = event_queue_generation();
    event_queue_push(&event);
    assert(event_queue_depth() == 1);
    event_queue_clear();
    puts("native DSP/Strudel audio controls ok");
    test_inactive_fast_paths();
    test_fm_source_semantics();
    test_fm_matrix();
    test_fm_wave_and_range();
    test_fm_lfo_traj();
    test_supersaw();
    test_source_controls();
    test_periodic_limits();
    test_parameter_lookup();
    test_distortion_site_formula();
    test_envelope4();
    test_saw4();
    test_square4();
    test_comb4();
    test_delay2();
    test_stretch_fft();
    test_stretch_real_fft();
    test_visual_fft();
    test_sample_interp();
    test_stereo_filter_path();
    test_stereo_filter_persistent();
    return 0;
}
