#ifndef SOUND_STORE_H
#define SOUND_STORE_H
#define USER_SOUND_NAMES 256
typedef struct { char name[64]; int slots; } UserSoundInfo;
int user_sounds_reload(void);
unsigned user_sounds_revision(void);
int user_sounds_list(UserSoundInfo *out, int capacity);
/* -2 = no such imported sound; -1 = known sound failed to load. */
int user_sounds_acquire(const char *name, int index);
#endif
