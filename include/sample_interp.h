#ifndef SAMPLE_INTERP_H
#define SAMPLE_INTERP_H
/* Two-channel linear interpolation used by the scalar sample path. Samples
   are frame-major stereo pairs. */
void sample_interp2_c(const float *samples, int frames, double position,
                      double *left, double *right);
#if defined(__x86_64__) || defined(__i386__)
void sample_interp2_avx2(const float *samples, int frames, double position,
                         double *left, double *right);
#endif
/* Runtime-selected implementation. Loop, reverse and out-of-range cases stay
   with the caller; this is the common forward stereo interpolation. */
void sample_interp2(const float *samples, int frames, double position,
                    double *left, double *right);
#endif
