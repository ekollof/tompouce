#ifndef WAVETABLE_H
#define WAVETABLE_H

#define WT_SIZE 2048
#define WT_MAX 16

/* Procedural single-cycle wavetables, bandlimited via additive synthesis.
   Manifest-backed Strudel wt_ banks use the sampler, independently of this
   fixed procedural table registry. */
void wt_init(void);
int wt_count(void);
const char *wt_name(int idx);
int wt_find(const char *name); /* -1 if missing */
/* phase in [0,1); linear interpolation; returns ~[-1,1] */
double wt_sample(int idx, double phase);

#endif
