#ifndef TRACK_H
#define TRACK_H
#include "common.h"
#include "pattern.h"
#include "event.h"
#include "audio.h"

typedef struct Track {
    char keyword[16];
    Node *pattern;
    int fast_repeat;
    int slow_stride;
    int rev;
    double gain;
    int inst;
    int wt; // wavetable index when inst == INST_WT, else -1
    char bankfrag[64]; // unresolved bank name from | bank
    char bank[256]; // sample bank dir (absolute) for drum tracks, "" = none
    int mute;       // | mute: silent
    int solo;       // | solo: only solo tracks sound
    int midi_chan;  // | midi N: also emit MIDI on channel N, -1 = off
    double att;     // | att X: attack seconds, -1 = default
    double rel;     // | rel X: release seconds, -1 = default
    double dec;     // | decay X: decay seconds, -1 = default
    double sus;     // | sustain X: sustain level, -1 = default
    double pw;      // | pw X: square pulse width, -1 = default
    double lpf_hz;  // | lpf Hz: per-voice lowpass, 0 = off
    int semi;       // | transpose N
    int chord;      // chord-symbol track (ireal names)
    int lfo_tgt;    // 0 off, 1 freq, 2 gain, 3 lpf, 4 pw
    int lfo_wave;   // 0 triangle, 1 sine, 2 saw, 3 square
    double lfo_hz;
    double lfo_depth;
    int scale_root;
    int scale_n;
    int scale_steps[12];
    Node *arp;      // | arp <mini-notation of indices>
    int shuffle;    // | shuffle: permute steps each cycle
    double degrade; // | degrade [X]: drop each event w.p. X (0.5 bare), -1 = off
    double next_cycle;
    int cycle_index;
    int active;
} Track;

void track_load_from_code(const char *code_buf);

/* arrays */
extern Track tracks[MAX_TRACKS];
extern int track_count;
extern pthread_mutex_t tracks_lock;
extern char current_code[65536];

#endif