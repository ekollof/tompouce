#ifndef DSP_H
#define DSP_H
#include <cjson/cJSON.h>
double dsp_periodic_gain(int waveform);

/* Resolved event controls. No JS, allocation, or locks in these algorithms. */
enum {
    DSP_LPF, DSP_LPQ, DSP_LPE, DSP_LPA, DSP_LPD, DSP_LPS, DSP_LPR,
    DSP_HPF, DSP_HPQ, DSP_HPE, DSP_HPA, DSP_HPD, DSP_HPS, DSP_HPR,
    DSP_BPF, DSP_BPQ, DSP_BPE, DSP_BPA, DSP_BPD, DSP_BPS, DSP_BPR,
    DSP_ANCHOR, DSP_FTYPE, DSP_VOWEL, DSP_COARSE, DSP_CRUSH, DSP_SHAPE,
    DSP_DISTORT, DSP_POST, DSP_TREM, DSP_TDEPTH, DSP_TSKEW, DSP_TPHASE,
    DSP_TSHAPE, DSP_PHASER, DSP_PHDEPTH, DSP_PHCENTER, DSP_PHSWEEP,
    DSP_PENV, DSP_PATT, DSP_PDEC, DSP_PSUS, DSP_PREL, DSP_PANCHOR,
    DSP_VIB, DSP_VIBMOD, DSP_ROOM, DSP_SIZE, DSP_DRY, DSP_BEGIN, DSP_END,
    DSP_LOOP, DSP_LOOPBEGIN, DSP_LOOPEND, DSP_CUT, DSP_CLIP, DSP_PW,
    DSP_PWRATE, DSP_PWSWEEP, DSP_UNISON, DSP_SPREAD, DSP_DISTTYPE, DSP_DISTVOL,
    DSP_COMP, DSP_COMPRATIO, DSP_COMPKNEE, DSP_COMPATT, DSP_COMPREL, DSP_PCURVE,
    DSP_CHORUS, DSP_SHAPEVOL,
    DSP_LPRATE, DSP_LPDEPTH, DSP_LPDEPTHFREQ, DSP_LPSHAPE, DSP_LPDC, DSP_LPSKEW,
    DSP_HPRATE, DSP_HPDEPTH, DSP_HPDEPTHFREQ, DSP_HPSHAPE, DSP_HPDC, DSP_HPSKEW,
    DSP_BPRATE, DSP_BPDEPTH, DSP_BPDEPTHFREQ, DSP_BPSHAPE, DSP_BPDC, DSP_BPSKEW,
    DSP_ROOMFADE, DSP_ROOMLP, DSP_ROOMDIM,
    DSP_FMBASE, /* 8 operators: index, ratio, A, D, S, R, envelope type, waveform */
    DSP_COUNT = DSP_FMBASE + 8 * 8
};

typedef struct {
    int enabled;
    double p[DSP_COUNT];
    double matrix[8][8];
    double partials[32], phases[32];
    int n_partials;
} DspParams;

typedef struct {
    double b0, b1, b2, a1, a2, z1, z2;
} DspBiquad;

typedef struct {
    DspParams control;
    DspBiquad filter[6], formant[5];
    double formant_gain[5];
    double ladder[4], ladder_g, ladder_k;
    double fm_phase[8], fm_last[8];
    double phaser_z[6], phaser_feedback;
    double saw_phase[16], noise[8], compressor_gain;
    double supersaw_l, supersaw_r;
    float chorus[2048];
    unsigned chorus_pos;
    unsigned noise_seed;
    double held;
    unsigned tick;
    /* Derived once in dsp_init. Controls are immutable per voice, so these
       cannot change during playback; they let the per-sample paths skip work
       that is provably identity (see dsp_frequency/dsp_process). */
    int pitch_active;   /* pitch envelope or vibrato configured */
    int fm_active;      /* at least one FM operator has non-zero index */
    int effects_active; /* dsp_process would change the sample */
} DspState;

void dsp_params_init(DspParams *p);
void dsp_params_read(DspParams *p, const cJSON *value);
void dsp_read_adsr(const cJSON *value, const char *const names[4], const double defaults[4], double out[4]);
double dsp_filter_frequency(const DspParams *params, int type, double age, double gate);
void dsp_init(DspState *state, const DspParams *p);
double dsp_process(DspState *state, double sample, double age, double gate);
double dsp_frequency(DspState *state, double freq, double age, double gate);
double dsp_pitch_multiplier(const DspParams *params, double age, double gate);
double dsp_envelope(double age, double gate, double attack, double decay, double sustain, double release);
double dsp_noise(DspState *state, int color);
double dsp_supersaw(DspState *state, double frequency);
/* waveform 0=saw, 1=pulse, 2=triangle; phase in radians. */
double dsp_oscillator(int waveform, double phase, double frequency, double width);
double dsp_additive(const DspParams *params, double phase, double frequency);
int dsp_supports_control(const char *name);
#endif
