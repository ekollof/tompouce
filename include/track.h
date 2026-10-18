#ifndef TRACK_H
#define TRACK_H
#include "common.h"
#include "pattern.h"
#include "event.h"
#include "audio.h"
#include "strudel.h"
#include "javascript.h"

typedef struct Track {
    JsProgram *javascript;
    int visual_requests;
    unsigned compile_generation;
    char keyword[16];
    Node *pattern;
    int pat_off;     /* byte offset of the pattern text in current_code, -1 if none */
    int pat_len;     /* source length of that text */
    int pat_bias;    /* subtract from event src (.slow wraps with a '[') */
    Node *hl_pat[HL_PAT_MAX];
    int hl_at[HL_PAT_MAX];   /* absolute offset in current_code */
    int hl_len[HL_PAT_MAX];
    int hl_n;
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
    double pan;     // constant pan -1..1, 0 center
    Node *gain_pat; // Strudel .gain("<pattern>") queried by event time
    Node *pan_pat;  // .pan pattern, values 0..1 map to -1..1
    Node *lpf_pat;  // .lpf pattern in Hz
    Node *struct_pat;
    Node *mask_pat;
    double off_time;
    int has_off;
    double off_add;
    int off_degrees;
    double sometimes_p;  /* <0 off */
    int sometimes_fast;
    double sometimes_slow;
    double rarely_p;     /* <0 off */
    double rarely_rate;
    double rarely_send;
    double post_degrade; /* <0 off; drop after struct/mask */
    double lpq;          /* <=0 one-pole (or bypass) */
    double lpenv;
    int has_lpenv;
    double lpa;          /* <0 = decay-shaped filter env when lpenv is set */
    double smp_end;      /* <=0 play the whole sample; else fraction */
    double smp_speed;    /* 0 = 1 */
    double delay_amt;    /* <0 none; per-event send into the master delay */
    double delay_time;   /* <0 default (3/16 cycle); else seconds */
    double delay_fb;     /* <0 default 0.5; else 0..0.98 */
    int palindrome;      /* reverse odd cycles (Strudel lastOf(2, rev)) */
    StrudelSig lpf_sig;
    StrudelSig dec_sig;
    StrudelSig end_sig;
    double next_cycle;
    int cycle_index;
    int active;
    int jux;
    int jux_rev;
    int jux_inst;        /* <0 inherit */
    int off_fn;
    double off_slow;
    double off_late;
    int off_inst;        /* <0 inherit */
    Node *off_add_pat;
    SigExpr *sig;        /* note source when pattern is empty */
    Node *speed_pat;     /* patterned .speed(), including negatives */
    Node *rarely_speed;  /* speed pattern applied only on the rarely coin */
} Track;

/* 1 accepted; 0 evaluation failed (the last playable pattern is retained). */
int track_load_from_code(const char *code_buf);

/* arrays */
extern Track tracks[MAX_TRACKS];
extern int track_count;
extern pthread_mutex_t tracks_lock;
extern char *current_code;
/* Copy s into current_code. 0 if malloc fails or s does not fit in an int
   offset. The previous text is left unchanged on failure. */
int current_code_set(const char *s);

#endif
