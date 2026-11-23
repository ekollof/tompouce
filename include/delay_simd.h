#ifndef DELAY_SIMD_H
#define DELAY_SIMD_H
/* Stereo shared-delay frame. `line` and `quantum` are interleaved L/R. */
void dsp_delay2_c(float *line, float *quantum, unsigned write, unsigned read,
    unsigned previous, unsigned feedback_pos, int interpolate,
    float fraction, double input_l, double input_r, double feedback,
    double *out_l, double *out_r);
#if defined(__SSE2__)
void dsp_delay2_sse2(float *line, float *quantum, unsigned write,
    unsigned read, unsigned previous, unsigned feedback_pos, int interpolate,
    float fraction, double input_l, double input_r, double feedback,
    double *out_l, double *out_r);
#endif
void dsp_delay2(float *line, float *quantum, unsigned write, unsigned read,
    unsigned previous, unsigned feedback_pos, int interpolate,
    float fraction, double input_l, double input_r, double feedback,
    double *out_l, double *out_r);
#endif
