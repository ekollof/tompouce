#include "sounds.h"
#include "websamp.h"
#include "sound_store.h"
#include "wavetable.h"
#include "common.h"
#include <ctype.h>
#include <cjson/cJSON.h>

const char *sounds_category(int category) {
    static const char *names[] = {"samples","drum-machines","synths","wavetables","user","import-sounds"};
    return category >= 0 && category < SOUND_CATEGORIES ? names[category] : "all";
}

static int matches(const char *name, const char *query) {
    if (!query || !*query) return 1;
    for (const char *start = name; *start; start++) {
        const char *a = start, *b = query;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
}

static int compare(const void *a, const void *b) { return strcmp(((const SoundInfo *)a)->name,((const SoundInfo *)b)->name); }

int sounds_search(int category, const char *query, SoundInfo *out, int capacity) {
    enum { LIMIT = 4096 };
    SoundInfo *rows = calloc(LIMIT,sizeof(*rows));
    if (!rows) return -1;
    int used = websamp_list_sounds(rows,LIMIT);
    if (used < 0) used = 0;
    if (used > LIMIT) used = LIMIT;
    UserSoundInfo imported[USER_SOUND_NAMES];
    int count = user_sounds_list(imported,USER_SOUND_NAMES);
    for (int i = 0; i < count && used < LIMIT; i++) {
        int duplicate = 0;
        for (int j = 0; j < used; j++) if (!strcmp(rows[j].name,imported[i].name)) {
            if (rows[j].category != SOUND_USER) {
                rows[j].category = SOUND_USER; rows[j].slots = imported[i].slots; rows[j].pitched = 0;
            }
            duplicate = 1; break;
        }
        if (duplicate) continue;
        snprintf(rows[used].name,sizeof(rows[used].name),"%.63s",imported[i].name);
        rows[used].category = SOUND_USER; rows[used++].slots = imported[i].slots;
    }
    const char *synths[] = {"sine","triangle","sawtooth","square","pulse","supersaw","white","pink","brown",
        "zzfx","z_sine","z_triangle","z_sawtooth","z_square","z_tan","z_noise","bytebeat"};
    for (size_t i = 0; i < sizeof(synths)/sizeof(synths[0]) && used < LIMIT; i++) {
        snprintf(rows[used].name,sizeof(rows[used].name),"%s",synths[i]);
        rows[used].category = SOUND_SYNTHS; rows[used].pitched = 1;
        rows[used++].slots = !strcmp(synths[i],"bytebeat")?15:1;
    }
    /* wt_init is called at engine startup; catalog lookup never mutates it. */
    for (int i = 0; i < wt_count() && used < LIMIT; i++) {
        snprintf(rows[used].name,sizeof(rows[used].name),"wt:%s",wt_name(i));
        rows[used].category = SOUND_WAVETABLES; rows[used].pitched = 1; rows[used++].slots = 1;
    }
    qsort(rows,(size_t)used,sizeof(*rows),compare);
    int total = 0;
    for (int i = 0; i < used; i++) if ((category < 0 || rows[i].category == category) && matches(rows[i].name,query)) {
        if (out && total < capacity) out[total] = rows[i];
        total++;
    }
    free(rows); return total;
}

unsigned sounds_revision(void) { return websamp_revision()+user_sounds_revision(); }

int sounds_snippet(const SoundInfo *sound, char *out, int capacity) {
    if (!sound || !out || capacity <= 0) return 0;
    cJSON *name = cJSON_CreateString(sound->name);
    if (!name) return 0;
    char *quoted = cJSON_PrintUnformatted(name); cJSON_Delete(name);
    if (!quoted) return 0;
    int n = snprintf(out,(size_t)capacity,sound->pitched ? "note(\"c4\").s(%s)" : "s(%s)",quoted);
    free(quoted); return n > 0 && n < capacity;
}
