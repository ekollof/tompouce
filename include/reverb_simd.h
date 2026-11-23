#ifndef REVERB_SIMD_H
#define REVERB_SIMD_H
/* Four independent feedback comb taps for one fallback-reverb channel.
   Each lane reads and writes its own line; tanh stays the scalar libm call. */
void dsp_comb4_c(float *const line[4], const unsigned read[4],
    unsigned write, double input, double feedback, double sample_out[4]);
#if defined(__x86_64__) || defined(__i386__)
void dsp_comb4_avx2(float *const line[4], const unsigned read[4],
    unsigned write, double input, double feedback, double sample_out[4]);
#endif
void dsp_comb4(float *const line[4], const unsigned read[4],
    unsigned write, double input, double feedback, double sample_out[4]);
#endif
