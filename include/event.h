#ifndef EVENT_H
#define EVENT_H
#include "common.h"
#include "dsp.h"
#include "convolution.h"
#include "native_synth.h"
#include "osc_output.h"

#define INST_SINE 0
#define INST_SAW 1
#define INST_SQUARE 2
#define INST_TRI 3
#define INST_NOISE 4
#define INST_KICK 5
#define INST_SNARE 6
#define INST_HH 7
#define INST_CP 8
#define INST_TOM 9
#define INST_WT 10
#define INST_SUPERSAW 11
#define INST_PINK 12
#define INST_BROWN 13
#define INST_ADDITIVE 14
#define INST_NATIVE 15

/* Strudel effect buses. Orbit 1 is the JS default. Legacy events use 0. */
#define N_ORBITS 12
#define N_PATTERN_SCOPES 32

typedef struct {
    unsigned generation; /* 0 = legacy; JS generation must match the queue */
    unsigned scope_mask; /* visualization call sites attached to this pattern */
    int csound_only;     /* scheduled by native renderer; retain UI note history */
    int osc_only;
    OscOutputMessage osc;
    double time;        // absolute seconds
    double duration;    // seconds
    int inst;           // for note tracks
    int wt;             // wavetable index when inst == INST_WT, else -1
    int smp;            // sample id when track has bank, else -1
    double att;         // attack seconds, -1 = default
    double rel;         // release seconds, -1 = default
    int midi_chan;      // MIDI channel 0-15, -1 = off
    int midi_note;      // MIDI note for midi out, -1 = none
    int smp_gate;       // gate sampled notes by duration (melodic banks)
    int is_drum;        // 1 if drum token
    char token[64];     // sound name for drum or note name
    double freq;        // for note
    double gain;
    double pan;
    int has_pan;
    double waveform_gain;
    double dec;         /* decay seconds, <0 default */
    double sus;         /* sustain 0..1, <0 default */
    double pw;          /* pulse width, <0 default square */
    double lpf;         /* per-voice lowpass Hz, 0 = off */
    double lpq;         /* <=0 one-pole; >0 resonant biquad Q.
                           An lpf with no .lpq uses Q=1. */
    double lpenv;       /* octaves of filter-envelope depth */
    int has_lpenv;
    double lpa;         /* filter attack seconds, <0 = decay shape */
    double smp_end;     /* <=0 whole sample; else fraction of length */
    double smp_speed;   /* 0 = natural 1; negative reverses the buffer */
    double stretch;     /* native pitch factor; 0 = disabled/legacy, 1 = unity */
    const ConvKernel *ir_kernel; /* immutable, prepared outside audio callback */
    const NativeSynth *native_synth; /* immutable native program, prepared by generator */
    double delay_send;  /* 0 dry; >0 feeds this event's orbit delay */
    double delay_time;  /* <0 leave the orbit time; else seconds */
    double delay_fb;    /* <0 leave the orbit feedback; else 0..0.98 */
    int orbit;          /* 0..N_ORBITS-1 */
    unsigned duck_mask; /* bit i ducks orbit i on this note's onset */
    double duck_attack; /* seconds to return to full level */
    double duck_depth;  /* 0..1 */
    int lfo_tgt;        /* 0 off, 1 freq, 2 gain, 3 lpf, 4 pw */
    int lfo_wave;       /* 0 triangle, 1 sine, 2 saw, 3 square */
    double lfo_hz;
    double lfo_depth;
    DspParams dsp;
} AudioEvent;

#endif
