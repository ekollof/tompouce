#ifndef SOUNDS_H
#define SOUNDS_H
enum { SOUND_SAMPLES, SOUND_MACHINES, SOUND_SYNTHS, SOUND_WAVETABLES, SOUND_USER, SOUND_IMPORT, SOUND_CATEGORIES };
typedef struct { char name[64]; int category, slots, pitched; } SoundInfo;
const char *sounds_category(int category);
int sounds_search(int category, const char *query, SoundInfo *out, int capacity);
unsigned sounds_revision(void);
int sounds_snippet(const SoundInfo *sound, char *out, int capacity);
enum { SOUND_ROW_SOUND, SOUND_ROW_BANK, SOUND_ROW_FILE };
typedef struct { SoundInfo sound; char name[512]; int kind; } SoundBrowserRow;
/* F6's manifest-first view. bank is a drum-machine name, or empty at root.
 * SOUND_CATEGORIES selects manifest banks plus uncovered local audio files. */
int sounds_browse(int category, const char *bank, const char *query, SoundBrowserRow *out, int capacity);
int sounds_browser_snippet(const SoundBrowserRow *row, char *out, int capacity);
#endif
