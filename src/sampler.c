#include "sampler.h"
#include "banks.h"
#include "websamp.h"
#include "common.h"
#define MINIMP3_IMPLEMENTATION
/* Preserve sub-16-bit decoder detail. Quantization perturbs the reference
   vocoder's peak regions, especially during downward pitch compensation. */
#define MINIMP3_FLOAT_OUTPUT
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
#define SMP_MAX_LEN (44100 * 120)
#define SMP_CACHE_BYTES (256u * 1024u * 1024u)
#define SMP_MAX_BANKS 32
#define SMP_MAX_FILES 2048

typedef struct {
    float *buf;
    float *stereo;
    int channels;
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
static size_t sample_bytes;
static BankCache banks[SMP_MAX_BANKS];
static int n_banks = 0;
static pthread_mutex_t smp_lock = PTHREAD_MUTEX_INITIALIZER;

int bank_find_dir(const char *frag, char *out, int outlen) {
    char rel[512];
    char full[2048];
    const char *root;
    struct stat st;
    int n;
    if (!frag || !frag[0] || !out || outlen <= 0) return 0;
    banks_ensure_default();
    if (!banks_dir_named(frag, rel, (int)sizeof(rel))) return 0;
    root = banks_root_dir();
    if (!root || !root[0]) return 0;
    n = snprintf(full, sizeof(full), "%s/%s", root, rel);
    if (n < 0 || (size_t)n >= sizeof(full) || (size_t)n >= (size_t)outlen) return 0;
    if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) return 0;
    memcpy(out, full, (size_t)n + 1);
    return 1;
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

/* Resample interleaved stereo to 44100 Hz, preserving recorded levels. */
static float *finish_audio(float *tmp, uint32_t have, int rate, int *len_out) {
    if (!tmp || have == 0 || rate < 1000 || rate > 384000) { free(tmp); return NULL; }
    double ratio = 44100.0 / (double)rate;
    double want_d = (double)have * ratio;
    if (!(want_d > 0.0) || !isfinite(want_d)) { free(tmp); return NULL; }
    if (want_d > SMP_MAX_LEN) {
        log_console("sampler: sample exceeds the 120-second decoded duration limit");
        free(tmp); return NULL;
    }
    uint32_t want = (uint32_t)want_d;
    if (want == 0) { free(tmp); return NULL; }
    float *out = malloc(sizeof(float) * (size_t)want * 2);
    if (!out) { free(tmp); return NULL; }
    for (uint32_t i = 0; i < want; i++) {
        double src = i / ratio;
        uint32_t i0 = (uint32_t)src;
        double fr = src - i0;
        uint32_t next = i0+1 < have ? i0+1 : have-1;
        if (i0 >= have) i0 = have-1;
        for (int ch = 0; ch < 2; ch++)
            out[(size_t)i*2+ch] = (float)(tmp[(size_t)i0*2+ch] + (tmp[(size_t)next*2+ch]-tmp[(size_t)i0*2+ch])*fr);
    }
    free(tmp);
    /* Preserve recorded levels, as WebAudio decodeAudioData does. */
    *len_out = (int)want;
    return out;
}

#define DECODE_MAX_BYTES (32u * 1024u * 1024u)
#define DECODE_MAX_FRAMES (8u * 1024u * 1024u)

/* PCM/float WAV to interleaved stereo at the source sample rate. */
static float *decode_wav(const char *path, uint32_t *have_out, int *rate_out, int *channels_out) {
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
    float *tmp = malloc(sizeof(float) * (size_t)have * 2);
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
            if (!isfinite(v)) v = 0;
            if (channels == 2) tmp[(size_t)i*2+c] = (float)v;
            acc += v;
        }
        if (channels != 2) tmp[(size_t)i*2] = tmp[(size_t)i*2+1] = (float)(acc/channels);
    }
    free(raw);
    *have_out = have;
    *rate_out = rate;
    *channels_out = channels == 2 ? 2 : 1;
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

static uint32_t mp3_u32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24) | ((uint32_t)p[1]<<16) | ((uint32_t)p[2]<<8) | p[3];
}

/* Xing/Info frames are metadata, not PCM. Match minimp3_ex's gapless
   convention: LAME encoder delay plus 529 decoder frames, then limit
   playback to the declared original length. Based on lieff/minimp3's
   CC0 mp3dec_check_vbrtag; keep all offsets inside the decoded frame. */
static int mp3_gapless(const uint8_t *frame, size_t size, int frame_samples,
                       size_t *skip, size_t *length) {
    if (size < 4) return 0;
    int mpeg1 = ((frame[1]>>3)&3) == 3;
    int mono = (frame[3]>>6) == 3;
    size_t offset = 4 + (mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17));
    if (!(frame[1]&1)) offset += 2; /* CRC precedes side information. */
    if (offset > size || size-offset < 8) return 0;
    if (memcmp(frame+offset, "Xing", 4) && memcmp(frame+offset, "Info", 4)) return 0;
    uint32_t flags = mp3_u32(frame+offset+4);
    offset += 8;
    if (!(flags&1) || size-offset < 4) return 1;
    uint64_t count = (uint64_t)mp3_u32(frame+offset)*(unsigned)frame_samples;
    offset += 4;
    const unsigned masks[] = {2, 4, 8};
    const size_t sizes[] = {4, 100, 4};
    for (int i = 0; i < 3; i++) {
        if (!(flags&masks[i])) continue;
        if (size-offset < sizes[i]) return 1;
        offset += sizes[i];
    }
    if (size-offset < 36 || !frame[offset]) return 1;
    const uint8_t *tag = frame+offset+21;
    unsigned delay = ((unsigned)tag[0]<<4) | (tag[1]>>4);
    unsigned padding = ((unsigned)(tag[1]&15)<<8) | tag[2];
    if (count <= (uint64_t)delay+padding || count > DECODE_MAX_FRAMES) return 1;
    *skip = delay+529;
    *length = (size_t)(count-delay-padding);
    return 1;
}

static float *decode_mp3(const char *path, uint32_t *have_out, int *rate_out, int *channels_out) {
    size_t len = 0;
    uint8_t *data = slurp(path, &len);
    if (!data) return NULL;
    mp3dec_t dec;
    mp3dec_init(&dec);
    mp3dec_frame_info_t info;
    memset(&info, 0, sizeof(info));
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    size_t cap = 65536, n = 0;
    float *mono = malloc(cap * sizeof(float) * 2);
    if (!mono) { free(data); return NULL; }
    int rate = 0, fail = 0, stereo = 0;
    int checked_tag = 0;
    size_t skip = 0, length = 0;
    uint8_t *p = data;
    size_t left = len;
    for (;;) {
        int samples = mp3dec_decode_frame(&dec, p, (int)(left > 1 << 30 ? 1 << 30 : left), pcm, &info);
        if (info.frame_bytes <= 0 || (size_t)info.frame_bytes > left) break;
        if (!checked_tag && samples > 0) {
            checked_tag = 1;
            if (info.layer == 3 && info.frame_offset >= 0 && info.frame_offset < info.frame_bytes &&
                mp3_gapless(p+info.frame_offset, (size_t)(info.frame_bytes-info.frame_offset),
                            samples, &skip, &length)) {
                p += info.frame_bytes;
                left -= (size_t)info.frame_bytes;
                mp3dec_init(&dec);
                continue;
            }
        }
        p += info.frame_bytes;
        left -= (size_t)info.frame_bytes;
        if (samples <= 0 || info.channels < 1 || info.channels > 2) continue;
        if (!rate && info.hz > 0) rate = info.hz;
        if (info.hz != rate) { fail = 1; break; }
        if (info.channels == 2) stereo = 1;
        size_t drop = skip < (size_t)samples ? skip : (size_t)samples;
        skip -= drop;
        int first = (int)drop;
        samples -= first;
        if (length && n+(size_t)samples > length) samples = (int)(length-n);
        if (n + (size_t)samples > DECODE_MAX_FRAMES) break;
        if (n + (size_t)samples > cap) {
            size_t nc = cap * 2;
            if (nc > DECODE_MAX_FRAMES) nc = DECODE_MAX_FRAMES;
            float *nm = realloc(mono, nc * sizeof(float) * 2);
            if (!nm) { fail = 1; break; }
            mono = nm;
            cap = nc;
        }
        for (int i = first; i < first+samples; i++) {
            mono[n*2] = pcm[i*info.channels];
            mono[n*2+1] = pcm[i*info.channels+(info.channels == 2 ? 1 : 0)];
            n++;
        }
        if (left == 0 || (length && n == length)) break;
    }
    free(data);
    if (fail || n == 0 || rate <= 0) { free(mono); return NULL; }
    *have_out = (uint32_t)n;
    *rate_out = rate;
    *channels_out = stereo ? 2 : 1;
    return mono;
}

static float *decode_flac(const char *path, uint32_t *have_out, int *rate_out, int *channels_out) {
    size_t len = 0;
    uint8_t *data = slurp(path, &len);
    if (!data) return NULL;
    drflac *f = drflac_open_memory(data, len, NULL);
    if (!f) { free(data); return NULL; }
    int channels = (int)f->channels;
    int rate = (int)f->sampleRate;
    if (channels <= 0 || channels > 8 || rate <= 0) { drflac_close(f); free(data); return NULL; }
    size_t cap = 65536, n = 0;
    float *mono = malloc(cap * sizeof(float) * 2);
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
            float *nm = realloc(mono, nc * sizeof(float) * 2);
            if (!nm) { drflac_close(f); free(data); free(mono); return NULL; }
            mono = nm;
            cap = nc;
            if (n + (size_t)got > cap) got = cap - n;
        }
        for (drflac_uint64 i = 0; i < got; i++) {
            mono[n*2] = raw[i*f->channels];
            mono[n*2+1] = raw[i*f->channels+(channels > 1 ? 1 : 0)];
            n++;
        }
        if (n >= DECODE_MAX_FRAMES) break;
    }
    drflac_close(f);
    free(data);
    if (n == 0) { free(mono); return NULL; }
    *have_out = (uint32_t)n;
    *rate_out = rate;
    *channels_out = channels > 1 ? 2 : 1;
    return mono;
}

/* Decode by extension -> interleaved stereo @44100. */
static float *read_audio(const char *path, int *len_out, int *channels_out) {
    const char *dot = strrchr(path, '.');
    float *tmp = NULL;
    uint32_t have = 0;
    int rate = 0;
    if (dot && strcasecmp(dot, ".mp3") == 0) tmp = decode_mp3(path, &have, &rate,channels_out);
    else if (dot && (strcasecmp(dot, ".flac") == 0 || strcasecmp(dot, ".fla") == 0))
        tmp = decode_flac(path, &have, &rate,channels_out);
    else tmp = decode_wav(path, &have, &rate,channels_out);
    if (!tmp) return NULL;
    return finish_audio(tmp, have, rate, len_out);
}

/* insert decoded buffer into cache (double-checked); consumes buf */
static int sample_cache_put(const char *full, float *buf, int len, int base, int channels) {
    if (!buf || len <= 0 || len > SMP_MAX_LEN) { free(buf); return -1; }
    size_t bytes = (size_t)len*3*sizeof(float);
    float *mono = malloc((size_t)len*sizeof(float));
    if (!mono) { free(buf); return -1; }
    for (int i = 0; i < len; i++) mono[i] = (buf[(size_t)i*2]+buf[(size_t)i*2+1])*.5f;
    pthread_mutex_lock(&smp_lock);
    for (int i = 0; i < n_samples; i++) {
        if (strcmp(samples[i].path, full) == 0 && samples[i].base_midi == base) {
            free(buf); free(mono);
            pthread_mutex_unlock(&smp_lock);
            return i;
        }
    }
    if (n_samples >= SMP_MAX_SAMPLES || bytes > SMP_CACHE_BYTES-sample_bytes) {
        free(buf); free(mono); pthread_mutex_unlock(&smp_lock);
        log_console("sampler: decoded sample cache is full (256 samples / 256 MiB)");
        return -1;
    }
    int id = n_samples++;
    samples[id].buf = mono;
    samples[id].stereo = buf;
    samples[id].channels = channels;
    sample_bytes += bytes;
    samples[id].len = len;
    samples[id].base_midi = base;
    snprintf(samples[id].path, sizeof(samples[id].path), "%.1023s", full);
    pthread_mutex_unlock(&smp_lock);
    return id;
}

/* Round sample indices like JavaScript Math.round, then wrap at selection. */
static void split_sample_n(const char *token, char *name, size_t nlen, int *idx) {
    *idx = 0;
    if (!token) { if (nlen) name[0] = 0; return; }
    snprintf(name, nlen, "%s", token);
    char *c = strchr(name, ':');
    if (!c || c == name || c[1] == 0) return;
    char *end = NULL;
    double v = strtod(c + 1, &end);
    if (end == c + 1 || (end && *end != 0)) return;
    if (!isfinite(v) || v < -1000000 || v > 1000000) return;
    *c = 0;
    *idx = (int)floor(v+.5);
}

int smp_acquire(const char *bankdir, const char *token) {
    if (!bankdir || !bankdir[0] || !token) return -1;
    char name[64];
    int sample_n = -1;
    split_sample_n(token, name, sizeof(name), &sample_n);
    pthread_mutex_lock(&smp_lock);
    BankCache *bc = bank_cache(bankdir);
    if (!bc || bc->nfiles == 0) { pthread_mutex_unlock(&smp_lock); return -1; }
    int na = 0;
    const char *const *al = aliases(name, &na);
    const char *pick = NULL;
    const char *hits[128];
    int nhits = 0;
    char low[256];
    if (al) {
        for (int a = 0; a < na; a++) {
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
                    if (leftok && rightok) {
                        int seen = 0;
                        for (int h = 0; h < nhits; h++)
                            if (hits[h] == bc->files[i]) seen = 1;
                        if (!seen && nhits < 128) hits[nhits++] = bc->files[i];
                        break;
                    }
                    at++;
                }
            }
        }
    }
    if (nhits > 0) {
        int ix = sample_n%nhits;
        if (ix < 0) ix += nhits;
        pick = hits[ix];
    }
    if (!pick) {
        /* single-instrument dir (cryptic names like BT0A0A7.wav):
           deterministic per-token slot. hh:1 walks one file further. */
        char t[16];
        snprintf(t, sizeof(t), "%.15s", name);
        for (char *p = t; *p; p++) *p = (char)tolower((unsigned char)*p);
        int slot = -1;
        if (strcmp(t, "bd") == 0) slot = 0;
        else if (strcmp(t, "sd") == 0 || strcmp(t, "sn") == 0) slot = 1;
        else if (strcmp(t, "hh") == 0 || strcmp(t, "hat") == 0) slot = 2;
        else if (strcmp(t, "cp") == 0 || strcmp(t, "clap") == 0) slot = 3;
        else if (strcmp(t, "tom") == 0) slot = 4;
        if (slot >= 0) {
            int ix = (slot+sample_n)%bc->nfiles;
            if (ix < 0) ix += bc->nfiles;
            pick = bc->files[ix];
        }
    }
    if (!pick) {
        /* n("0") on an array bank: file 0 in directory order (000_… first). */
        char *end = NULL;
        long v = strtol(name, &end, 10);
        if (name[0] && end != name && end && *end == 0 && v >= 0 && v <= 1000000)
            pick = bc->files[(int)(v % bc->nfiles)];
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
    int channels = 1;
    float *buf = read_audio(full, &len,&channels); /* file I/O outside lock */
    if (!buf) return -1;
    return sample_cache_put(full, buf, len, 60,channels);
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
    int channels = 1;
    float *buf = read_audio(full, &len,&channels);
    if (!buf) return -1;
    return sample_cache_put(full, buf, len, 60,channels);
}

static int has_dotdot(const char *path) {
    const char *p = path;
    while ((p = strstr(p, "..")) != NULL) {
        int left = (p == path) || p[-1] == '/';
        int right = p[2] == 0 || p[2] == '/';
        if (left && right) return 1;
        p++;
    }
    return 0;
}

static int abs_audio_path(const char *fullpath) {
    const char *p;
    const char *dot;
    struct stat st;
    if (!fullpath || fullpath[0] != '/' || strlen(fullpath) >= 1024) return 0;
    if (has_dotdot(fullpath)) return 0;
    for (p = fullpath; *p; p++)
        if ((unsigned char)*p < 32 || (unsigned char)*p == 127) return 0;
    dot = strrchr(fullpath, '.');
    if (!dot || (strcasecmp(dot, ".wav") != 0 && strcasecmp(dot, ".mp3") != 0 &&
                 strcasecmp(dot, ".flac") != 0 && strcasecmp(dot, ".fla") != 0)) return 0;
    if (lstat(fullpath, &st) != 0 || S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode)) return 0;
    return 1;
}

int smp_acquire_abs(const char *fullpath, int base_midi) {
    char full[1024];
    int len = 0;
    int channels = 1;
    float *buf;
    if (!abs_audio_path(fullpath)) return -1;
    if (base_midi < 0 || base_midi > 127) base_midi = 60;
    snprintf(full, sizeof(full), "%s", fullpath);
    pthread_mutex_lock(&smp_lock);
    for (int i = 0; i < n_samples; i++) {
        if (strcmp(samples[i].path, full) == 0 && samples[i].base_midi == base_midi) {
            pthread_mutex_unlock(&smp_lock);
            return i;
        }
    }
    if (n_samples >= SMP_MAX_SAMPLES) { pthread_mutex_unlock(&smp_lock); return -1; }
    pthread_mutex_unlock(&smp_lock);
    buf = read_audio(full, &len, &channels);
    if (!buf) return -1;
    return sample_cache_put(full, buf, len, base_midi, channels);
}

int smp_acquire_file(const char *fullpath, int base_midi) {
    if (!websamp_is_cache_file(fullpath)) return -1;
    if (base_midi < 0 || base_midi > 127) base_midi = 60;
    const char *dot = strrchr(fullpath, '.');
    if (!dot || (strcasecmp(dot, ".wav") != 0 && strcasecmp(dot, ".mp3") != 0 &&
                 strcasecmp(dot, ".flac") != 0 && strcasecmp(dot, ".fla") != 0)) return -1;
    char full[2048];
    snprintf(full, sizeof(full), "%s", fullpath);
    pthread_mutex_lock(&smp_lock);
    for (int i = 0; i < n_samples; i++) {
        if (strcmp(samples[i].path, full) == 0 && samples[i].base_midi == base_midi) {
            pthread_mutex_unlock(&smp_lock);
            return i;
        }
    }
    if (n_samples >= SMP_MAX_SAMPLES) { pthread_mutex_unlock(&smp_lock); return -1; }
    pthread_mutex_unlock(&smp_lock);
    int len = 0;
    int channels = 1;
    float *buf = read_audio(full, &len,&channels);
    if (!buf) return -1;
    return sample_cache_put(full, buf, len, base_midi,channels);
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
    int channels = 1;
    float *buf = read_audio(full, &len,&channels);
    if (!buf) return -1;
    return sample_cache_put(full, buf, len, base,channels);
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

void smp_get_stereo(int id, float **buf, int *len, int *channels) {
    pthread_mutex_lock(&smp_lock);
    if (buf) *buf = id >= 0 && id < n_samples ? samples[id].stereo : NULL;
    if (len) *len = id >= 0 && id < n_samples ? samples[id].len : 0;
    if (channels) *channels = id >= 0 && id < n_samples ? samples[id].channels : 0;
    pthread_mutex_unlock(&smp_lock);
}
