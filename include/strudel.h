#ifndef STRUDEL_H
#define STRUDEL_H
#include "pattern.h"

/* Continuous control: sine / perlin / rand in 0..1, then .range / .slow / .fast.
   kind 0 = unused, 1 = sine (0.5+0.5*sin), 2 = perlin, 3 = rand. */
typedef struct {
    int kind;
    double slow;
    double fast;
    double lo, hi;
    int has_range;
} StrudelSig;

/* Note-source signal: sine.add(saw.slow(4)).range(0,7).segment(8).
   Evaluated at each hap's span.begin. segment is the event grid of this node. */
#define SIG_SINE 1
#define SIG_SAW 2
#define SIG_COS 3
#define SIG_SQUARE 4
#define SIG_TRI 5
#define SIG_PERLIN 6
#define SIG_RAND 7
#define SIG_NUM 8
#define SIG_ADD 9
#define SIG_MUL 10
typedef struct SigExpr {
    int kind;
    double num;
    double slow;
    double fast;
    double lo, hi;
    int has_range;
    int segment;     /* 0 = one event covering the cycle; else 1..64 */
    int has_extra;   /* superimpose(x => x.add(n)) */
    double extra;
    struct SigExpr *a, *b;
} SigExpr;

/* One workshop-style statement:
     setcpm(45)
     $: note("c e g").s("sawtooth").lpf(800)
     s("hh*16").gain("[.25 1]*4")
     s("bd sd").gain(slider(0.5,0,1,0.05))
   handled=0 means the line is not this syntax (try the track dialect). */

typedef struct {
    int handled;
    int bpm_set;
    double bpm;
    char keyword[16];
    char pattern[4096];
    int pattern_at;    /* byte offset of pattern text in the statement, -1 if none */
    int pattern_len;   /* source length; ignores a .slow() wrap */
    int pattern_bias;  /* columns to subtract when .slow() prefixed '[' */
    /* Other quoted minis (struct, mask, gain, …) highlighted while they play.
       Double quotes and backticks always; single quotes only for the controls
       that are queried over the cycle. */
#define HL_PAT_MAX 8
    Node *hl_pat[HL_PAT_MAX];
    int hl_at[HL_PAT_MAX];
    int hl_len[HL_PAT_MAX];
    int hl_n;
    char inst[32];
    char bank[64];
    char scale[64];
    int rev;
    int fast_repeat;   /* .fast(integer) — track fast, default 1 */
    double gain;
    double lpf;
    double pan;
    double att, dec, sus, rel;
    int has_gain, has_lpf, has_pan, has_env;
    double room;       /* <0 off; master verb */
    double delay;      /* <0 off; per-event delay send */
    double degrade;    /* <0 off; probability an event is dropped */
    char struct_pat[512];
    char mask_pat[512];
    double off_time;   /* cycle fraction; meaningful when has_off */
    int has_off;
    double off_add;
    int off_degrees;   /* 1: add to scale degrees (off came before .scale) */
    double sometimes_p;/* <0 off; Strudel sometimesBy */
    int sometimes_fast;
    double sometimes_slow;
    double rarely_p;   /* <0 off; 0.25 for .rarely */
    double rarely_rate;/* <0 unset */
    double rarely_send;/* <0 unset */
    double lpq;        /* <0 off; biquad Q (.lpq / .resonance) */
    double lpenv;
    int has_lpenv;
    double lpa;        /* <0 off; filter attack seconds */
    double smp_end;    /* <0 default (play the whole sample) */
    double smp_speed;  /* 0 default */
    int palindrome;    /* lastOf(2, rev): reverse odd cycles */
    double delay_time; /* <0 unset; seconds (.delaytime) */
    double delay_fb;   /* <0 unset; 0..0.98 (.delayfeedback) */
    StrudelSig lpf_sig;
    StrudelSig dec_sig;
    StrudelSig end_sig;
    Node *gain_pat;
    Node *pan_pat;
    Node *lpf_pat;
    int jux;             /* .jux(fn) */
    int jux_rev;         /* fn reverses the right copy */
    char jux_inst[32];   /* oscillator on the right copy, "" = inherit */
    int off_fn;          /* .off(time, arrow); has_off stays the scalar path */
    double off_slow;
    double off_late;
    char off_inst[32];
    Node *off_add_pat;   /* add(note("mini")) inside the arrow */
    SigExpr *sig;        /* statement began with sine/saw/…; pattern may be empty */
    Node *speed_pat;     /* .speed("1 2 -1"); 0 smp_speed means this or natural 1 */
    Node *rarely_speed;  /* .rarely(x => x.speed("2 | -2")) */
} StrudelLine;

int strudel_parse(const char *line, StrudelLine *out);
void strudel_line_free(StrudelLine *s);
void sig_free(SigExpr *e);
double sig_eval(const SigExpr *e, double t);
void sig_events(const SigExpr *e, PatEventList *list, int cycle);

#endif
