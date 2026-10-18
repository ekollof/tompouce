#ifndef SOUNDS_H
#define SOUNDS_H
enum { SOUND_SAMPLES, SOUND_MACHINES, SOUND_SYNTHS, SOUND_WAVETABLES, SOUND_USER, SOUND_IMPORT, SOUND_CATEGORIES };
typedef struct { char name[64]; int category, slots, pitched; } SoundInfo;
const char *sounds_category(int category);
int sounds_search(int category, const char *query, SoundInfo *out, int capacity);
unsigned sounds_revision(void);
int sounds_snippet(const SoundInfo *sound, char *out, int capacity);
#endif
