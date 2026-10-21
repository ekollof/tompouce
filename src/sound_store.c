#include "sound_store.h"
#include "common.h"
#include "sampler.h"
#include <cjson/cJSON.h>
#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <sys/stat.h>
#include <strings.h>
#include <errno.h>

typedef struct { char name[64]; char (*files)[72]; int *midi; int *region; int count; } Imported;
static Imported sounds[USER_SOUND_NAMES];
static int count, initialized;
static unsigned revision;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t load_lock = PTHREAD_MUTEX_INITIALIZER;

static int store_path(char *path, size_t size) {
    char root[1024]; struct stat st;
    user_dir(root,sizeof(root),"XDG_CONFIG_HOME","tompouce",".config/tompouce");
    if (!*root || lstat(root,&st) || !S_ISDIR(st.st_mode)) return 0;
    int n = snprintf(path,size,"%s/sounds",root);
    return n > 0 && (size_t)n < size && !lstat(path,&st) && S_ISDIR(st.st_mode);
}

static int safe_name(const char *name) {
    if (!name || !isalpha((unsigned char)*name) || strlen(name) >= 64) return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (!isalnum(*p) && *p != '_' && *p != '-') return 0;
    return 1;
}

static int safe_file(const char *file) {
    if (!file || strlen(file) > 69 || strlen(file) < 68) return 0;
    for (int i = 0; i < 64; i++) if (!isxdigit((unsigned char)file[i])) return 0;
    return !strcmp(file+64,".wav") || !strcmp(file+64,".mp3") || !strcmp(file+64,".flac");
}

int user_sounds_reload(void) {
    pthread_mutex_lock(&load_lock);
    char root[1200], path[1400];
    cJSON *manifest = NULL;
    int invalid = 0;
    if (store_path(root,sizeof(root))) {
        snprintf(path,sizeof(path),"%s/index.json",root);
        int fd = open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
        if (fd >= 0 || errno != ENOENT) invalid = 1;
        struct stat st;
        if (fd >= 0 && !fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size < 2*1024*1024) {
            size_t length = (size_t)st.st_size, used = 0;
            char *text = malloc(length+1);
            if (text) {
                while (used < length) { ssize_t got = read(fd,text+used,length-used); if (got <= 0) break; used += (size_t)got; }
                text[used] = 0;
                if (used == length) manifest = cJSON_Parse(text);
                if (manifest) invalid = 0;
                free(text);
            }
        }
        if (fd >= 0) close(fd);
    }
    if (invalid) { pthread_mutex_unlock(&load_lock); return -1; }
    Imported *next = calloc(USER_SOUND_NAMES,sizeof(*next));
    if (!next) { cJSON_Delete(manifest); pthread_mutex_unlock(&load_lock); return -1; }
    int n = 0, total = 0;
    if (manifest && (!cJSON_IsObject(manifest) ||
        !cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(manifest,"version")) ||
        (cJSON_GetObjectItemCaseSensitive(manifest,"version")->valuedouble != 1 &&
         cJSON_GetObjectItemCaseSensitive(manifest,"version")->valuedouble != 2) ||
        !cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(manifest,"sounds")))) {
        cJSON_Delete(manifest); free(next); pthread_mutex_unlock(&load_lock); return -1;
    }
    const cJSON *groups = cJSON_GetObjectItemCaseSensitive(manifest,"sounds"), *group;
    cJSON_ArrayForEach(group,groups) {
        if (n >= USER_SOUND_NAMES || !safe_name(group->string) || !cJSON_IsArray(group)) continue;
        int slots = cJSON_GetArraySize(group);
        if (slots < 1 || slots > 1024 || total+slots > 8192) continue;
        char (*files)[72] = calloc((size_t)slots,sizeof(*files));
        int *midi = calloc((size_t)slots,sizeof(*midi));
        int *region = calloc((size_t)slots,sizeof(*region));
        if (!files || !midi || !region) { free(files); free(midi); free(region); continue; }
        int used = 0; const cJSON *item;
        cJSON_ArrayForEach(item,group) {
            const cJSON *file = cJSON_GetObjectItemCaseSensitive(item,"file");
            if (!cJSON_IsString(file) || !safe_file(file->valuestring)) continue;
            /* Pitch/region affect playback grouping; invalid values drop the
               slot. Bank origin only affects replacement and defaults to
               unowned, so it never drops playable audio. */
            int key = 36, area = 0;
            const cJSON *number = cJSON_GetObjectItemCaseSensitive(item,"midi");
            if (number) {
                if (!cJSON_IsNumber(number) || !isfinite(number->valuedouble) ||
                    number->valuedouble < 0 || number->valuedouble > 127 ||
                    floor(number->valuedouble) != number->valuedouble) continue;
                key = (int)number->valuedouble;
            }
            number = cJSON_GetObjectItemCaseSensitive(item,"region");
            if (number) {
                if (!cJSON_IsNumber(number) || !isfinite(number->valuedouble) ||
                    number->valuedouble < 0 || number->valuedouble > 1000000 ||
                    floor(number->valuedouble) != number->valuedouble) continue;
                area = (int)number->valuedouble;
            }
            snprintf(files[used],72,"%s",file->valuestring);
            midi[used] = key; region[used] = area; used++;
        }
        if (!used) { free(files); free(midi); free(region); continue; }
        snprintf(next[n].name,sizeof(next[n].name),"%s",group->string);
        next[n].files = files; next[n].midi = midi; next[n].region = region;
        next[n++].count = used; total += used;
    }
    cJSON_Delete(manifest);
    pthread_mutex_lock(&lock);
    for (int i = 0; i < count; i++) { free(sounds[i].files); free(sounds[i].midi); free(sounds[i].region); }
    memcpy(sounds,next,sizeof(sounds)); count = n; initialized = 1; revision++;
    pthread_mutex_unlock(&lock);
    free(next); pthread_mutex_unlock(&load_lock); return n;
}

static void ensure_store(void) {
    pthread_mutex_lock(&lock); int ready = initialized; pthread_mutex_unlock(&lock);
    if (!ready) {
        (void)user_sounds_reload();
        pthread_mutex_lock(&lock); initialized = 1; pthread_mutex_unlock(&lock);
    }
}

unsigned user_sounds_revision(void) { ensure_store(); pthread_mutex_lock(&lock); unsigned r = revision; pthread_mutex_unlock(&lock); return r; }

int user_sounds_list(UserSoundInfo *out, int capacity) {
    ensure_store(); pthread_mutex_lock(&lock);
    for (int i = 0; out && i < count && i < capacity; i++) {
        snprintf(out[i].name,sizeof(out[i].name),"%.63s",sounds[i].name); out[i].slots = sounds[i].count;
    }
    int n = count; pthread_mutex_unlock(&lock); return n;
}

int user_sounds_acquire(const char *name, int index, double midi) {
    ensure_store(); char file[72] = "";
    int base = 36;
    /* Upstream selects by the raw trigger pitch (drums pass -1, which simply
       lands on the lowest key); only non-finite input falls back to 36. */
    if (!isfinite(midi)) midi = 36;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < count; i++) if (!strcasecmp(sounds[i].name,name)) {
        /* Nearest pitch key wins, first in stored order on ties; the sample
           index wraps within that key's region, matching upstream
           getCommonSampleInfo. Flat banks (every key 36) reduce to the
           previous whole-list wrap. */
        int best = -1;
        double best_diff = 0;
        for (int s = 0; s < sounds[i].count; s++) {
            double diff = fabs((double)sounds[i].midi[s]-midi);
            if (best < 0 || diff < best_diff) { best = s; best_diff = diff; }
        }
        int key = sounds[i].midi[best], area = sounds[i].region[best], run = 0;
        for (int s = 0; s < sounds[i].count; s++)
            if (sounds[i].region[s] == area) run++;
        int slot = run > 0 ? index%run : 0;
        if (slot < 0) slot += run;
        for (int s = 0; s < sounds[i].count; s++) {
            if (sounds[i].region[s] != area) continue;
            if (!slot--) { snprintf(file,sizeof(file),"%s",sounds[i].files[s]); break; }
        }
        base = key;
        break;
    }
    pthread_mutex_unlock(&lock);
    if (!*file) return -2;
    char root[1200], assets[1300], path[1400]; struct stat st;
    if (!store_path(root,sizeof(root))) return -1;
    snprintf(assets,sizeof(assets),"%s/assets",root);
    if (lstat(assets,&st) || !S_ISDIR(st.st_mode)) return -1;
    snprintf(path,sizeof(path),"%s/%s",assets,file);
    return smp_acquire_abs(path,base);
}
