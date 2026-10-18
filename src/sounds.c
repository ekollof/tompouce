#include "sounds.h"
#include "websamp.h"
#include "sound_store.h"
#include "wavetable.h"
#include "common.h"
#include "banks.h"
#include "gm_synth.h"
#include <limits.h>
#include <strings.h>
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
    if (gm_synth_available()) for (int i=0;i<gm_synth_count() && used<LIMIT;i++) {
        int duplicate=0;
        for (int j=0;j<used;j++) if (!strcmp(rows[j].name,gm_synth_name(i))) { duplicate=1; break; }
        if (duplicate) continue;
        snprintf(rows[used].name,sizeof(rows[used].name),"%s",gm_synth_name(i));
        rows[used].category=SOUND_SYNTHS; rows[used].pitched=1; rows[used++].slots=1;
    }
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

static int machine_bank(const SoundInfo *sound, char bank[64]) {
    if (sound->category!=SOUND_MACHINES) return 0;
    const char *split=strrchr(sound->name,'_');
    if (!split || split==sound->name || !split[1]) return 0;
    size_t len=(size_t)(split-sound->name);
    memcpy(bank,sound->name,len); bank[len]=0; return 1;
}

int sounds_browse(int category, const char *bank, const char *query, SoundBrowserRow *out, int capacity) {
    SoundInfo *sounds=calloc(4096,sizeof(*sounds));
    if (!sounds) return -1;
    int count=sounds_search(category==SOUND_CATEGORIES?-1:category,NULL,sounds,4096);
    if (count<0) { free(sounds); return -1; }
    if (count>4096) count=4096;
    char seen[4096][64]; int nseen=0, total=0;
    for (int i=0;i<count;i++) {
        if (category==SOUND_CATEGORIES && sounds[i].category!=SOUND_SAMPLES && sounds[i].category!=SOUND_MACHINES &&
            !(sounds[i].category==SOUND_WAVETABLES && !strncmp(sounds[i].name,"wt_",3))) continue;
        char group[64]; int grouped=machine_bank(&sounds[i],group);
        if (bank && *bank) {
            if (!grouped || strcmp(group,bank) || !matches(sounds[i].name,query)) continue;
        } else if (!matches(sounds[i].name,query) && !(grouped && matches(group,query))) continue;
        SoundBrowserRow row={0}; row.sound=sounds[i];
        snprintf(row.name,sizeof(row.name),"%s",sounds[i].name);
        if (grouped && (!bank || !*bank)) {
            int duplicate=0;
            for (int j=0;j<nseen;j++) if (!strcmp(seen[j],group)) { duplicate=1; break; }
            if (duplicate) continue;
            snprintf(seen[nseen++],64,"%s",group);
            snprintf(row.name,sizeof(row.name),"%s",group); row.kind=SOUND_ROW_BANK;
            row.sound.slots=0;
            for (int j=0;j<count;j++) {
                char other[64];
                if (machine_bank(&sounds[j],other) && !strcmp(group,other)) row.sound.slots++;
            }
        }
        if (out && total<capacity) out[total]=row;
        total++;
    }
    free(sounds);
    if ((category<0 || category==SOUND_CATEGORIES) && (!bank || !*bank) && banks_ready()) {
        char root[PATH_MAX];
        if (!realpath(banks_root_dir(),root)) return total;
        int files=banks_search("",NULL,0);
        const char **paths=calloc((size_t)files+1,sizeof(*paths));
        if (!paths) return total;
        int found=banks_search("",paths,files);
        if (found<files) files=found;
        for (int i=0;i<files;i++) {
            const char *dot=strrchr(paths[i],'.');
            if (!dot || (strcasecmp(dot,".wav") && strcasecmp(dot,".mp3") && strcasecmp(dot,".flac") && strcasecmp(dot,".fla"))) continue;
            char absolute[PATH_MAX];
            int n=snprintf(absolute,sizeof(absolute),"%s/%s",root,paths[i]);
            if (n<0 || (size_t)n>=sizeof(absolute) || websamp_manifest_contains(absolute) || !matches(paths[i],query)) continue;
            if (out && total<capacity) {
                memset(&out[total],0,sizeof(out[total])); out[total].kind=SOUND_ROW_FILE;
                snprintf(out[total].name,sizeof(out[total].name),"%s",paths[i]); out[total].sound.slots=1;
            }
            total++;
        }
        free(paths);
    }
    return total;
}

int sounds_browser_snippet(const SoundBrowserRow *row, char *out, int capacity) {
    if (!row || !out || capacity<=0 || row->kind==SOUND_ROW_BANK) return 0;
    if (row->kind==SOUND_ROW_SOUND) return sounds_snippet(&row->sound,out,capacity);
    cJSON *name=cJSON_CreateString(row->name);
    char *quoted=name?cJSON_PrintUnformatted(name):NULL; cJSON_Delete(name);
    if (!quoted) return 0;
    /* pure keeps spaces and mini-notation punctuation inside one filename. */
    int n=snprintf(out,(size_t)capacity,"s(pure(%s))",quoted); free(quoted);
    return n>0 && n<capacity;
}
