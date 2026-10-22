#ifndef EVENT_H
#define EVENT_H
#include "common.h"
#include "dsp.h"
#include "convolution.h"
#include "native_synth.h"
#include "osc_output.h"
#include "duck_onset.h"

enum { DUCK_ONSET_STRING=1,DUCK_ONSET_SUMMARY=2 };
typedef struct {
    double onset, attack, depth;
    int string_onset;
    union {
        char onset_text[128];
        const DuckOnsetText *long_onset;
    };
} DuckControl;

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
#define INST_PULSE 16

/* Strudel effect buses. Orbit 1 is the JS default. Legacy events use 0. */
#define N_ORBITS 12
#define N_PATTERN_SCOPES 32

typedef struct StretchBuffer StretchBuffer;

typedef struct {
    unsigned generation; /* 0 = legacy; JS generation must match the queue */
    unsigned scope_mask; /* visualization call sites attached to this pattern */
    int csound_only;     /* scheduled by native renderer; retain UI note history */
    int osc_only;
    int midi_only;       /* dominant MIDI: ALSA note without a local voice */
    int control_only;   /* shared controls with no source/MIDI/history event */
    int eviction_only;  /* failed/rest source still removes a full pool's oldest entry */
    int audio_prepared; /* consumer reserved a timestamped voice before onset */
    int delay_prepared; /* generator accepted shared AudioParam automation */
    int room_prepared;  /* shared convolver selected during source preparation */
    int room_params_valid, room_sample; /* -1 identifies a generated IR */
    int room_rebuild;
    unsigned room_defined; /* explicit IR speed/offset, distinct from defaults */
    double room_speed, room_begin;
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
    /* Dominant MIDI extras, parsed in web send order (program, sysex, CC,
       bend, touch, transport). A rejected extra drops itself and every later
       extra, matching the web scheduler's throw-abort behavior. */
    int midi_has_cc, midi_cc, midi_ccv; /* CC number/value, scaled 0..127 */
    int midi_has_prog, midi_prog;       /* program change 0..127 */
    int midi_has_bend, midi_bend;       /* pitch bend -8192..8191, zero-centered */
    int midi_has_touch, midi_touch;     /* channel aftertouch 0..127 */
    int midi_has_sysex, midi_sysex_id_len, midi_sysex_len;
    unsigned char midi_sysex_id[3];
    unsigned char midi_sysex[256];
    int midi_cmd;         /* 0 none, 1 clock, 2 start, 3 continue, 4 stop */
    int midi_cycle_start; /* whole.begin == 0 transport start */
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
    const StretchBuffer *stretch_buffer;
    const ConvKernel *ir_kernel; /* immutable, prepared outside audio callback */
    const NativeSynth *native_synth; /* immutable native program, prepared by generator */
    double delay_send;  /* 0 dry; >0 feeds this event's orbit delay */
    double delay_time;  /* <0 leave the orbit time; else seconds */
    double delay_fb;    /* <0 leave the orbit feedback; else 0..0.98 */
    int orbit;          /* 0..N_ORBITS-1 */
    unsigned duck_mask; /* bit i selects a target orbit; JS ramps begin 10 ms early */
    double duck_attack; /* seconds to return to full level */
    double duck_depth;  /* 0..1 */
    double duck_onset;  /* seconds from source onset to minimum gain */
    int duck_values;
    DuckControl ducks[N_ORBITS];
    int lfo_tgt;        /* 0 off, 1 freq, 2 gain, 3 lpf, 4 pw */
    int lfo_wave;       /* 0 triangle, 1 sine, 2 saw, 3 square */
    double lfo_hz;
    double lfo_depth;
    DspParams dsp;
} AudioEvent;

#endif
