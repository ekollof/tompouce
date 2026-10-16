#ifndef HARMONY_H
#define HARMONY_H

/* Strudel tonal helpers (strudel.cc/learn/tonal, ireal chord symbols).
   Close-position voicings: root at octave 3 unless the symbol carries one.
   Scale roots also default to octave 3 ("C:minor" == C3 minor). */

/* Chord symbol -> MIDI notes. as_chord treats a bare root ("C") as major,
   which is the chord() track. On note tracks only real qualities ("m7","^7")
   count, so "c7" stays the note C7. Returns count, or 0 if not a chord. */
int harmony_chord(const char *token, int as_chord, int *out, int max);

/* "C:minor", "D4:dorian", "bb:mixolydian", "pentatonic". Returns 1 on success. */
int harmony_scale(const char *spec, int *root_out, int *steps, int max, int *nsteps);

/* Scale degree token: "0", "-2", "7b", "4#". Returns 1 if it is one. */
int harmony_degree(const char *token, int *degree, int *acc);

int harmony_degree_midi(int degree, int acc, int root, const int *steps, int n);

#endif
