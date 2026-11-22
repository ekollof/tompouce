#ifndef SOA_VOICE_H
#define SOA_VOICE_H
#include "dsp.h"
#include "voice_soa_dsp.h"

/* From-allocation SoA render lanes. A voice that qualifies copies its compact
   per-sample state once, at the first block boundary after its onset; the lane
   then owns source position, envelope, oscillator phase, LPF coefficients and
   state, and routing constants until the voice ends. */
#define SOA_VOICE_LANES 128
#define SOA_VOICE_GROUPS (SOA_VOICE_LANES/4)
#define SOA_VOICE_FRAMES 4

enum {
    SOA_KIND_SAMPLE = 0,
    SOA_KIND_SINE,
    SOA_KIND_SAW,
    SOA_KIND_SQUARE,
    SOA_KIND_TRI
};

typedef struct {
    void *owner;
    unsigned long long onset_serial;
    const float *buf;
    int kind;        /* SOA_KIND_* */
    int channels;    /* sample channels, 1 or 2 */
    int smplen;
    int orbit;
    int out_target;
    int route_dsp;
    int stereo_filter;
    double pos, step;
    double amp, wave_gain;
    /* Raw values feed the sampled-source envelope; the clamped values feed
       the oscillator envelope, matching the scalar branch. */
    double age, gate, att, dec, sus, rel;
    double env_att, env_dec, env_sus, env_rel;
    double env, raw_rel;
    double note_off_time, release_time, release_start_level;
    double freq, phase, phase_inc, pw;
    /* Single active biquad (DSP_LPF/HPF/BPF) controls. */
    int filter_type;
    double filter_cutoff, filter_q, filter_env, filter_anchor;
    double filter_a, filter_d, filter_s, filter_r;
    unsigned tick;
    unsigned rendered; /* per-frame output mask, reset each process call */
    int released, ended, active, filterless;
    double ll, lr, rl, rr, left, right, dry, delay, room, room_size;
    unsigned scope_mask, scope_generation;
    /* Filter coefficients and state copied at admission. */
    DspBiquad biquad_l, biquad_r;
} SoaVoiceLane;

typedef struct {
    SoaVoiceLane lanes[SOA_VOICE_LANES];
    DspBiquad4 filter_l[SOA_VOICE_GROUPS], filter_r[SOA_VOICE_GROUPS];
    unsigned char active_group[SOA_VOICE_GROUPS];
} SoaVoiceBank;

void soa_voice_bank_clear(SoaVoiceBank *bank);
/* Returns the lane index, or -1 when full. */
int soa_voice_bank_add(SoaVoiceBank *bank, const SoaVoiceLane *lane);
void soa_voice_bank_remove(SoaVoiceBank *bank, int lane);
/* Render `frames` (at most SOA_VOICE_FRAMES) at `times`. Output is
   frame-major: out[frame*SOA_VOICE_LANES+lane]. */
void soa_voice_bank_process(SoaVoiceBank *bank, unsigned frames,
    const double *times, double *out_l, double *out_r);
/* Mirrors dsp_filter_frequency(type 0) for cached controls. */
double soa_voice_filter_frequency(const SoaVoiceLane *lane, double age);

#endif
