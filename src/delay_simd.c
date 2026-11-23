#include "delay_simd.h"
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

void dsp_delay2_c(float *line, float *quantum, unsigned write, unsigned read,
    unsigned previous, unsigned feedback_pos, int interpolate,
    float fraction, double input_l, double input_r, double feedback,
    double *out_l, double *out_r) {
    float *q=quantum+2*feedback_pos;
    if (interpolate) {
        line[2*write]=(float)(input_l+q[0]);
        line[2*write+1]=(float)(input_r+q[1]);
    }
    float l=line[2*read], r=line[2*read+1];
    if (interpolate) {
        float keep=1.0f-fraction;
        l=l*keep+line[2*previous]*fraction;
        r=r*keep+line[2*previous+1]*fraction;
        q[0]=(float)(l*feedback);
        q[1]=(float)(r*feedback);
    } else {
        q[0]=q[1]=0;
        line[2*write]=(float)(input_l+l*feedback);
        line[2*write+1]=(float)(input_r+r*feedback);
    }
    *out_l=l; *out_r=r;
}

void dsp_delay2(float *line, float *quantum, unsigned write, unsigned read,
    unsigned previous, unsigned feedback_pos, int interpolate,
    float fraction, double input_l, double input_r, double feedback,
    double *out_l, double *out_r) {
    /* The two-lane SSE2 candidate is byte-exact but slightly slower than the
       interleaved scalar path; keep C for production. */
    dsp_delay2_c(line,quantum,write,read,previous,feedback_pos,interpolate,
        fraction,input_l,input_r,feedback,out_l,out_r);
}

#if defined(__SSE2__)
void dsp_delay2_sse2(float *line, float *quantum, unsigned write,
    unsigned read, unsigned previous, unsigned feedback_pos, int interpolate,
    float fraction, double input_l, double input_r, double feedback,
    double *out_l, double *out_r) {
    float *q=quantum+2*feedback_pos;
    __m128d inputs=_mm_set_pd(input_r,input_l);
    __m128d rate=_mm_set1_pd(feedback);
    __m128 value;
    if (interpolate) {
        __m128d quantum_value=_mm_cvtps_pd(_mm_loadl_pi(_mm_setzero_ps(),
            (const __m64*)q));
        value=_mm_cvtpd_ps(_mm_add_pd(inputs,quantum_value));
        _mm_storel_pi((__m64*)(line+2*write),value);
    }
    __m128 de=_mm_loadl_pi(_mm_setzero_ps(),(const __m64*)(line+2*read));
    if (interpolate) {
        __m128 previous_value=_mm_loadl_pi(_mm_setzero_ps(),
            (const __m64*)(line+2*previous));
        __m128 keep=_mm_set1_ps(1.0f-fraction);
        __m128 amount=_mm_set1_ps(fraction);
        de=_mm_add_ps(_mm_mul_ps(de,keep),
            _mm_mul_ps(previous_value,amount));
        __m128d product=_mm_mul_pd(_mm_cvtps_pd(de),rate);
        _mm_storel_pi((__m64*)q,_mm_cvtpd_ps(product));
    } else {
        _mm_storel_pi((__m64*)q,_mm_setzero_ps());
        __m128d product=_mm_mul_pd(_mm_cvtps_pd(de),rate);
        __m128 sum=_mm_cvtpd_ps(_mm_add_pd(inputs,product));
        _mm_storel_pi((__m64*)(line+2*write),sum);
    }
    float out[4];
    _mm_storeu_ps(out,de);
    *out_l=out[0]; *out_r=out[1];
}
#endif
