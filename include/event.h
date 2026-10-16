#ifndef EVENT_H
#define EVENT_H
#include "common.h"

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

typedef struct {
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
    double dec;         /* decay seconds, <0 default */
    double sus;         /* sustain 0..1, <0 default */
    double pw;          /* pulse width, <0 default square */
    double lpf;         /* per-voice lowpass Hz, 0 = off */
    int lfo_tgt;        /* 0 off, 1 freq, 2 gain, 3 lpf, 4 pw */
    int lfo_wave;       /* 0 triangle, 1 sine, 2 saw, 3 square */
    double lfo_hz;
    double lfo_depth;
} AudioEvent;

#endif