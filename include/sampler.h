#ifndef SAMPLER_H
#define SAMPLER_H

/* One-shot WAV sampler for drum banks.
   Thread-safe: acquire/load from scheduler/generator threads,
   pointer lookup from the consumer thread. The realtime audio
   callback only touches voice-owned pointers, never the cache. */

/* Resolve bank fragment to a directory (absolute path) using the
   banks index. Returns 1 on success. */
int bank_find_dir(const char *frag, char *out, int outlen);

/* Load (or fetch cached) sample for a drum token in a bank dir.
   Returns sample id >= 0, or -1 if none found. */
int smp_acquire(const char *bankdir, const char *token);

/* Pointer lookup by id (brief lock, safe from consumer thread). */
void smp_get(int id, float **buf, int *len);

/* Load an arbitrary bank-relative path (F6 audition). Returns id or -1. */
int smp_acquire_path(const char *relpath);

/* Drop cached dir listings so added/removed files are picked up
   (decoded samples stay valid in memory). */
void smp_invalidate(void);

/* Best sample for a MIDI note in a bank dir (filename note match,
   nearest pitch wins, first-file fallback). Returns id or -1. */
int smp_acquire_note(const char *bankdir, int midi);

/* Base MIDI the sample was recorded at (60 if unknown). */
int smp_base_midi(int id);

#endif
