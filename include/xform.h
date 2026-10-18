#ifndef XFORM_H
#define XFORM_H
#include "pattern.h"

/* Ordered Strudel transforms applied to one cycle of events.
   sometimes -> off -> struct -> mask -> degrade -> rarely.
   Scale is applied later, when the token becomes a pitch. */

typedef struct XForm {
    Node *structure;
    Node *mask;
    double off_time;
    int has_off;
    double off_add;
    int off_degrees;      /* add to a numeric degree token */
    double sometimes_p;   /* <0 off */
    int sometimes_fast;
    double sometimes_slow;
    double rarely_p;      /* <0 off */
    double rarely_rate;   /* <0 unset; ignored when rarely_speed is set */
    Node *rarely_speed;   /* mini such as "2 | -2"; negative plays backwards */
    double rarely_send;   /* <0 unset */
    double degrade;       /* <0 off; probability of dropping an event */
    /* .jux(fn): left copy hard-left, fn on the right copy only. */
    int jux;
    int jux_rev;
    int jux_inst;         /* <0 inherit */
    /* .off(time, x => …): stack a time-warped copy. Not the scalar has_off path. */
    int off_fn;
    double off_slow;      /* 1 = no stretch */
    double off_late;      /* cycles, applied after slow */
    int off_inst;         /* <0 inherit */
    Node *off_add_pat;    /* add(note("…")); sampled on the pre-slow timeline */
    Node *pat;            /* source mini, re-expanded for the slowed copy */
    int rev;
    int palindrome;
} XForm;

void xform_events(const XForm *x, PatEventList *list, int cycle);
void xform_mirror(PatEventList *list);
/* right_inst < 0 keeps the track oscillator. Pan is Strudel 0 (left) and 1 (right). */
void xform_jux(PatEventList *list, int rev_right, int right_inst);
/* Append the off-arrow copy for this output cycle. Scalar .off stays in xform_events. */
void xform_off_arrow(const XForm *x, PatEventList *list, int cycle);

/* Strudel palindrome is lastOf(2, rev): odd cycles are reversed.
   A .rev() already on the pattern flips that (rev of rev is forward). */
int xform_wants_rev(int rev, int palindrome, int cycle);

/* Number under `when` (0..1) in this cycle. 0 if the hap is missing or not numeric. */
int xform_number_at(Node *pat, int cycle, double when, double *out);

/* Signal at cycle + when (0..1). kind as in StrudelSig. */
double xform_signal(int kind, double slow, double fast, int has_range,
                    double lo, double hi, int cycle, double when);

#endif
