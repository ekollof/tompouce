#ifndef SIGNAL_WAVE_H
#define SIGNAL_WAVE_H

/* Legacy continuous Strudel waveforms. Precise RNG mode stays in JavaScript. */
enum {
    SIGNAL_SAW = 0,
    SIGNAL_ISAW,
    SIGNAL_SINE2,
    SIGNAL_SQUARE,
    SIGNAL_PERLIN,
    SIGNAL_BERLIN,
    SIGNAL_RAND,
    SIGNAL_WAVES
};

/* Writes one raw sample per time. 0 on success, -1 for an unknown kind. */
int signal_wave_fill(const int *kind, const double *time, int count, double seed, double *out);
#endif
