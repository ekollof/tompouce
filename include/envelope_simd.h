#ifndef ENVELOPE_SIMD_H
#define ENVELOPE_SIMD_H
/* Four independent ADSR envelopes. Each lane follows dsp_envelope exactly. */
void dsp_envelope4_c(const double age[4], const double gate[4],
    const double attack[4], const double decay[4], const double sustain[4],
    const double release[4], double output[4]);
#if defined(__x86_64__) || defined(__i386__)
void dsp_envelope4_avx2(const double age[4], const double gate[4],
    const double attack[4], const double decay[4], const double sustain[4],
    const double release[4], double output[4]);
#endif
#endif
