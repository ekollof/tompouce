#ifndef DSP_H
#define DSP_H
#include <cjson/cJSON.h>
/* Maximum chorus delay is SR*0.025 (1102.5 frames at 44.1 kHz); the ring is
   one power-of-two-free size that still exceeds it, so reads stay in range
   without masking. */
#define DSP_CHORUS_LEN 1152
#define DSP_SUPERSAW_VOICES 100

/* Indices into DspParams.gen_lfo: per-sample generic-modulation offsets,
   driven by voice LFO slots (see voice_lfo_step_all). */
enum {
    GEN_TREM_RATE, GEN_TREM_GAIN, GEN_TREM_SKEW, GEN_TREM_SHAPE,
    GEN_COARSE, GEN_CRUSH, GEN_SHAPE, GEN_SHAPEVOL,
    GEN_DISTORT, GEN_DISTVOL, GEN_DISTTYPE,
    GEN_PHCENTER, GEN_PHSWEEP, GEN_PHDEPTH, GEN_PHRATE,
    GEN_COMP, GEN_COMPRATIO, GEN_COMPKNEE, GEN_COMPATT, GEN_COMPREL,
    GEN_POSTGAIN, GEN_VIBMOD, GEN_PW, GEN_PWRATE, GEN_PWSWEEP, GEN_VOWEL,
    GEN_DETUNE,
    GEN_DELAY, GEN_ROOM,
    GEN_WTRATE, GEN_WTDEPTH, GEN_WTSKEW, GEN_WTSHAPE,
    GEN_WARPRATE, GEN_WARPDEPTH, GEN_WARPSKEW, GEN_WARPSHAPE,
    GEN_FMI1, GEN_FMI2, GEN_FMI3, GEN_FMI4, GEN_FMI5, GEN_FMI6, GEN_FMI7, GEN_FMI8,
    GEN_FMH1, GEN_FMH2, GEN_FMH3, GEN_FMH4, GEN_FMH5, GEN_FMH6, GEN_FMH7, GEN_FMH8,
    GEN_DELAYTIME, GEN_DELAYFB,
    GEN_VIB, GEN_SPREAD,
    GEN_DJF, GEN_WTPOS, GEN_WARPPOS,
    NGEN
};

/* Voice LFO slot targets for DSP-param modulation start here (see GEN_*). */
#define GEN_TGT_BASE 12
typedef struct DspPeriodic DspPeriodic;
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
    DSP_LPCURVE,
    DSP_HPRATE, DSP_HPDEPTH, DSP_HPDEPTHFREQ, DSP_HPSHAPE, DSP_HPDC, DSP_HPSKEW,
    DSP_HPCURVE,
    DSP_BPRATE, DSP_BPDEPTH, DSP_BPDEPTHFREQ, DSP_BPSHAPE, DSP_BPDC, DSP_BPSKEW,
    DSP_BPCURVE,
    DSP_ROOMFADE, DSP_ROOMLP, DSP_ROOMDIM,
    DSP_FMBASE, /* 8 operators: index, ratio, A, D, S, R, envelope type, waveform */
    DSP_COUNT = DSP_FMBASE + 8 * 8
};

typedef struct {
    int enabled;
    double p[DSP_COUNT];
    double gen_lfo[NGEN]; /* per-sample generic-modulation offsets, voice-driven (see GEN_*) */
    double matrix[8][9]; /* source -> carrier (0) or operator (1..8) */
    signed char fm_gen_edge[8]; /* registered generic-fmi edge per operator: native matrix column + 1, 0 = none or unconnected (last-edge-wins, matching upstream) */
    double partials[32], phases[32];
    double supersaw_detune; /* total unison width in semitones, not pitch cents */
    unsigned fm_envelopes;
    double noise_mix;
    const float *source_noise;
    /* White, pink and brown modulator buffers. Null unless that wave is used. */
    const float *fm_noise[3];
    const DspPeriodic *periodic;
    int periodic_kind, periodic_numeric;
    int n_partials;
    int lfo_sample_rate; /* generic cutoff LFO needs per-sample filter updates */
    int vib_lfo; /* generic vib-rate LFO: integrate the vibrato phase */
    double lfo_start_time; /* absolute node start; -1 disables the quantum hold */
    double q_lfo[3]; /* per-sample generic resonance offset, voice-driven */
} DspParams;

typedef struct {
    double b0, b1, b2, a1, a2, z1, z2;
    /* Direct-form-I history used when coefficients update per sample. */
    double x1, x2, y1, y2;
} DspBiquad;

typedef struct {
    double b0[2], b1[2], b2[2], a1[2], a2[2], z1[2], z2[2];
} DspBiquad2;

typedef struct {
    DspParams control;
    DspBiquad filter[6], formant[5];
    double formant_gain[5];
    double ladder[4], ladder_g, ladder_k;
    double fm_phase[8];
    float fm_buffer[8][128];
    /* Integrated vibrato-oscillator phase (radians) for rate-modulated
       vibrato; the deployed oscillator integrates its frequency AudioParam
       per sample. Selected by control.vib_lfo when a generic vib-rate LFO
       exists; otherwise the closed form below is bit-identical legacy. */
    double vib_phase;
    double fm_noise_pos[8];
    unsigned fm_order[8], fm_count, fm_frame;
    unsigned fm_initial_frames;
    double phaser_z[6], phaser_feedback;
    double phaser_lfo_phase; /* absolute worklet phase base (cycles) */
    double phaser_fm_phase; /* accumulated generic-LFO rate offset (cycles) */
    double trem_fm_phase; /* accumulated generic-LFO rate offset (cycles) */
    int lfo_switched;
    double saw_phase[DSP_SUPERSAW_VOICES], saw_ratio[DSP_SUPERSAW_VOICES];
    double noise[8], compressor_gain;
    double supersaw_pan_l, supersaw_pan_r, supersaw_gain;
    unsigned supersaw_voices;
    double pulse[6], pulse_lfo_phase;
    unsigned pulse_frame;
    double source_noise_position;
    float periodic_frequency, periodic_blend;
    unsigned periodic_range;
    double supersaw_l, supersaw_r;
    /* Ring just long enough for the maximum chorus delay (SR * 0.025 at
       44.1 kHz), rounded up; the extra 1024 samples of the old 2048 ring
       were never readable and cost 2 MiB across the voice pool. */
    float chorus[DSP_CHORUS_LEN];
    unsigned chorus_pos;
    unsigned noise_seed;
    double held;
    unsigned tick;
    /* Oscillator a-rate automation alignment for its first render quantum. */
    double pitch_onset_fraction, pitch_quantum_offset;
    unsigned pitch_initial_frames;
    /* Derived once in dsp_init. Controls are immutable per voice, so these
       cannot change during playback; they let the per-sample paths skip work
       that is provably identity (see dsp_frequency/dsp_process). */
    int pitch_active;   /* pitch envelope or vibrato configured */
    int fm_active;      /* at least one FM operator has non-zero index */
    int effects_active; /* dsp_process would change the sample */
} DspState;

void dsp_params_init(DspParams *p);
void dsp_params_read(DspParams *p, const cJSON *value);
/* Canonical numeric DSP parameter lookup, shared with FX(...) replay. */
int dsp_parameter_lookup(const char *name, int *index, double *minimum,
                         double *maximum);
void dsp_read_adsr(const cJSON *value, const char *const names[4], const double defaults[4], double out[4]);
double dsp_filter_frequency(const DspParams *params, int type, double age, double gate);
/* Same computation from cached cutoff/envelope controls. */
double dsp_filter_frequency_values(double cutoff, double depth, double anchor,
    double attack, double decay, double sustain, double release,
    double age, double gate);
/* Coefficients for a cached cutoff/Q pair and biquad type. */
void dsp_filter_q(DspBiquad *filter, int type, double hz, double q);
/* Scalar biquad step with the same operation order and non-finite reset as
   dsp_process. Used by post-source FX filter stages. */
double dsp_biquad_process(DspBiquad *filter, double sample);
/* Returns the single active filter type (0 low-pass, 1 high-pass, 2 band-pass)
   when no other per-sample effect can change the signal, else -1. */
int dsp_single_filter(const DspState *state);
void dsp_filter_coefficients(const DspParams *params, int type, double hz,
    DspBiquad *filter);
void dsp_init(DspState *state, const DspParams *p);
double dsp_process(DspState *state, double sample, double age, double gate);
int dsp_process_stereo_filter(DspState *left, DspState *right,
    double xl, double xr, double age, double gate, double *yl, double *yr);
/* Persistent two-lane state: coefficients update every 32 samples and the
   filter state is carried in `state`, not rebuilt per sample. */
int dsp_stereo_filter_step(DspState *left, DspState *right, DspBiquad2 *state,
    int type, double xl, double xr, double age, double gate,
    double *yl, double *yr);
#ifdef TOMPOUCE_RENDER_PROFILE
void dsp_profile_stats(unsigned long long *calls, unsigned long long *total_cycles,
                       unsigned long long *max_cycles);
void dsp_envelope_profile_stats(unsigned long long *calls,
                                unsigned long long *total_cycles);
#endif
double dsp_frequency(DspState *state, double freq, double age, double gate);
/* Per-frame generic FM offsets for the next quantum refill: rows 0..7 are
   GEN_FMI1..8, rows 8..15 GEN_FMH1..8, each 128 frames (NULL selects the
   legacy held value). A modulator OscillatorNode's frequency AudioParam is
   a-rate, so the LFO bends it every sample; the quantum fill must use the
   per-frame worklet output, not the sample-held gen_lfo value. The
   silent-head pre-roll keeps the held value: those frames predate the node
   start, where the deployed worklet output is zero. */
double dsp_frequency_traj(DspState *state, double freq, double age, double gate,
    const double *fm_traj);
/* Sine wavetable gain: 1 below Nyquist/2^(1/3), then 3*log2(Nyquist/|f|),
   and 0 at ±Nyquist. Phase still advances at the clamped frequency. */
float dsp_oscillator_nyquist_gain(float frequency);
/* OscillatorNode a-rate automation starts at the first quantum's array head,
   even when audible output begins part-way through that quantum. */
void dsp_oscillator_onset(DspState *state, double onset);
double dsp_pitch_multiplier(const DspParams *params, double age, double gate);
double dsp_sample_pitch_multiplier(const DspParams *params,double age,double gate,double first_time);
double dsp_envelope(double age, double gate, double attack, double decay, double sustain, double release);
double dsp_noise(DspState *state, int color);
/* Voice preparation only. Independent phases avoid a coherent onset burst. */
void dsp_supersaw_init(DspState *state, unsigned seed);
double dsp_supersaw(DspState *state, double frequency);
/* waveform 0=saw, 1=pulse, 2=triangle; phase in radians. */
double dsp_oscillator(int waveform, double phase, double frequency, double width);
/* Preparation only; immutable, bounded process-lifetime caches. */
int dsp_prepare_periodic(DspParams *params);
const float *dsp_prepare_noise(int color);
/* Preparation only. Noise-waveform operators share the source noise buffers. */
int dsp_prepare_fm(DspParams *params);
double dsp_additive(DspState *state, double phase, double frequency);
double dsp_source_noise(DspState *state);
double dsp_pulse(DspState *state, double frequency);
int dsp_supports_control(const char *name);
#endif
