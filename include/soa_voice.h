#ifndef SOA_VOICE_H
#define SOA_VOICE_H
#include "dsp.h"
#include "voice_soa_dsp.h"

#define SOA_VOICE_LANES 16
#define SOA_VOICE_GROUPS (SOA_VOICE_LANES/4)

typedef struct {
    int active;
    const float *buf;
    double pos, step, amp, age, gate, att, dec, sus, rel;
    int channels, smplen;
    unsigned tick;
    DspParams control;
} SoaVoiceLane;

typedef struct {
    SoaVoiceLane lanes[SOA_VOICE_LANES];
    DspBiquad4 filter_l[SOA_VOICE_GROUPS], filter_r[SOA_VOICE_GROUPS];
} SoaVoiceBank;

void soa_voice_bank_clear(SoaVoiceBank *bank);
/* Returns the lane index, or -1 when full. Caller must pass filter-only
   controls and a sample buffer. */
int soa_voice_bank_add(SoaVoiceBank *bank, const DspParams *control,
    const float *pcm, double position, double step, double amp,
    double age, double gate, double att, double dec, double sus, double rel,
    int channels, int frames, unsigned tick);
/* Render `frames` samples. Output is frame-major: out[frame*16+lane]. */
void soa_voice_bank_process(SoaVoiceBank *bank, unsigned frames,
    double *out_l, double *out_r);

#endif
