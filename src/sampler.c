#include "sampler.h"
#include "banks.h"
#define MINIMP3_IMPLEMENTATION
#define DR_FLAC_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif
#include "minimp3.h"
#include "dr_flac.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>
#include <math.h>

#define SMP_MAX_SAMPLES 256
#define SMP_MAX_LEN (44100 * 4)
#define SMP_MAX_BANKS 32
#define SMP_MAX_FILES 2048

typedef struct {
    float *buf;
    int len;
    int base_midi; /* recorded pitch, 60 if unknown */
    char path[1024];
} Sample;

typedef struct {
    char dir[1024];
    char *files[SMP_MAX_FILES]; /* basenames, sorted */
    int nfiles;
} BankCache;

static Sample samples[SMP_MAX_SAMPLES];
static int n_samples = 0;
static BankCache banks[SMP_MAX_BANKS];
static int n_banks = 0;
static pthread_mutex_t smp_lock = PTHREAD_MUTEX_INITIALIZER;

int bank_find_dir(const char *frag, char *out, int outlen) {
    if (!frag || !frag[0] || !out || outlen <= 0) return 0;
    banks_ensure_default();
    /* find first indexed file whose directory contains frag (case-insensitive) */
    char low[256];
    size_t fl = strlen(frag);
    if (fl >= sizeof(low)) fl = sizeof(low) - 1;
    for (size_t i = 0; i < fl; i++) low[i] = (char)tolower((unsigned char)frag[i]);
    low[fl] = 0;
    /* walk entries via search with empty query is wasteful; use targeted search */
    const char *hits[64] = {0};
    int total = banks_search(frag, hits, 64);
    (void)total;
    /* Collect distinct candidate dirs, then prefer the one that is a
       proper prefix-parent of the most others (machine root beats
       per-drum subdirs); tie-break: shallower, then first. */
    char cand[64][1024];
    int ncand = 0;
    for (int i = 0; i < 64 && hits[i]; i++) {
        const char *rel = hits[i];
        const char *slash = strrchr(rel, '/');
        if (!slash) continue;
        char dirpart[1024];
        size_t dl = (size_t)(slash - rel);
        if (dl == 0 || dl >= sizeof(dirpart)) continue;
        memcpy(dirpart, rel, dl);
        dirpart[dl] = 0;
        char lowd[1024];
        for (size_t k = 0; k <= dl; k++) lowd[k] = (char)tolower((unsigned char)dirpart[k]);
        if (!strstr(lowd, low)) continue;
        /* candidate: this dir plus parents (while they still contain frag),
           so machine roots compete with per-drum subdirs */
        char up[1024];
        memcpy(up, dirpart, dl + 1);
        for (int lvl = 0; lvl < 8; lvl++) {
            int dup = 0;
            for (int c = 0; c < ncand; c++)
                if (strcmp(cand[c], up) == 0) { dup = 1; break; }
            if (!dup && ncand < 64) {
                memcpy(cand[ncand], up, strlen(up) + 1);
                ncand++;
            }
            char *s = strrchr(up, '/');
            if (!s) break;
            *s = 0;
            if (!*up) break;
            char lowu[1024];
            size_t ul = strlen(up);
            for (size_t k = 0; k <= ul; k++) lowu[k] = (char)tolower((unsigned char)up[k]);
            if (!strstr(lowu, low)) break;
        }
    }
    if (ncand == 0) return 0;
    int best = 0, best_score = -1, best_depth = 1 << 30;
    for (int i = 0; i < ncand; i++) {
        int score = 0, depth = 0;
        size_t li = strlen(cand[i]);
        for (size_t k = 0; k < li; k++) if (cand[i][k] == '/') depth++;
        for (int j = 0; j < ncand; j++) {
            if (i == j) continue;
            if (strncmp(cand[j], cand[i], li) == 0 && cand[j][li] == '/') score++;
        }
        if (score > best_score || (score == best_score && depth < best_depth)) {
            best_score = score; best_depth = depth; best = i;
        }
    }
    {
        /* reconstruct absolute dir from the resolved banks root */
        const char *root = banks_root_dir();
        if (!root || !root[0]) return 0;
        {
            char full[2048];
            snprintf(full, sizeof(full), "%.1023s/%.1023s", root, cand[best]);
            struct stat st;
            if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
                snprintf(out, (size_t)outlen, "%s", full);
                out[outlen - 1] = 0;
                return 1;
            }
        }
        return 0;
    }
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static BankCache *bank_cache(const char *bankdir) {
    for (int i = 0; i < n_banks; i++)
        if (strcmp(banks[i].dir, bankdir) == 0) return &banks[i];
    if (n_banks >= SMP_MAX_BANKS) return NULL;
    BankCache *bc = &banks[n_banks++];
    snprintf(bc->dir, sizeof(bc->dir), "%s", bankdir);
    bc->nfiles = 0;
    /* recursive walk, depth <= 8, breadth via explicit queue */
    char *queue[1024];
    int qh = 0, qt = 0;
    char *root = malloc(strlen(bankdir) + 1);
    if (!root) { n_banks--; return NULL; }
    memcpy(root, bankdir, strlen(bankdir) + 1);
    queue[qt++] = root;
    while (qh < qt && bc->nfiles < SMP_MAX_FILES) {
        char *dir = queue[qh++];
        int depth = 0;
        for (const char *p = dir + strlen(bankdir); *p; p++)
            if (*p == '/') depth++;
        DIR *dp = opendir(dir);
        if (!dp) { free(dir); continue; }
        struct dirent *de;
        while ((de = readdir(dp)) != NULL && bc->nfiles < SMP_MAX_FILES) {
            if (de->d_name[0] == '.') continue;
            char full[2048];
            snprintf(full, sizeof(full), "%.1023s/%.255s", dir, de->d_name);
            struct stat st;
            if (lstat(full, &st) != 0 || S_ISLNK(st.st_mode)) continue;
            if (S_ISDIR(st.st_mode)) {
                if (depth < 8 && qt < 1024) {
                    char *d = malloc(strlen(full) + 1);
                    if (d) { memcpy(d, full, strlen(full) + 1); queue[qt++] = d; }
                }
                continue;
            }
            if (!S_ISREG(st.st_mode)) continue;
            const char *dot = strrchr(de->d_name, '.');
            if (!dot || (strcasecmp(dot, ".wav") != 0 && strcasecmp(dot, ".mp3") != 0 &&
                         strcasecmp(dot, ".flac") != 0 && strcasecmp(dot, ".fla") != 0)) continue;
            /* store path relative to bankdir */
            const char *rel = full + strlen(bankdir);
            while (*rel == '/') rel++;
            if (strlen(rel) >= 512) continue;
            char *d = malloc(strlen(rel) + 1);
            if (!d) continue;
            memcpy(d, rel, strlen(rel) + 1);
            bc->files[bc->nfiles++] = d;
        }
        closedir(dp);
        free(dir);
    }
    while (qh < qt) free(queue[qh++]);
    if (bc->nfiles == 0) { n_banks--; return NULL; }
    qsort(bc->files, (size_t)bc->nfiles, sizeof(char *), cmp_str);
    return bc;
}

/* drop cached dir listings (decoded samples stay valid in memory) */
void smp_invalidate(void) {
    pthread_mutex_lock(&smp_lock);
    for (int i = 0; i < n_banks; i++) {
        for (int k = 0; k < banks[i].nfiles; k++) free(banks[i].files[k]);
        banks[i].nfiles = 0;
        banks[i].dir[0] = 0;
    }
    n_banks = 0;
    pthread_mutex_unlock(&smp_lock);
}

static const char *const *aliases(const char *token, int *n) {
    static const char *bd[] = {"bd", "kick", "bassdrum", "bass-drum", "kickdrum"};
    static const char *sd[] = {"sd", "snare", "snare-drum", "sn"};
    static const char *hh[] = {"hh", "hat", "hihat", "hi-hat", "closedhat", "closed-hat", "chh"};
    static const char *cp[] = {"cp", "clap"};
    static const char *tom[] = {"tom"};
    char t[64];
    snprintf(t, sizeof(t), "%s", token);
    for (char *p = t; *p; p++) *p = (char)tolower((unsigned char)*p);
    if (strcmp(t, "bd") == 0) { *n = 5; return bd; }
    if (strcmp(t, "sd") == 0 || strcmp(t, "sn") == 0) { *n = 4; return sd; }
    if (strcmp(t, "hh") == 0 || strcmp(t, "hat") == 0) { *n = 7; return hh; }
    if (strcmp(t, "cp") == 0 || strcmp(t, "clap") == 0) { *n = 2; return cp; }
    if (strcmp(t, "tom") == 0) { *n = 1; return tom; }
    *n = 0;
    return NULL;
}

/* resample mono float @rate to 44100 + normalize; consumes tmp */
static float *finish_audio(float *tmp, uint32_t have, int rate, int *len_out) {
    if (!tmp || have == 0 || rate < 1000 || rate > 384000) { free(tmp); return NULL; }
    double ratio = 44100.0 / (double)rate;
    double want_d = (double)have * ratio;
    if (!(want_d > 0.0) || !isfinite(want_d)) { free(tmp); return NULL; }
    uint32_t want = (want_d > (double)SMP_MAX_LEN) ? (uint32_t)SMP_MAX_LEN : (uint32_t)want_d;
    if (want == 0) { free(tmp); return NULL; }
    float *out = malloc(sizeof(float) * want);
    if (!out) { free(tmp); return NULL; }
    for (uint32_t i = 0; i < want; i++) {
        double src = i / ratio;
        uint32_t i0 = (uint32_t)src;
        if (i0 >= have - 1) { out[i] = tmp[have - 1]; continue; }
        double fr = src - i0;
        out[i] = (float)(tmp[i0] + (tmp[i0 + 1] - tmp[i0]) * fr);
    }
    free(tmp);
    /* normalize to 0.9 peak */
    float peak = 0;
    for (uint32_t i = 0; i < want; i++) {
        float a = out[i] < 0 ? -out[i] : out[i];
        if (a > peak) peak = a;
    }
    if (peak > 1e-6f) {
        float g = 0.9f / peak;
        for (uint32_t i = 0; i < want; i++) out[i] *= g;
    }
    *len_out = (int)want;
    return out;
}

#define DECODE_MAX_BYTES (32u * 1024u * 1024u)
#define DECODE_MAX_FRAMES (8u * 1024u * 1024u)

/* minimal WAV reader: PCM 8/16/24/32 + IEEE float -> mono float @native rate */
static float *decode_wav(const char *path, uint32_t *have_out, int *rate_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12) { fclose(f); return NULL; }
    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) { fclose(f); return NULL; }
    int format = -1, channels = 0, rate = 0, bits = 0;
    long data_pos = -1;
    uint32_t data_len = 0;
    for (;;) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, f) != 8) break;
        uint32_t sz = (uint32_t)ch[4] | ((uint32_t)ch[5] << 8) | ((uint32_t)ch[6] << 16) | ((uint32_t)ch[7] << 24);
        long pos = ftell(f);
        if (memcmp(ch, "fmt ", 4) == 0 && sz >= 16) {
            uint8_t fmt[16];
            if (fread(fmt, 1, 16, f) != 16) break;
            format = fmt[0] | (fmt[1] << 8);
            channels = fmt[2] | (fmt[3] << 8);
            rate = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | (fmt[7] << 24);
            bits = fmt[14] | (fmt[15] << 8);
        } else if (memcmp(ch, "data", 4) == 0) {
            data_pos = pos;
            data_len = sz;
            break;
        }
        if (sz > DECODE_MAX_BYTES) break;
        if (fseek(f, pos + (long)((sz + 1) & ~1u), SEEK_SET) != 0) break;
    }
    if (data_pos < 0 || channels < 1 || channels > 8 || rate < 1000 || rate > 384000) {
        fclose(f); return NULL;
    }
    int bytes = (bits == 8) ? 1 : (bits == 16) ? 2 : (bits == 24) ? 3 : 4;
    if (format != 1 && format != 3) { fclose(f); return NULL; }
    if (format == 1 && bits != 8 && bits != 16 && bits != 24 && bits != 32) { fclose(f); return NULL; }
    if (format == 3 && bits != 32) { fclose(f); return NULL; }
    if (data_len == 0 || data_len > DECODE_MAX_BYTES) { fclose(f); return NULL; }
    uint64_t bpf = (uint64_t)bytes * (uint64_t)channels;
    if (bpf == 0 || (uint64_t)data_len / bpf == 0) { fclose(f); return NULL; }
    uint64_t frames64 = (uint64_t)data_len / bpf;
    if (frames64 > DECODE_MAX_FRAMES) { fclose(f); return NULL; }
    data_len = (uint32_t)(frames64 * bpf);
    uint8_t *raw = malloc(data_len);
    if (!raw) { fclose(f); return NULL; }
    if (fseek(f, data_pos, SEEK_SET) != 0) { free(raw); fclose(f); return NULL; }
    size_t got = fread(raw, 1, data_len, f);
    fclose(f);
    uint32_t have = (uint32_t)(got / (bytes * channels));
    if (have == 0) { free(raw); return NULL; }
    float *tmp = malloc(sizeof(float) * have);
    if (!tmp) { free(raw); return NULL; }
    for (uint32_t i = 0; i < have; i++) {
        double acc = 0;
        for (int c = 0; c < channels; c++) {
            uint8_t *s = raw + ((size_t)i * channels + (size_t)c) * bytes;
            double v = 0;
            if (format == 3) { float fv; memcpy(&fv, s, 4); v = fv; }
            else if (bits == 8) v = ((int)s[0] - 128) / 128.0;
            else if (bits == 16) { int16_t sv; memcpy(&sv, s, 2); v = sv / 32768.0; }
            else if (bits == 24) {
                int32_t sv = (int32_t)(s[0] | (s[1] << 8) | (s[2] << 16));
                if (sv & 0x800000) sv |= ~0xffffff;
                v = sv / 8388608.0;
            } else { int32_t sv; memcpy(&sv, s, 4); v = sv / 2147483648.0; }
            acc += v;
        }
        tmp[i] = (float)(acc / channels);
    }
    free(raw);
    *have_out = have;
    *rate_out = rate;
    return tmp;
}

/* read whole file, bounded */
static uint8_t *slurp(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0 || (size_t)sz > DECODE_MAX_BYTES) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return NULL; }
    *len_out = (size_t)sz;
    return buf;
}

static float *decode_mp3(const char *path, uint32_t *have_out, int *rate_out) {
    size_t len = 0;
    uint8_t *data = slurp(path, &len);
    if (!data) return NULL;
    mp3dec_t dec;
    mp3dec_init(&dec);
    mp3dec_frame_info_t info;
    memset(&info, 0, sizeof(info));
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    size_t cap = 65536, n = 0;
    float *mono = malloc(cap * sizeof(float));
    if (!mono) { free(data); return NULL; }
    int rate = 0, fail = 0;
    uint8_t *p = data;
    size_t left = len;
    for (;;) {
        int samples = mp3dec_decode_frame(&dec, p, (int)(left > 1 << 30 ? 1 << 30 : left), pcm, &info);
        if (info.frame_bytes <= 0 || (size_t)info.frame_bytes > left) break;
        p += info.frame_bytes;
        left -= (size_t)info.frame_bytes;
        if (samples <= 0 || info.channels < 1 || info.channels > 2) continue;
        if (!rate && info.hz > 0) rate = info.hz;
        if (n + (size_t)samples > DECODE_MAX_FRAMES) break;
        if (n + (size_t)samples > cap) {
            size_t nc = cap * 2;
            if (nc > DECODE_MAX_FRAMES) nc = DECODE_MAX_FRAMES;
            float *nm = realloc(mono, nc * sizeof(float));
            if (!nm) { fail = 1; break; }
            mono = nm;
            cap = nc;
        }
        int ch = info.channels > 2 ? 2 : info.channels;
        for (int i = 0; i < samples; i++) {
            double acc = 0;
            for (int c = 0; c < ch; c++) acc += pcm[i * info.channels + c] / 32768.0;
            mono[n++] = (float)(acc / ch);
        }
        if (left == 0) break;
    }
    free(data);
    if (fail || n == 0 || rate <= 0) { free(mono); return NULL; }
    *have_out = (uint32_t)n;
    *rate_out = rate;
    return mono;
}

static float *decode_flac(const char *path, uint32_t *have_out, int *rate_out) {
    size_t len = 0;
    uint8_t *data = slurp(path, &len);
    if (!data) return NULL;
    drflac *f = drflac_open_memory(data, len, NULL);
    if (!f) { free(data); return NULL; }
    int channels = (int)f->channels;
    int rate = (int)f->sampleRate;
    if (channels <= 0 || channels > 8 || rate <= 0) { drflac_close(f); free(data); return NULL; }
    int mixch = channels > 2 ? 2 : channels;
    size_t cap = 65536, n = 0;
    float *mono = malloc(cap * sizeof(float));
    if (!mono) { drflac_close(f); free(data); return NULL; }
    for (;;) {
        float raw[8192 * 8];
        drflac_uint64 got = drflac_read_pcm_frames_f32(f, 8192, raw);
        if (got == 0) break;
        if (n + (size_t)got > DECODE_MAX_FRAMES) got = DECODE_MAX_FRAMES - n;
        if (n + (size_t)got > cap) {
            size_t nc = cap;
            while (nc < n + (size_t)got) {
                nc *= 2;
                if (nc > DECODE_MAX_FRAMES) { nc = DECODE_MAX_FRAMES; break; }
            }
            float *nm = realloc(mono, nc * sizeof(float));
            if (!nm) { drflac_close(f); free(data); free(mono); return NULL; }
            mono = nm;
            cap = nc;
            if (n + (size_t)got > cap) got = cap - n;
        }
        for (drflac_uint64 i = 0; i < got; i++) {
            double acc = 0;
            for (int c = 0; c < mixch; c++) acc += raw[i * f->channels + c];
            mono[n++] = (float)(acc / mixch);
        }
        if (n >= DECODE_MAX_FRAMES) break;
    }
    drflac_close(f);
    free(data);
    if (n == 0) { free(mono); return NULL; }
    *have_out = (uint32_t)n;
    *rate_out = rate;
    return mono;
}

/* decode by extension -> mono float @44100 */
static float *read_audio(const char *path, int *len_out) {
    const char *dot = strrchr(path, '.');
    float *tmp = NULL;
    uint32_t have = 0;
    int rate = 0;
    if (dot && strcasecmp(dot, ".mp3") == 0) tmp = decode_mp3(path, &have, &rate);
    else if (dot && (strcasecmp(dot, ".flac") == 0 || strcasecmp(dot, ".fla") == 0))
        tmp = decode_flac(path, &have, &rate);
    else tmp = decode_wav(path, &have, &rate);
    if (!tmp) return NULL;
    return finish_audio(tmp, have, rate, len_out);
}

/* insert decoded buffer into cache (double-checked); consumes buf */
static int sample_cache_put(const char *full, float *buf, int len, int base) {
    pthread_mutex_lock(&smp_lock);
    for (int i = 0; i < n_samples; i++) {
        if (strcmp(samples[i].path, full) == 0) {
            free(buf);
            pthread_mutex_unlock(&smp_lock);
            return i;
        }
    }
    if (n_samples >= SMP_MAX_SAMPLES) { free(buf); pthread_mutex_unlock(&smp_lock); return -1; }
    int id = n_samples++;
    samples[id].buf = buf;
    samples[id].len = len;
    samples[id].base_midi = base;
    snprintf(samples[id].path, sizeof(samples[id].path), "%.1023s", full);
    pthread_mutex_unlock(&smp_lock);
    return id;
}

int smp_acquire(const char *bankdir, const char *token) {
    if (!bankdir || !bankdir[0] || !token) return -1;
    pthread_mutex_lock(&smp_lock);
    BankCache *bc = bank_cache(bankdir);
    if (!bc || bc->nfiles == 0) { pthread_mutex_unlock(&smp_lock); return -1; }
    int na = 0;
    const char *const *al = aliases(token, &na);
    const char *pick = NULL;
    char low[256];
    if (al) {
        for (int a = 0; a < na && !pick; a++) {
            size_t alen = strlen(al[a]);
            for (int i = 0; i < bc->nfiles; i++) {
                /* basename only: dirs like rolandtr909-bd must not match */
                const char *base = strrchr(bc->files[i], '/');
                base = base ? base + 1 : bc->files[i];
                size_t L = strlen(base);
                if (L >= sizeof(low)) L = sizeof(low) - 1;
                for (size_t k = 0; k < L; k++)
                    low[k] = (char)tolower((unsigned char)base[k]);
                low[L] = 0;
                /* boundary-aware: alias must not sit inside a longer
                   alpha run (so "sd" skips "bassdrum") */
                const char *at = low;
                while ((at = strstr(at, al[a])) != NULL) {
                    size_t pos = (size_t)(at - low);
                    int leftok = (pos == 0) || !isalpha((unsigned char)low[pos - 1]);
                    int rightok = (pos + alen >= L) || !isalpha((unsigned char)low[pos + alen]);
                    if (leftok && rightok) { pick = bc->files[i]; break; }
                    at++;
                }
                if (pick) break;
            }
        }
    }
    if (!pick) {
        /* single-instrument dir (cryptic names like BT0A0A7.wav):
           deterministic per-token slot */
        char t[16];
        snprintf(t, sizeof(t), "%s", token);
        for (char *p = t; *p; p++) *p = (char)tolower((unsigned char)*p);
        int slot = -1;
        if (strcmp(t, "bd") == 0) slot = 0;
        else if (strcmp(t, "sd") == 0 || strcmp(t, "sn") == 0) slot = 1;
        else if (strcmp(t, "hh") == 0 || strcmp(t, "hat") == 0) slot = 2;
        else if (strcmp(t, "cp") == 0 || strcmp(t, "clap") == 0) slot = 3;
        else if (strcmp(t, "tom") == 0) slot = 4;
        if (slot >= 0) pick = bc->files[slot % bc->nfiles];
    }
    if (!pick) { pthread_mutex_unlock(&smp_lock); return -1; }
    char full[2048];
    snprintf(full, sizeof(full), "%.1023s/%.1023s", bankdir, pick);
    for (int i = 0; i < n_samples; i++) {
        if (strcmp(samples[i].path, full) == 0) {
            int id = i;
            pthread_mutex_unlock(&smp_lock);
            return id;
        }
    }
    if (n_samples >= SMP_MAX_SAMPLES) { pthread_mutex_unlock(&smp_lock); return -1; }
    pthread_mutex_unlock(&smp_lock);
    int len = 0;
    float *buf = read_audio(full, &len); /* file I/O outside lock */
    if (!buf) return -1;
    return sample_cache_put(full, buf, len, 60);
}

/* load an arbitrary bank-relative path (for F6 audition) */
int smp_acquire_path(const char *relpath) {
    if (!relpath || !relpath[0]) return -1;
    if (strstr(relpath, "..") || relpath[0] == '/') return -1;
    const char *root = banks_root_dir();
    if (!root || !root[0]) return -1;
    char full[2048];
    snprintf(full, sizeof(full), "%.1023s/%.1023s", root, relpath);
    struct stat st;
    if (lstat(full, &st) != 0 || S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode)) return -1;
    const char *dot = strrchr(full, '.');
    if (!dot || (strcasecmp(dot, ".wav") != 0 && strcasecmp(dot, ".mp3") != 0 &&
                 strcasecmp(dot, ".flac") != 0 && strcasecmp(dot, ".fla") != 0)) return -1;
    pthread_mutex_lock(&smp_lock);
    for (int i = 0; i < n_samples; i++) {
        if (strcmp(samples[i].path, full) == 0) {
            pthread_mutex_unlock(&smp_lock);
            return i;
        }
    }
    if (n_samples >= SMP_MAX_SAMPLES) { pthread_mutex_unlock(&smp_lock); return -1; }
    pthread_mutex_unlock(&smp_lock);
    int len = 0;
    float *buf = read_audio(full, &len);
    if (!buf) return -1;
    return sample_cache_put(full, buf, len, 60);
}

/* parse first note-like token [A-G][#b]?-?digits in a filename; -1 if none.
   The letter must start the name or follow a separator, so cryptic
   names like BT0A0D0 don't false-positive. */
static int filename_note_midi(const char *name) {
    static const int semis[7] = {9, 11, 0, 2, 4, 5, 7}; /* a b c d e f g */
    for (const char *p = name; *p; p++) {
        char c = (char)tolower((unsigned char)*p);
        if (c < 'a' || c > 'g') continue;
        if (p != name) {
            char prev = p[-1];
            if ((prev >= 'a' && prev <= 'z') || (prev >= 'A' && prev <= 'Z') ||
                (prev >= '0' && prev <= '9')) continue;
        }
        const char *q = p + 1;
        int semi = semis[c - 'a'];
        if (*q == '#') { semi++; q++; }
        else if (*q == 'b' || *q == 'B') {
            const char *r = q + 1;
            if (*r == '-' || (*r >= '0' && *r <= '9')) { semi--; q++; }
            else continue;
        }
        int neg = 0;
        if (*q == '-') { neg = 1; q++; }
        if (*q < '0' || *q > '9') continue;
        int oct = 0;
        while (*q >= '0' && *q <= '9') { oct = oct * 10 + (*q - '0'); q++; }
        if (neg) oct = -oct;
        if (oct < -1 || oct > 9) continue;
        int midi = (oct + 1) * 12 + semi;
        if (midi < 0 || midi > 127) continue;
        return midi;
    }
    return -1;
}

int smp_acquire_note(const char *bankdir, int midi) {
    if (!bankdir || !bankdir[0]) return -1;
    if (midi < 0) midi = 60;
    if (midi > 127) midi = 127;
    pthread_mutex_lock(&smp_lock);
    BankCache *bc = bank_cache(bankdir);
    if (!bc || bc->nfiles == 0) { pthread_mutex_unlock(&smp_lock); return -1; }
    /* best filename-note match (nearest pitch), else first file */
    int best = -1, bestdist = 1 << 30;
    for (int i = 0; i < bc->nfiles; i++) {
        const char *base = strrchr(bc->files[i], '/');
        base = base ? base + 1 : bc->files[i];
        int m = filename_note_midi(base);
        if (m < 0) continue;
        int d = m > midi ? m - midi : midi - m;
        if (d < bestdist) { bestdist = d; best = i; }
        if (d == 0) break;
    }
    const char *pick = (best >= 0) ? bc->files[best] : bc->files[0];
    int base = (best >= 0) ? filename_note_midi(
        strrchr(pick, '/') ? strrchr(pick, '/') + 1 : pick) : 60;
    if (base < 0) base = 60;
    char full[2048];
    snprintf(full, sizeof(full), "%.1023s/%.1023s", bankdir, pick);
    for (int i = 0; i < n_samples; i++) {
        if (strcmp(samples[i].path, full) == 0) {
            pthread_mutex_unlock(&smp_lock);
            return i;
        }
    }
    if (n_samples >= SMP_MAX_SAMPLES) { pthread_mutex_unlock(&smp_lock); return -1; }
    pthread_mutex_unlock(&smp_lock);
    int len = 0;
    float *buf = read_audio(full, &len);
    if (!buf) return -1;
    return sample_cache_put(full, buf, len, base);
}

int smp_base_midi(int id) {
    int b = 60;
    pthread_mutex_lock(&smp_lock);
    if (id >= 0 && id < n_samples) b = samples[id].base_midi;
    pthread_mutex_unlock(&smp_lock);
    return b;
}

void smp_get(int id, float **buf, int *len) {
    pthread_mutex_lock(&smp_lock);
    if (id >= 0 && id < n_samples) {
        if (buf) *buf = samples[id].buf;
        if (len) *len = samples[id].len;
    } else {
        if (buf) *buf = NULL;
        if (len) *len = 0;
    }
    pthread_mutex_unlock(&smp_lock);
}
