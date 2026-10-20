#include "websamp.h"
#include "sampler.h"
#include "common.h"
#include "sound_store.h"
#include "banks.h"
#include <dirent.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define WEB_NAMES 256
#define WAVEFORM_CATALOG "Dough-Waveforms/strudel.json"
#define WAVEFORM_SPEC "github:Bubobubobubobubo/Dough-Waveforms/125801fa9a799117fd0a0350a195ab7c2f4cdf13"
static int waveform_installed(void);
#define WEB_URL 480
#define WEB_JSON_MAX (2u * 1024u * 1024u)
#define WEB_AUDIO_MAX (8u * 1024u * 1024u)
#define WEB_SLOTS_MAX 1024
#define WEB_TOTAL_SLOTS 8192

typedef struct {
    char url[WEB_URL];
    char local[512];
    int base_midi;
    int pitched;
    int ready; /* url resolved */
    int failed;
    unsigned gen;
} WebSlot;

typedef struct {
    char name[64];
    WebSlot *slots;
    int nslots;
    int machine;
} WebName;

static WebName web_names[WEB_NAMES];
static int web_n = 0;
static unsigned web_gen = 1;
static unsigned call_gen = 1;
static pthread_mutex_t web_lock = PTHREAD_MUTEX_INITIALIZER;
/* Concurrent preparation must not write the same .part file. Web loadCache
   shares one fetch promise per URL; hashed download locks keep same-URL jobs
   serialized while decoding and unrelated fetches stay parallel. */
#define DOWNLOAD_LOCK_COUNT 16
static pthread_mutex_t download_locks[DOWNLOAD_LOCK_COUNT] = {
    PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER
};

static int is_ident_char(int c) {
    return isalnum(c) || c == '_';
}

static void slot_clear_all(void) {
    for (int i = 0; i < web_n; i++) {
        free(web_names[i].slots);
        web_names[i].slots = NULL;
        web_names[i].nslots = 0;
        web_names[i].name[0] = 0;
    }
    web_n = 0;
    web_gen++;
    if (web_gen == 0) web_gen = 1;
}

static void lower_copy(char *dst, int n, const char *src) {
    int i = 0;
    if (!src) src = "";
    for (; src[i] && i + 1 < n; i++)
        dst[i] = (char)tolower((unsigned char)src[i]);
    dst[i] = 0;
}

static WebName *find_name(const char *name, int create) {
    char key[64];
    lower_copy(key, (int)sizeof(key), name);
    if (!key[0] || strlen(name) >= sizeof(key)) return NULL;
    if (key[0] == '_') return NULL;
    for (int i = 0; i < web_n; i++) {
        if (strcmp(web_names[i].name, key) == 0) return &web_names[i];
    }
    if (!create || web_n >= WEB_NAMES) return NULL;
    WebName *w = &web_names[web_n++];
    memset(w, 0, sizeof(*w));
    snprintf(w->name, sizeof(w->name), "%s", key);
    return w;
}

static WebSlot *add_slot(const char *name) {
    WebName *w = find_name(name, 1);
    if (!w || w->nslots >= WEB_SLOTS_MAX) return NULL;
    int total = 0;
    for (int i = 0; i < web_n; i++) total += web_names[i].nslots;
    if (total >= WEB_TOTAL_SLOTS) return NULL;
    WebSlot *ns = realloc(w->slots, (size_t)(w->nslots + 1) * sizeof(WebSlot));
    if (!ns) return NULL;
    w->slots = ns;
    WebSlot *s = &w->slots[w->nslots++];
    memset(s, 0, sizeof(*s));
    s->base_midi = 36;
    s->gen = call_gen;
    return s;
}

/* ---- URL policy -------------------------------------------------- */

static int host_of(const char *url, char *host, int n) {
    const char *p = strstr(url, "://");
    if (!p) return 0;
    p += 3;
    if (!*p || *p == '/' || *p == '@' || *p == '[') return 0;
    const char *at = strchr(p, '@');
    const char *slash = strchr(p, '/');
    if (at && (!slash || at < slash)) return 0; /* userinfo */
    int i = 0;
    while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#') {
        unsigned char c = (unsigned char)*p++;
        if (c <= 0x20 || c == 0x7f || c == '\\') return 0;
        if (i + 1 >= n) return 0;
        host[i++] = (char)tolower(c);
    }
    host[i] = 0;
    return i > 0;
}

static int ipv4_private(const char *host) {
    unsigned a, b, c, d;
    char tail;
    if (sscanf(host, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return 0;
    if (a > 255 || b > 255 || c > 255 || d > 255) return 1;
    if (a == 0 || a == 10 || a == 127) return 1;
    if (a == 169 && b == 254) return 1;
    if (a == 172 && b >= 16 && b <= 31) return 1;
    if (a == 192 && b == 168) return 1;
    if (a == 100 && b >= 64 && b <= 127) return 1; /* CGNAT */
    if (a >= 224) return 1;
    return 0;
}

int websamp_url_allowed(const char *url) {
    if (!url || !url[0] || strlen(url) >= WEB_URL) return 0;
    if (strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0) return 0;
    for (const char *p = url; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c <= 0x20 || c == 0x7f) return 0;
    }
    char host[256];
    if (!host_of(url, host, (int)sizeof(host))) return 0;
    if (strcmp(host, "localhost") == 0) return 0;
    if (strlen(host) >= 10 && strcmp(host + strlen(host) - 10, ".localhost") == 0) return 0;
    if (strlen(host) >= 6 && strcmp(host + strlen(host) - 6, ".local") == 0) return 0;
    if (ipv4_private(host)) return 0;
    return 1;
}

static int comp_ok(const char *s) {
    if (!s || !s[0] || strcmp(s, ".") == 0 || strcmp(s, "..") == 0) return 0;
    if (strlen(s) > 80) return 0;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(isalnum(c) || c == '.' || c == '_' || c == '-')) return 0;
    }
    return 1;
}

int websamp_github_to_url(const char *spec, int as_json, char *out, int outlen) {
    if (!spec || !out || outlen < 16) return 0;
    if (strncmp(spec, "github:", 7) != 0) return 0;
    const char *p = spec + 7;
    char comp[6][96];
    int nc = 0;
    while (*p && nc < 6) {
        while (*p == '/') p++;
        if (!*p) break;
        int k = 0;
        while (*p && *p != '/') {
            if (k + 1 >= (int)sizeof(comp[0])) return 0;
            comp[nc][k++] = *p++;
        }
        comp[nc][k] = 0;
        if (!comp_ok(comp[nc])) return 0;
        nc++;
    }
    if (nc < 1) return 0;
    const char *user = comp[0];
    const char *repo = (nc >= 2) ? comp[1] : "samples";
    const char *branch = (nc >= 3) ? comp[2] : "main";
    char rest[320];
    rest[0] = 0;
    size_t rp = 0;
    for (int i = 3; i < nc; i++) {
        size_t cl = strlen(comp[i]);
        if (rp + cl + 2 >= sizeof(rest)) return 0;
        if (rp) rest[rp++] = '/';
        memcpy(rest + rp, comp[i], cl);
        rp += cl;
        rest[rp] = 0;
    }
    if (as_json) {
        size_t rl = strlen(rest);
        int has_json = rl >= 5 && strcmp(rest + rl - 5, ".json") == 0;
        if (!has_json) {
            const char *add = rl ? "/strudel.json" : "strudel.json";
            if (rl + strlen(add) + 1 >= sizeof(rest)) return 0;
            memcpy(rest + rl, add, strlen(add) + 1);
        }
    }
    int wr = snprintf(out, (size_t)outlen, "https://raw.githubusercontent.com/%s/%s/%s/%s",
                      user, repo, branch, rest);
    if (wr < 0 || wr >= outlen) return 0;
    return websamp_url_allowed(out);
}

int websamp_resolve(const char *base, const char *rel, char *out, int outlen) {
    if (!rel || !out || outlen < 16) return 0;
    char abs[WEB_URL];
    abs[0] = 0;
    if (strncmp(rel, "github:", 7) == 0) {
        if (!websamp_github_to_url(rel, 0, abs, (int)sizeof(abs))) return 0;
        rel = abs;
    }
    if (strncmp(rel, "https://", 8) == 0 || strncmp(rel, "http://", 7) == 0) {
        int wr = snprintf(out, (size_t)outlen, "%s", rel);
        if (wr < 0 || wr >= outlen) return 0;
        return websamp_url_allowed(out);
    }
    if (!base || !base[0]) return 0;
    char b[WEB_URL];
    if (strncmp(base, "github:", 7) == 0) {
        if (!websamp_github_to_url(base, 0, b, (int)sizeof(b))) return 0;
    } else {
        int wr = snprintf(b, sizeof(b), "%s", base);
        if (wr < 0 || wr >= (int)sizeof(b)) return 0;
    }
    size_t L = strlen(b);
    if (L + 2 >= sizeof(b)) return 0;
    if (b[L - 1] != '/') {
        b[L] = '/';
        b[L + 1] = 0;
    }
    while (*rel == '/') rel++;
    size_t bl = strlen(b);
    size_t rl = strlen(rel);
    if (!*rel || bl + rl >= (size_t)outlen) return 0;
    memcpy(out, b, bl);
    memcpy(out + bl, rel, rl + 1);
    return websamp_url_allowed(out);
}

/* ---- map fill ---------------------------------------------------- */

static int pitch_key_midi(const char *name, int *out) {
    if (!name || !name[0] || !out) return 0;
    char n = (char)tolower((unsigned char)name[0]);
    int semi;
    if (n == 'c') semi = 0;
    else if (n == 'd') semi = 2;
    else if (n == 'e') semi = 4;
    else if (n == 'f') semi = 5;
    else if (n == 'g') semi = 7;
    else if (n == 'a') semi = 9;
    else if (n == 'b') semi = 11;
    else return 0;
    const char *p = name + 1;
    while (*p == '#' || *p == 's' || *p == 'b' || *p == 'f') {
        semi += (*p == '#' || *p == 's') ? 1 : -1;
        p++;
    }
    int neg = 0;
    if (!*p) {
        int midi = 48+semi;
        if (midi < 0 || midi > 127) return 0;
        *out = midi; return 1;
    }
    if (*p == '-') { neg = 1; p++; }
    if (*p < '0' || *p > '9') return 0;
    int oct = 0;
    while (*p >= '0' && *p <= '9') {
        oct = oct * 10 + (*p - '0');
        if (oct > 20) return 0;
        p++;
    }
    if (*p) return 0;
    if (neg) oct = -oct;
    int midi = (oct + 1) * 12 + semi;
    if (midi < 0 || midi > 127) return 0;
    *out = midi;
    return 1;
}

/* Remember a path. Absolute http(s) is stored now; anything else
   waits for finish_call so a later _base key still applies. */
static int remember(const char *name, const char *path, int pitched, int midi) {
    if (!name || !path || !path[0]) return 0;
    pthread_mutex_lock(&web_lock);
    WebName *w = find_name(name, 1);
    int replacing = w && w->nslots > 0 && w->slots[0].gen != call_gen;
    if (replacing) {
        free(w->slots);
        w->slots = NULL;
        w->nslots = 0;
    }
    WebSlot *s = add_slot(name);
    if (!s) { pthread_mutex_unlock(&web_lock); return 0; }
    s->pitched = pitched;
    s->base_midi = midi;
    if (strncmp(path, "https://", 8) == 0 || strncmp(path, "http://", 7) == 0) {
        if (strlen(path) >= sizeof(s->url) || !websamp_url_allowed(path)) {
            w = find_name(name, 0);
            if (w && w->nslots > 0) w->nslots--;
            pthread_mutex_unlock(&web_lock);
            return 0;
        }
        snprintf(s->url, sizeof(s->url), "%s", path);
        s->ready = 1;
    } else {
        snprintf(s->url, sizeof(s->url), "%s", path);
        s->ready = 0;
    }
    pthread_mutex_unlock(&web_lock);
    return 1;
}

static void abandon_call(void) {
    pthread_mutex_lock(&web_lock);
    int wn = 0;
    for (int i = 0; i < web_n; i++) {
        WebName *w = &web_names[i];
        int kept = 0;
        for (int s = 0; s < w->nslots; s++) {
            if (w->slots[s].gen == call_gen) continue;
            if (kept != s) w->slots[kept] = w->slots[s];
            kept++;
        }
        w->nslots = kept;
        if (kept <= 0) {
            free(w->slots);
            continue;
        }
        if (wn != i) web_names[wn] = *w;
        wn++;
    }
    web_n = wn;
    pthread_mutex_unlock(&web_lock);
    call_gen++;
    if (call_gen == 0) call_gen = 1;
}

static int finish_call(const char *base) {
    int added = 0;
    pthread_mutex_lock(&web_lock);
    for (int i = 0; i < web_n; i++) {
        WebName *w = &web_names[i];
        int kept = 0;
        for (int s = 0; s < w->nslots; s++) {
            WebSlot *sl = &w->slots[s];
            if (sl->gen != call_gen) { kept++; continue; }
            if (!sl->ready) {
                char resolved[WEB_URL];
                if (!websamp_resolve(base, sl->url, resolved, (int)sizeof(resolved))) {
                    continue;
                }
                snprintf(sl->url, sizeof(sl->url), "%s", resolved);
                sl->ready = 1;
            }
            if (kept != s) w->slots[kept] = *sl;
            kept++;
            added++;
        }
        w->nslots = kept;
    }
    /* drop names that lost every slot */
    int wn = 0;
    for (int i = 0; i < web_n; i++) {
        if (web_names[i].nslots <= 0) {
            free(web_names[i].slots);
            continue;
        }
        if (wn != i) web_names[wn] = web_names[i];
        wn++;
    }
    web_n = wn;
    pthread_mutex_unlock(&web_lock);
    call_gen++;
    if (call_gen == 0) call_gen = 1;
    return added;
}

/* ---- JS object / JSON -------------------------------------------- */

typedef struct {
    const char *s;
    const char *e;
} Px;

static void px_skip(Px *p) {
    for (;;) {
        while (p->s < p->e && isspace((unsigned char)*p->s)) p->s++;
        if (p->s + 1 < p->e && p->s[0] == '/' && p->s[1] == '/') {
            while (p->s < p->e && *p->s != '\n') p->s++;
            continue;
        }
        if (p->s + 1 < p->e && p->s[0] == '/' && p->s[1] == '*') {
            p->s += 2;
            while (p->s + 1 < p->e && !(p->s[0] == '*' && p->s[1] == '/')) p->s++;
            if (p->s + 1 < p->e) p->s += 2;
            continue;
        }
        break;
    }
}

static int px_str(Px *p, char *out, int n) {
    px_skip(p);
    if (p->s >= p->e) return 0;
    char q = *p->s;
    if (q != '"' && q != '\'' && q != '`') return 0;
    p->s++;
    int k = 0;
    int ok = 0;
    while (p->s < p->e && *p->s != q) {
        char c = *p->s++;
        if (c == '\\' && p->s < p->e) c = *p->s++;
        if (k + 1 < n) out[k++] = c;
        else ok = -1;
    }
    if (p->s >= p->e || *p->s != q) return 0;
    p->s++;
    out[k] = 0;
    return ok == 0;
}

static int px_key(Px *p, char *out, int n) {
    px_skip(p);
    if (p->s >= p->e) return 0;
    if (*p->s == '"' || *p->s == '\'' || *p->s == '`') return px_str(p, out, n);
    int k = 0;
    if (!(isalpha((unsigned char)*p->s) || *p->s == '_')) return 0;
    while (p->s < p->e) {
        unsigned char c = (unsigned char)*p->s;
        if (!(isalnum(c) || c == '_' || c == '-' || c == '#')) break;
        if (k + 1 < n) out[k++] = (char)c;
        p->s++;
    }
    out[k] = 0;
    return k > 0;
}

static int px_eat(Px *p, char c) {
    px_skip(p);
    if (p->s < p->e && *p->s == c) { p->s++; return 1; }
    return 0;
}

static int parse_one_path(Px *p, const char *name, int pitched, int midi) {
    char path[WEB_URL];
    if (!px_str(p, path, (int)sizeof(path))) return 0;
    remember(name, path, pitched, midi);
    return 1;
}

static int parse_value(Px *p, const char *name) {
    px_skip(p);
    if (p->s >= p->e) return 0;
    if (*p->s == '"' || *p->s == '\'' || *p->s == '`')
        return parse_one_path(p, name, 0, 36);
    if (*p->s == '[') {
        p->s++;
        px_skip(p);
        if (px_eat(p, ']')) return 1;
        for (;;) {
            if (!parse_one_path(p, name, 0, 36)) return 0;
            px_skip(p);
            if (px_eat(p, ',')) {
                px_skip(p);
                if (p->s < p->e && *p->s == ']') { p->s++; break; }
                continue;
            }
            if (!px_eat(p, ']')) return 0;
            break;
        }
        return 1;
    }
    if (*p->s == '{') {
        p->s++;
        px_skip(p);
        if (px_eat(p, '}')) return 1;
        for (;;) {
            char key[64];
            int midi = 60;
            if (!px_key(p, key, (int)sizeof(key))) return 0;
            if (!px_eat(p, ':')) return 0;
            if (!pitch_key_midi(key, &midi)) midi = 60;
            if (px_eat(p,'[')) {
                px_skip(p);
                if (!px_eat(p,']')) {
                    for (;;) {
                        if (!parse_one_path(p,name,1,midi)) return 0;
                        if (!px_eat(p,',')) { if (!px_eat(p,']')) return 0; break; }
                        px_skip(p);
                        if (px_eat(p,']')) break;
                    }
                }
            } else if (!parse_one_path(p, name, 1, midi)) return 0;
            px_skip(p);
            if (px_eat(p, ',')) {
                px_skip(p);
                if (p->s < p->e && *p->s == '}') { p->s++; break; }
                continue;
            }
            if (!px_eat(p, '}')) return 0;
            break;
        }
        return 1;
    }
    return 0;
}

static int parse_object(Px *p, char *base, int baselen) {
    if (!px_eat(p, '{')) return 0;
    px_skip(p);
    if (px_eat(p, '}')) return 1;
    for (;;) {
        char key[64];
        if (!px_key(p, key, (int)sizeof(key))) return 0;
        if (!px_eat(p, ':')) return 0;
        if (strcmp(key, "_base") == 0) {
            char b[WEB_URL];
            if (!px_str(p, b, (int)sizeof(b))) return 0;
            if (base[0] == 0) {
                snprintf(base, (size_t)baselen, "%s", b);
            }
        } else if (!parse_value(p, key)) {
            return 0;
        }
        px_skip(p);
        if (px_eat(p, ',')) {
            px_skip(p);
            if (p->s < p->e && *p->s == '}') { p->s++; break; }
            continue;
        }
        if (!px_eat(p, '}')) return 0;
        break;
    }
    return 1;
}

static int json_add(cJSON *root, char *base, int baselen) {
    if (!cJSON_IsObject(root)) return 0;
    cJSON *b = cJSON_GetObjectItemCaseSensitive(root, "_base");
    if (cJSON_IsString(b) && b->valuestring)
        snprintf(base, (size_t)baselen, "%s", b->valuestring);
    for (cJSON *ch = root->child; ch; ch = ch->next) {
        if (!ch->string || ch->string[0] == '_') continue;
        if (cJSON_IsString(ch) && ch->valuestring) {
            remember(ch->string, ch->valuestring, 0, 36);
        } else if (cJSON_IsArray(ch)) {
            for (cJSON *el = ch->child; el; el = el->next) {
                if (cJSON_IsString(el) && el->valuestring)
                    remember(ch->string, el->valuestring, 0, 36);
            }
        } else if (cJSON_IsObject(ch)) {
            for (cJSON *el = ch->child; el; el = el->next) {
                if (!el->string) continue;
                int midi = 60;
                if (!pitch_key_midi(el->string, &midi)) midi = 60;
                if (cJSON_IsString(el) && el->valuestring) remember(ch->string,el->valuestring,1,midi);
                else if (cJSON_IsArray(el)) {
                    const cJSON *sample;
                    cJSON_ArrayForEach(sample,el)
                        if (cJSON_IsString(sample) && sample->valuestring) remember(ch->string,sample->valuestring,1,midi);
                }
            }
        }
    }
    return 1;
}

/* ---- download ---------------------------------------------------- */

static int cache_dir(char *out, int n) {
    const char *home = getenv("HOME");
    if (!home || !home[0] || strlen(home) > 400) return 0;
    char parent[512];
    snprintf(parent, sizeof(parent), "%s/.cache", home);
    mkdir(parent, 0700);
    user_dir(parent, sizeof(parent), NULL, "", ".cache/tompouce");
    if (!parent[0]) return 0;
    mkdir(parent, 0700);
    int wr = snprintf(out, (size_t)n, "%s/web", parent);
    if (wr < 0 || wr >= n) return 0;
    if (mkdir(out, 0700) != 0 && errno != EEXIST) return 0;
    return 1;
}

int websamp_is_cache_file(const char *path) {
    if (!path || path[0] != '/') return 0;
    char dir[512];
    if (!cache_dir(dir, (int)sizeof(dir))) return 0;
    char realc[PATH_MAX];
    char realp[PATH_MAX];
    if (!realpath(dir, realc) || !realpath(path, realp)) return 0;
    size_t n = strlen(realc);
    if (strncmp(realp, realc, n) != 0) return 0;
    if (realp[n] != '/') return 0;
    struct stat st;
    if (lstat(realp, &st) != 0 || S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode)) return 0;
    return 1;
}

static int addr_is_public(struct addrinfo *ai) {
    for (; ai; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET) {
            uint32_t ip = ntohl(((struct sockaddr_in *)ai->ai_addr)->sin_addr.s_addr);
            unsigned a = (ip >> 24) & 255;
            unsigned b = (ip >> 16) & 255;
            if (a == 0 || a == 10 || a == 127) return 0;
            if (a == 169 && b == 254) return 0;
            if (a == 172 && b >= 16 && b <= 31) return 0;
            if (a == 192 && b == 168) return 0;
            if (a == 100 && b >= 64 && b <= 127) return 0;
            unsigned c=(ip>>8)&255;
            if (a==192 && b==0 && (c==0 || c==2)) return 0;
            if (a==198 && (b==18 || b==19 || (b==51 && c==100))) return 0;
            if (a==203 && b==0 && c==113) return 0;
            if (a >= 224) return 0;
        } else if (ai->ai_family == AF_INET6) {
            unsigned char *b = ((struct sockaddr_in6 *)ai->ai_addr)->sin6_addr.s6_addr;
            int v4 = 1;
            for (int i = 0; i < 10; i++) if (b[i]) v4 = 0;
            if (v4 && b[10] == 0xff && b[11] == 0xff) {
                struct sockaddr_in mapped={.sin_family=AF_INET};
                memcpy(&mapped.sin_addr,b+12,4);
                struct addrinfo one={.ai_family=AF_INET,.ai_addr=(struct sockaddr *)&mapped};
                if (!addr_is_public(&one)) return 0;
                continue;
            }
            /* Global unicast only; exclude special-purpose/transition ranges
               whose embedded destinations may be private. */
            if ((b[0]&0xe0)!=0x20) return 0;
            if (b[0]==0x20 && b[1]==0x01 && b[2]<2) return 0;
            if (b[0]==0x20 && b[1]==0x01 && b[2]==0x0d && b[3]==0xb8) return 0;
            if (b[0]==0x20 && b[1]==0x02) return 0;
        } else return 0;
    }
    return 1;
}

static curl_socket_t public_socket(void *unused, curlsocktype purpose, struct curl_sockaddr *address) {
    (void)unused;
    if (purpose!=CURLSOCKTYPE_IPCXN || (address->socktype&~(SOCK_CLOEXEC|SOCK_NONBLOCK))!=SOCK_STREAM ||
        (address->family!=AF_INET && address->family!=AF_INET6) ||
        address->addrlen!=(address->family==AF_INET?sizeof(struct sockaddr_in):sizeof(struct sockaddr_in6)))
        return CURL_SOCKET_BAD;
    struct addrinfo peer={.ai_family=address->family,.ai_addr=&address->addr};
    /* Validate exactly the numeric destination curl will connect to. No
       separate DNS lookup can change the answer after this check. */
    if (!addr_is_public(&peer)) return CURL_SOCKET_BAD;
    return socket(address->family,address->socktype|SOCK_CLOEXEC,address->protocol);
}

struct WebMem {
    char *data;
    size_t size;
    size_t cap;
};

static size_t mem_write(void *contents, size_t size, size_t nmemb, void *userp) {
    if (nmemb != 0 && size > (size_t)-1 / nmemb) return 0;
    size_t realsize = size * nmemb;
    struct WebMem *m = userp;
    if (m->size > m->cap || realsize > m->cap - m->size) return 0;
    char *ptr = realloc(m->data, m->size + realsize + 1);
    if (!ptr) return 0;
    m->data = ptr;
    memcpy(m->data + m->size, contents, realsize);
    m->size += realsize;
    m->data[m->size] = 0;
    return realsize;
}

struct WebFile {
    FILE *f;
    size_t n;
};

static size_t file_write(void *contents, size_t size, size_t nmemb, void *userp) {
    if (nmemb != 0 && size > (size_t)-1 / nmemb) return 0;
    size_t realsize = size * nmemb;
    struct WebFile *w = userp;
    if (w->n > WEB_AUDIO_MAX || realsize > WEB_AUDIO_MAX - w->n) return 0;
    if (fwrite(contents, 1, realsize, w->f) != realsize) return 0;
    w->n += realsize;
    return realsize;
}

static CURL *curl_new(const char *url) {
    if (!websamp_url_allowed(url)) return NULL;
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    /* Proxy-side resolution would bypass the destination policy. Remote
       assets use direct connections, regardless of ambient proxy settings. */
    if (curl_easy_setopt(curl,CURLOPT_PROXY,"")!=CURLE_OK ||
        curl_easy_setopt(curl,CURLOPT_OPENSOCKETFUNCTION,public_socket)!=CURLE_OK) {
        curl_easy_cleanup(curl); return NULL;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "tompouce/1.0");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    return curl;
}

static int http_get_mem(const char *url, struct WebMem *m) {
    char cur[WEB_URL];
    snprintf(cur, sizeof(cur), "%s", url);
    for (int hop = 0; hop < 4; hop++) {
        if (!websamp_url_allowed(cur)) return 0;
        CURL *curl = curl_new(cur);
        if (!curl) return 0;
        m->data = NULL;
        m->size = 0;
        m->cap = WEB_JSON_MAX;
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mem_write);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, m);
        CURLcode rc = curl_easy_perform(curl);
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        char *loc = NULL;
        if (rc == CURLE_OK && code >= 300 && code < 400)
            curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &loc);
        char next[WEB_URL];
        next[0] = 0;
        if (loc) snprintf(next, sizeof(next), "%s", loc);
        curl_easy_cleanup(curl);
        if (rc != CURLE_OK) { free(m->data); m->data = NULL; return 0; }
        if (code == 200 && m->data && m->size > 0) return 1;
        free(m->data);
        m->data = NULL;
        m->size = 0;
        if (next[0] == 0) return 0;
        snprintf(cur, sizeof(cur), "%s", next);
    }
    return 0;
}

char *websamp_fetch_text(const char *url) {
    if (!url || strlen(url) >= WEB_URL) return NULL;
    char resolved[WEB_URL];
    if (!websamp_resolve("",url,resolved,sizeof(resolved))) return NULL;
    struct WebMem memory = {0};
    if (!http_get_mem(resolved,&memory)) return NULL;
    return memory.data;
}

static const char *url_ext(const char *url) {
    const char *q = strchr(url, '?');
    const char *h = strchr(url, '#');
    const char *end = url + strlen(url);
    if (q && q < end) end = q;
    if (h && h < end) end = h;
    const char *dot = NULL;
    for (const char *p = url; p < end; p++) if (*p == '.') dot = p;
    const char *slash = NULL;
    for (const char *p = url; p < end; p++) if (*p == '/') slash = p;
    if (!dot || (slash && dot < slash)) return "";
    static char ext[8];
    size_t n = (size_t)(end - dot);
    if (n >= sizeof(ext)) return "";
    for (size_t i = 0; i < n; i++) ext[i] = (char)tolower((unsigned char)dot[i]);
    ext[n] = 0;
    if (strcmp(ext, ".wav") && strcmp(ext, ".mp3") && strcmp(ext, ".flac") && strcmp(ext, ".fla"))
        return "";
    return ext;
}

static int audio_magic_ok(const char *path, const char *ext) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char b[16];
    size_t n = fread(b, 1, sizeof(b), f);
    fclose(f);
    if (n < 12) return 0;
    if (strcmp(ext, ".wav") == 0)
        return memcmp(b, "RIFF", 4) == 0 && memcmp(b + 8, "WAVE", 4) == 0;
    if (strcmp(ext, ".flac") == 0 || strcmp(ext, ".fla") == 0)
        return memcmp(b, "fLaC", 4) == 0;
    if (strcmp(ext, ".mp3") == 0)
        return (b[0] == 'I' && b[1] == 'D' && b[2] == '3') || b[0] == 0xff;
    return 0;
}

static uint64_t fnv64(const char *s) {
    uint64_t h = 14695981039346656037ull;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        h ^= *p;
        h *= 1099511628211ull;
    }
    return h;
}

static int download_audio_unlocked(const char *url, char *path_out, int pathlen) {
    const char *ext = url_ext(url);
    if (!ext[0]) return 0;
    char dir[512];
    if (!cache_dir(dir, (int)sizeof(dir))) return 0;
    char final[512];
    int wr = snprintf(final, sizeof(final), "%s/%016llx%s", dir,
                      (unsigned long long)fnv64(url), ext);
    if (wr < 0 || wr >= (int)sizeof(final) || wr >= pathlen) return 0;
    struct stat st;
    if (stat(final, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 32 &&
        audio_magic_ok(final, ext)) {
        snprintf(path_out, (size_t)pathlen, "%s", final);
        return 1;
    }
    char part[540];
    snprintf(part, sizeof(part), "%s.part", final);
    char cur[WEB_URL];
    snprintf(cur, sizeof(cur), "%s", url);
    for (int hop = 0; hop < 4; hop++) {
        if (!websamp_url_allowed(cur)) {
            unlink(part);
            return 0;
        }
        FILE *f = fopen(part, "wb");
        if (!f) return 0;
        struct WebFile wf = { f, 0 };
        CURL *curl = curl_new(cur);
        if (!curl) { fclose(f); unlink(part); return 0; }
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, file_write);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wf);
        CURLcode rc = curl_easy_perform(curl);
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        char *loc = NULL;
        if (rc == CURLE_OK && code >= 300 && code < 400)
            curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &loc);
        char next[WEB_URL];
        next[0] = 0;
        if (loc) snprintf(next, sizeof(next), "%s", loc);
        curl_easy_cleanup(curl);
        fclose(f);
        if (rc != CURLE_OK || wf.n < 32) { unlink(part); return 0; }
        if (code == 200) {
            if (!audio_magic_ok(part, ext)) { unlink(part); return 0; }
            if (rename(part, final) != 0) { unlink(part); return 0; }
            snprintf(path_out, (size_t)pathlen, "%s", final);
            return 1;
        }
        unlink(part);
        if (!next[0]) return 0;
        snprintf(cur, sizeof(cur), "%s", next);
    }
    return 0;
}

static int download_audio(const char *url, char *path_out, int pathlen) {
    pthread_mutex_t *lock=&download_locks[fnv64(url)%DOWNLOAD_LOCK_COUNT];
    pthread_mutex_lock(lock);
    int ok = download_audio_unlocked(url, path_out, pathlen);
    pthread_mutex_unlock(lock);
    return ok;
}

static int load_json_text(const char *text, const char *fallback_base) {
    cJSON *root = cJSON_Parse(text);
    if (!root) return 0;
    char base[WEB_URL];
    base[0] = 0;
    if (fallback_base) snprintf(base, sizeof(base), "%s", fallback_base);
    int ok = json_add(root, base, (int)sizeof(base));
    cJSON_Delete(root);
    if (!ok) return 0;
    return finish_call(base);
}

static int fetch_json_url(const char *url, const char *fallback_base) {
    struct WebMem m = {0};
    if (!http_get_mem(url, &m)) return 0;
    const char *t = m.data ? m.data : "";
    while (*t && isspace((unsigned char)*t)) t++;
    int n = (*t == '{') ? load_json_text(t, fallback_base) : 0;
    free(m.data);
    return n;
}

static int fetch_github_json(const char *spec) {
    char url[WEB_URL];
    if (!websamp_github_to_url(spec, 1, url, (int)sizeof(url))) return 0;
    /* Directory that holds strudel.json, used when the file has no _base. */
    char dir[WEB_URL];
    snprintf(dir, sizeof(dir), "%s", url);
    char *slash = strrchr(dir, '/');
    if (slash) slash[1] = 0;
    if (fetch_json_url(url, dir)) return 1;
    /* Documented default branch is main. Dirt-Samples still publishes master. */
    const char *p = spec + 7;
    int parts = 0;
    int trailing = 0;
    while (*p) {
        while (*p == '/') { trailing = 1; p++; }
        if (!*p) break;
        trailing = 0;
        parts++;
        while (*p && *p != '/') p++;
    }
    if (parts >= 3 || trailing) return 0;
    char retry[WEB_URL];
    snprintf(retry, sizeof(retry), "%s", spec);
    size_t L = strlen(retry);
    while (L > 0 && retry[L - 1] == '/') { retry[--L] = 0; }
    if (L + 8 >= sizeof(retry)) return 0;
    memcpy(retry + L, "/master", 8);
    if (!websamp_github_to_url(retry, 1, url, (int)sizeof(url))) return 0;
    snprintf(dir, sizeof(dir), "%s", url);
    slash = strrchr(dir, '/');
    if (slash) slash[1] = 0;
    return fetch_json_url(url, dir);
}

/* ---- call scan --------------------------------------------------- */

static int parse_call(const char *args, int len) {
    Px p = { args, args + len };
    px_skip(&p);
    if (p.s >= p.e) return 0;
    if (*p.s == '"' || *p.s == '\'' || *p.s == '`') {
        char spec[WEB_URL];
        if (!px_str(&p, spec, (int)sizeof(spec))) return 0;
        if (!strcmp(spec,"bubo:waveforms")) return waveform_installed()?1:fetch_github_json(WAVEFORM_SPEC);
        if (strncmp(spec, "github:", 7) == 0) return fetch_github_json(spec);
        if (strncmp(spec, "https://", 8) == 0 || strncmp(spec, "http://", 7) == 0) {
            if (!websamp_url_allowed(spec)) return 0;
            char dir[WEB_URL];
            snprintf(dir, sizeof(dir), "%s", spec);
            char *slash = strrchr(dir, '/');
            if (slash) slash[1] = 0;
            return fetch_json_url(spec, dir);
        }
        return 0;
    }
    char base[WEB_URL];
    base[0] = 0;
    if (!parse_object(&p, base, (int)sizeof(base))) {
        abandon_call();
        return 0;
    }
    if (px_eat(&p, ',')) {
        char b[WEB_URL];
        if (!px_str(&p, b, (int)sizeof(b))) {
            abandon_call();
            return 0;
        }
        snprintf(base, sizeof(base), "%s", b);
    }
    return finish_call(base);
}

static void scan_calls(const char *code) {
    const char *s = code;
    int q = 0;
    int linecom = 0;
    int blockcom = 0;
    for (const char *p = s; *p; p++) {
        if (linecom) {
            if (*p == '\n') linecom = 0;
            continue;
        }
        if (blockcom) {
            if (*p == '*' && p[1] == '/') { blockcom = 0; p++; }
            continue;
        }
        if (q) {
            if (*p == '\\' && p[1]) { p++; continue; }
            if (*p == q) q = 0;
            continue;
        }
        if (*p == '/' && p[1] == '/') { linecom = 1; p++; continue; }
        if (*p == '/' && p[1] == '*') { blockcom = 1; p++; continue; }
        if (*p == '"' || *p == '\'' || *p == '`') { q = *p; continue; }
        if (strncmp(p, "samples", 7) != 0) continue;
        if (p > s && is_ident_char((unsigned char)p[-1])) continue;
        const char *a = p + 7;
        while (*a == ' ' || *a == '\t' || *a == '\n' || *a == '\r') a++;
        if (*a != '(') continue;
        a++;
        int dq = 0, depth = 1, lc = 0, bc = 0;
        const char *e = a;
        for (; *e; e++) {
            if (lc) { if (*e == '\n') lc = 0; continue; }
            if (bc) {
                if (*e == '*' && e[1] == '/') { bc = 0; e++; }
                continue;
            }
            if (dq) {
                if (*e == '\\' && e[1]) { e++; continue; }
                if (*e == dq) dq = 0;
                continue;
            }
            if (*e == '/' && e[1] == '/') { lc = 1; e++; continue; }
            if (*e == '/' && e[1] == '*') { bc = 1; e++; continue; }
            if (*e == '"' || *e == '\'' || *e == '`') { dq = *e; continue; }
            if (*e == '(') depth++;
            else if (*e == ')') {
                depth--;
                if (depth == 0) break;
            }
        }
        if (*e != ')' || depth != 0) continue;
        size_t n = (size_t)(e - a);
        if (n > 0 && n < WEB_JSON_MAX) {
            int got = parse_call(a, (int)n);
            if (got <= 0) log_console("samples: map not loaded");
        }
        p = e;
    }
}

void websamp_load_code(const char *code) {
    pthread_mutex_lock(&web_lock);
    slot_clear_all();
    pthread_mutex_unlock(&web_lock);
    if (!code || !code[0]) return;
    scan_calls(code);
    int n = 0;
    pthread_mutex_lock(&web_lock);
    n = web_n;
    pthread_mutex_unlock(&web_lock);
    if (n > 0) log_console("samples: %d name%s", n, n == 1 ? "" : "s");
}

void websamp_load_arguments(const char *json) {
    pthread_mutex_lock(&web_lock);
    slot_clear_all();
    pthread_mutex_unlock(&web_lock);
    if (!json || strlen(json) >= WEB_JSON_MAX) return;
    cJSON *calls = cJSON_Parse(json);
    if (!cJSON_IsArray(calls) || cJSON_GetArraySize(calls) > 64) { cJSON_Delete(calls); return; }
    const cJSON *call;
    cJSON_ArrayForEach(call,calls) {
        if (!cJSON_IsArray(call) || cJSON_GetArraySize(call) > 3) continue;
        char *args = cJSON_PrintUnformatted(call);
        if (!args) continue;
        size_t n = strlen(args);
        if (n > 2 && n < WEB_JSON_MAX && parse_call(args+1,(int)n-2) <= 0)
            log_console("samples: map not loaded");
        free(args);
    }
    cJSON_Delete(calls);
}

static void split_token(const char *token, char *name, size_t nlen, int *idx) {
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

static int select_slot(const WebName *w, int index, double midi) {
    if (!w || w->nslots <= 0) return -1;
    int pitched = w->slots[0].pitched;
    if (!pitched) { int slot = index%w->nslots; return slot < 0 ? slot+w->nslots : slot; }
    double target = isfinite(midi) && midi >= 0 ? midi : 36;
    int best = 0;
    for (int i = 1; i < w->nslots; i++)
        if (fabs(w->slots[i].base_midi-target) < fabs(w->slots[best].base_midi-target)) best = i;
    int matches[WEB_SLOTS_MAX], count = 0;
    for (int i = 0; i < w->nslots; i++)
        if (w->slots[i].base_midi == w->slots[best].base_midi) matches[count++] = i;
    int slot = index%count;
    return matches[slot < 0 ? slot+count : slot];
}

/* Default catalogs. User samples() stays in web_names and wins.
   Installed manifests retain their array order and pitch-keyed regions.
   Local files are preferred, with the manifest URL kept. The
   uzu-drumkit map is last, matching current strudel.cc prebake, so
   bare hh / mt / lt / ht / misc are those URLs. */
#define DEF_NAMES 2048
#define DEF_SLOT_CAP WEB_SLOTS_MAX
#define DEF_TOTAL_SLOTS 16384

/* Share selection semantics with user maps, including fractional pitch
   region selection and wrapping n within the chosen region. */
typedef WebSlot DefSlot;
typedef WebName DefName;

static DefName def_names[DEF_NAMES];
static int def_n;
static int def_slots;
#define MANIFEST_FILES_MAX 150000
#define MANIFEST_FILE_BUCKETS 262144
static char *manifest_files[MANIFEST_FILES_MAX];
static char *manifest_file_hash[MANIFEST_FILE_BUCKETS];
static int manifest_file_count;

static int manifest_file_compare(const void *a, const void *b) {
    return strcmp(*(const char *const *)a,*(const char *const *)b);
}

static const char UZU_JSON[] =
    "{\"_base\":\"https://raw.githubusercontent.com/tidalcycles/uzu-drumkit/main/\","
    "\"bd\":[\"bd/10_bd_switchangel.wav\",\"bd/11_bd_mot4i.wav\",\"bd/12_bd_mot4i.wav\","
    "\"bd/13_bd_mot4i.wav\",\"bd/14_bd_switchangel.wav\",\"bd/15_bd_switchangel.wav\","
    "\"bd/16_bd_switchangel.wav\",\"bd/17_bd_switchangel.wav\"],"
    "\"brk\":[\"brk/10_break_amen_pprocessed.wav\"],"
    "\"cb\":[\"cb/10_perc_switchangel.wav\"],"
    "\"cp\":[\"cp/10_cp_switchangel.wav\",\"cp/11_cp_mot4i.wav\"],"
    "\"cr\":[\"cr/10_cr_switchangel.wav\",\"cr/11_cr_mot4i.wav\"],"
    "\"hh\":[\"hh/10_hh_switchangel.wav\",\"hh/11_hh_mot4i.wav\",\"hh/12_hh_switchangel.wav\","
    "\"hh/13_hh_switchangel.wav\",\"hh/14_hh_mot4i.wav\"],"
    "\"ht\":[\"ht/10_ht_mot4i.wav\"],"
    "\"lt\":[\"lt/10_lt_mot4i.wav\"],"
    "\"misc\":[\"misc/10_misc_switchangel_ludens.wav\",\"misc/11_misc_switchangel_ludens.wav\","
    "\"misc/12_misc_switchangel_ludens.wav\",\"misc/13_misc_switchangel_ludens.wav\","
    "\"misc/14_misc_switchangel_ludens.wav\"],"
    "\"mt\":[\"mt/10_mt_mot4i.wav\"],"
    "\"oh\":[\"oh/10_oh_switchangel.wav\",\"oh/11_oh_switchangel.wav\",\"oh/12_oh_switchangel.wav\","
    "\"oh/13_oh_switchangel.wav\"],"
    "\"rd\":[\"rd/10_rd_switchangel.wav\"],"
    "\"rim\":[\"rim/10_rim_switchangel.wav\",\"rim/11_rim_switch_angel.wav\"],"
    "\"sd\":[\"sd/10_sd_switchangel-bounce-2.wav\",\"sd/11_sd_switchangel_3.wav\","
    "\"sd/12_sd_switchangel_2.wav\",\"sd/13_sd_switchangel_2.wav\",\"sd/14_sd.wav\"],"
    "\"sh\":[\"sh/10_sh_switchangel.wav\"],"
    "\"tb\":[\"tb/10_tb.wav\"]}";

static DefName *def_find(const char *name, int create) {
    char key[64];
    int i;
    if (!name || !name[0] || strlen(name) >= sizeof(key)) return NULL;
    lower_copy(key, (int)sizeof(key), name);
    if (!key[0] || key[0] == '_') return NULL;
    for (i = 0; i < def_n; i++)
        if (strcmp(def_names[i].name, key) == 0) return &def_names[i];
    if (!create || def_n >= DEF_NAMES) return NULL;
    memset(&def_names[def_n], 0, sizeof(def_names[def_n]));
    snprintf(def_names[def_n].name, sizeof(def_names[def_n].name), "%s", key);
    return &def_names[def_n++];
}

static int def_reset(const char *name) {
    DefName *w = def_find(name, 1);
    if (!w) return 0;
    def_slots -= w->nslots;
    free(w->slots);
    w->slots = NULL;
    w->nslots = 0;
    return 1;
}

static int def_push(const char *name, const char *url, const char *local, int pitched, int midi) {
    DefName *w = def_find(name, 0);
    DefSlot *ns;
    DefSlot *s;
    if (!w || w->nslots >= DEF_SLOT_CAP || def_slots >= DEF_TOTAL_SLOTS) return 0;
    if (url && url[0] && (strlen(url) >= WEB_URL || !websamp_url_allowed(url))) url = "";
    if ((!url || !url[0]) && (!local || !local[0])) return 0;
    ns = realloc(w->slots, (size_t)(w->nslots + 1) * sizeof(DefSlot));
    if (!ns) return 0;
    w->slots = ns;
    s = &w->slots[w->nslots++];
    memset(s, 0, sizeof(*s));
    s->pitched = pitched;
    s->base_midi = midi;
    def_slots++;
    if (url && url[0]) snprintf(s->url, sizeof(s->url), "%s", url);
    if (local && local[0]) snprintf(s->local, sizeof(s->local), "%s", local);
    return 1;
}

static void def_compact(void) {
    int n = 0;
    int i;
    for (i = 0; i < def_n; i++) {
        if (def_names[i].nslots <= 0) {
            free(def_names[i].slots);
            def_names[i].slots = NULL;
            continue;
        }
        if (n != i) def_names[n] = def_names[i];
        n++;
    }
    def_n = n;
}

static int rel_safe(const char *rel) {
    const char *p;
    if (!rel || !rel[0] || rel[0] == '/' || rel[0] == '\\') return 0;
    if (strstr(rel, "..") || strchr(rel, '\\')) return 0;
    for (p = rel; *p; p++)
        if ((unsigned char)*p < 32 || (unsigned char)*p == 127) return 0;
    return 1;
}

static int local_audio(const char *dir, const char *rel, char *out, size_t n) {
    struct stat st;
    char resolved[PATH_MAX];
    char decoded[WEB_URL];
    int w;
    if (!dir || !dir[0] || !rel_safe(rel)) return 0;
    /* Catalog paths are URL paths (VCSL contains percent-encoded spaces).
       Validate again after decoding, including embedded NUL/traversal. */
    if (strchr(rel, '%')) {
        int length = 0;
        char *plain = curl_easy_unescape(NULL, rel, 0, &length);
        if (!plain) return 0;
        int safe = length > 0 && (size_t)length < sizeof(decoded) &&
            !memchr(plain,0,(size_t)length) && rel_safe(plain);
        if (safe) memcpy(decoded,plain,(size_t)length+1);
        curl_free(plain);
        if (!safe) return 0;
        rel = decoded;
    }
    w = snprintf(out, n, "%s/%s", dir, rel);
    if (w < 0 || (size_t)w >= n) return 0;
    /* Reject links in manifest subdirectories as well as at the leaf. */
    for (char *p = out + strlen(dir) + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        int safe = lstat(out, &st) == 0 && S_ISDIR(st.st_mode);
        *p = '/';
        if (!safe) { out[0] = 0; return 0; }
    }
    if (lstat(out, &st) != 0 || !S_ISREG(st.st_mode)) {
        out[0] = 0;
        return 0;
    }
    if (!realpath(out, resolved) || strlen(resolved) >= n) {
        out[0] = 0;
        return 0;
    }
    snprintf(out, n, "%s", resolved);
    return 1;
}

static void def_add_path(const char *name, const char *path, const char *base,
                         const char *dir, int pitched, int midi) {
    char url[WEB_URL];
    char local[512];
    url[0] = 0;
    local[0] = 0;
    if (!path || !path[0]) return;
    if (strncmp(path, "https://", 8) == 0 || strncmp(path, "http://", 7) == 0) {
        if (strlen(path) < sizeof(url) && websamp_url_allowed(path))
            snprintf(url, sizeof(url), "%s", path);
    } else if (!websamp_resolve(base ? base : "", path, url, (int)sizeof(url))) {
        url[0] = 0;
    }
    if (dir) local_audio(dir, path, local, sizeof(local));
    def_push(name, url, local, pitched, midi);
}

static void manifest_claim_value(const cJSON *value, const char *directory, int depth) {
    if (!directory || !*directory || depth>3) return;
    if (cJSON_IsString(value)) {
        char local[PATH_MAX];
        if (!local_audio(directory,value->valuestring,local,sizeof(local))) return;
        uint32_t hash=2166136261u;
        for (const unsigned char *p=(const unsigned char *)local;*p;p++) hash=(hash^*p)*16777619u;
        unsigned slot=hash&(MANIFEST_FILE_BUCKETS-1);
        while (manifest_file_hash[slot]) {
            if (!strcmp(manifest_file_hash[slot],local)) return;
            slot=(slot+1)&(MANIFEST_FILE_BUCKETS-1);
        }
        if (manifest_file_count>=MANIFEST_FILES_MAX) return;
        char *copy=strdup(local);
        if (copy) { manifest_file_hash[slot]=copy; manifest_files[manifest_file_count++]=copy; }
    } else if (cJSON_IsArray(value) || cJSON_IsObject(value)) {
        const cJSON *child;
        cJSON_ArrayForEach(child,value) manifest_claim_value(child,directory,depth+1);
    }
}

static void def_add_value(const char *name, const cJSON *value, const char *base,
                          const char *dir, int pitched, int midi) {
    if (cJSON_IsString(value) && value->valuestring) {
        def_add_path(name, value->valuestring, base, dir, pitched, midi);
    } else if (cJSON_IsArray(value)) {
        const cJSON *el;
        cJSON_ArrayForEach(el, value)
            if (cJSON_IsString(el) && el->valuestring)
                def_add_path(name, el->valuestring, base, dir, pitched, midi);
    }
}

static int manifest_machine_key(const char *key) {
    const char *suffix=strrchr(key,'_');
    const char *parts[]={"_bd","_sd","_hh","_oh","_cp","_cb","_cr","_ht","_mt","_lt","_rd","_sh","_tb","_rim","_rs","_perc"};
    if (!suffix) return 0;
    for (size_t i=0;i<sizeof(parts)/sizeof(*parts);i++) if (!strcasecmp(suffix,parts[i])) return 1;
    return 0;
}

static void def_ingest(cJSON *root, const char *local_dir, int machine) {
    char base[WEB_URL];
    cJSON *b;
    cJSON *ch;
    base[0] = 0;
    if (!cJSON_IsObject(root)) return;
    b = cJSON_GetObjectItemCaseSensitive(root, "_base");
    if (cJSON_IsString(b) && b->valuestring && strlen(b->valuestring) < sizeof(base))
        snprintf(base, sizeof(base), "%s", b->valuestring);
    for (ch = root->child; ch; ch = ch->next) {
        if (!ch->string || ch->string[0] == '_') continue;
        manifest_claim_value(ch,local_dir,0);
        if (!def_reset(ch->string)) continue;
        def_find(ch->string,0)->machine = machine<0?manifest_machine_key(ch->string):machine;
        if (cJSON_IsObject(ch)) {
            const cJSON *region;
            cJSON_ArrayForEach(region, ch) {
                int midi;
                if (!region->string || !pitch_key_midi(region->string, &midi)) continue;
                def_add_value(ch->string, region, base, local_dir, 1, midi);
            }
        } else {
            def_add_value(ch->string, ch, base, local_dir, 0, 36);
        }
    }
}

static char *read_capped(const char *path, size_t cap) {
    struct stat st;
    FILE *f;
    char *buf;
    size_t n;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) return NULL;
    if (st.st_size <= 0 || (size_t)st.st_size >= cap) return NULL;
    f = fopen(path, "rb");
    if (!f) return NULL;
    buf = malloc((size_t)st.st_size + 1);
    if (!buf) { fclose(f); return NULL; }
    n = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);
    if (n != (size_t)st.st_size) { free(buf); return NULL; }
    buf[n] = 0;
    return buf;
}

static int try_catalog(const char *root, const char *manifest, const char *samples,
                       char *json, size_t jn, char *dir, size_t dn) {
    struct stat st;
    int w;
    if (!root || !root[0]) return 0;
    w = snprintf(dir, dn, "%s/%s", root, samples);
    if (w < 0 || (size_t)w >= dn) return 0;
    w = snprintf(json, jn, "%s/%s", root, manifest);
    if (w < 0 || (size_t)w >= jn) return 0;
    if (lstat(json, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    return 1;
}

static int find_catalog(const char *manifest, const char *samples,
                        char *json, size_t jn, char *dir, size_t dn) {
    char root[1024];
    char exe[1024];
    struct stat st;
    const char *env = getenv("TOMPOUCE_BANKS");
    if (!env || !env[0]) env = getenv("STRUDELTERM_BANKS");
    ssize_t n;
    if (env && strlen(env) < 900) {
        snprintf(root, sizeof(root), "%s", env);
        if (stat(root, &st) == 0 && S_ISDIR(st.st_mode) &&
            try_catalog(root, manifest, samples, json, jn, dir, dn))
            return 1;
    }
    if (try_catalog("./assets/banks", manifest, samples, json, jn, dir, dn)) return 1;
    n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0 && (size_t)n < sizeof(exe)) {
        char *slash;
        exe[n] = 0;
        slash = strrchr(exe, '/');
        if (slash) {
            *slash = 0;
            int w = snprintf(root, sizeof(root), "%s/assets/banks", exe);
            if (w > 0 && (size_t)w < sizeof(root) &&
                try_catalog(root, manifest, samples, json, jn, dir, dn))
                return 1;
        }
    }
    return 0;
}

static void load_default_catalog(const char *manifest, const char *samples) {
    char json_path[1200];
    char dir[1024];
    char *text;
    json_path[0] = 0;
    dir[0] = 0;
    if (find_catalog(manifest, samples, json_path, sizeof(json_path), dir, sizeof(dir))) {
        text = read_capped(json_path, WEB_JSON_MAX);
        if (text) {
            cJSON *root = cJSON_Parse(text);
            free(text);
            if (root) {
                def_ingest(root, dir, strstr(manifest,"tidal-drum-machines") != NULL);
                cJSON_Delete(root);
            }
        }
    }
}

static int waveform_installed(void) {
    char path[1200], directory[1024];
    return find_catalog(WAVEFORM_CATALOG,"Dough-Waveforms",path,sizeof(path),directory,sizeof(directory));
}

static int catalog_audio_value(const cJSON *value, int depth) {
    if (depth>3) return 0;
    if (cJSON_IsString(value)) {
        const char *dot=strrchr(value->valuestring,'.');
        return dot && (!strcasecmp(dot,".wav") || !strcasecmp(dot,".mp3") ||
                       !strcasecmp(dot,".flac") || !strcasecmp(dot,".fla"));
    }
    if (cJSON_IsArray(value) || cJSON_IsObject(value)) {
        const cJSON *child;
        cJSON_ArrayForEach(child,value) if (catalog_audio_value(child,depth+1)) return 1;
    }
    return 0;
}

/* Discover additional local Strudel maps once, outside the audio callback.
 * Known catalogs below are then applied in their established precedence order.
 * Bounds and symlink rejection match the bank scanner. */
static void discover_catalogs(const char *directory, int depth, int *visited, int *loaded) {
    if (depth>=32 || *visited>=200000 || *loaded>=512) return;
    (*visited)++;
    struct dirent **names=NULL;
    int count=scandir(directory,&names,NULL,alphasort);
    if (count<0) return;
    for (int i=0;i<count;i++) {
        const char *name=names[i]->d_name;
        char path[PATH_MAX]; struct stat st;
        int n=snprintf(path,sizeof(path),"%s/%s",directory,name);
        if (name[0]!='.' && n>0 && (size_t)n<sizeof(path) && !lstat(path,&st)) {
            if (S_ISDIR(st.st_mode)) discover_catalogs(path,depth+1,visited,loaded);
            else if (S_ISREG(st.st_mode) && *loaded<512 && strlen(name)>5 && !strcasecmp(name+strlen(name)-5,".json")) {
                char *text=read_capped(path,WEB_JSON_MAX);
                cJSON *root=text?cJSON_ParseWithOpts(text,NULL,1):NULL; free(text);
                int valid=0, machine=strstr(name,"tidal-drum-machines")?1:-1;
                if (cJSON_IsObject(root)) {
                    const cJSON *child;
                    cJSON_ArrayForEach(child,root) {
                        if (!child->string || child->string[0]=='_' || !catalog_audio_value(child,0)) continue;
                        valid=1;
                    }
                }
                if (valid) { def_ingest(root,directory,machine); (*loaded)++; }
                cJSON_Delete(root);
            }
        }
        free(names[i]);
    }
    free(names);
}

static void load_defaults(void) {
    int visited=0, loaded=0;
    char root[PATH_MAX]; root[0]=0;
    const char *configured=getenv("TOMPOUCE_BANKS");
    if (!configured || !*configured) configured=getenv("STRUDELTERM_BANKS");
    struct stat st;
    if (configured && !stat(configured,&st) && S_ISDIR(st.st_mode)) snprintf(root,sizeof(root),"%s",configured);
    else if (banks_root_dir()[0]) snprintf(root,sizeof(root),"%s",banks_root_dir());
    else if (!stat("./assets/banks",&st) && S_ISDIR(st.st_mode)) snprintf(root,sizeof(root),"./assets/banks");
    else {
        char exe[PATH_MAX]; ssize_t n=readlink("/proc/self/exe",exe,sizeof(exe)-1);
        if (n>0 && (size_t)n<sizeof(exe)) {
            exe[n]=0; char *slash=strrchr(exe,'/');
            if (slash) { *slash=0; int len=snprintf(root,sizeof(root),"%s/assets/banks",exe);
                if (len<0 || (size_t)len>=sizeof(root)) root[0]=0; }
        }
    }
    if (root[0]) discover_catalogs(root,0,&visited,&loaded);
    load_default_catalog("Dirt-Samples/strudel.json", "Dirt-Samples");
    load_default_catalog("dough-samples/vcsl.json", "VCSL");
    load_default_catalog("dough-samples/mridangam.json", "mrid");
    load_default_catalog("dough-samples/tidal-drum-machines.json", "tidal-drum-machines/machines");
    load_default_catalog("dough-samples/piano.json", "dough-samples/piano");
    load_default_catalog(WAVEFORM_CATALOG,"Dough-Waveforms");
    cJSON *uzu = cJSON_Parse(UZU_JSON);
    if (uzu) {
        char manifest[1200], directory[1024];
        directory[0] = 0;
        (void)find_catalog("uzu-drumkit/strudel.json","uzu-drumkit",manifest,sizeof(manifest),directory,sizeof(directory));
        def_ingest(uzu, directory[0] ? directory : NULL, 0);
        cJSON_Delete(uzu);
    }
    def_compact();
    qsort(manifest_files,(size_t)manifest_file_count,sizeof(*manifest_files),manifest_file_compare);
    if (def_n > 0) log_console("samples: default map %d names", def_n);
}

static void ensure_defaults(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, load_defaults);
}

int websamp_manifest_contains(const char *absolute_path) {
    ensure_defaults();
    return absolute_path && bsearch(&absolute_path,manifest_files,(size_t)manifest_file_count,
                                    sizeof(*manifest_files),manifest_file_compare)!=NULL;
}

static void def_mark_failed(const char *name, const char *url) {
    DefName *d;
    int i;
    if (!name || !url || !url[0]) return;
    pthread_mutex_lock(&web_lock);
    d = def_find(name, 0);
    if (d) {
        for (i = 0; i < d->nslots; i++)
            if (strcmp(d->slots[i].url, url) == 0) d->slots[i].failed = 1;
    }
    pthread_mutex_unlock(&web_lock);
}

static int acquire_default(const char *name, int index, double midi) {
    char url[WEB_URL];
    char local[512];
    char key[64];
    char path[512];
    int failed = 0;
    int id;
    DefName *d;
    DefSlot *s;
    int pick;
    int base_midi;
    url[0] = 0;
    local[0] = 0;
    key[0] = 0;
    pthread_mutex_lock(&web_lock);
    d = def_find(name, 0);
    if (!d || d->nslots <= 0) { pthread_mutex_unlock(&web_lock); return -1; }
    pick = select_slot(d, index, midi);
    s = &d->slots[pick];
    if ((!s->url[0] && !s->local[0]) || (s->failed && !s->local[0])) {
        pthread_mutex_unlock(&web_lock);
        return -1;
    }
    snprintf(url, sizeof(url), "%s", s->url);
    snprintf(local, sizeof(local), "%s", s->local);
    snprintf(key, sizeof(key), "%s", d->name);
    failed = s->failed;
    base_midi = s->base_midi;
    pthread_mutex_unlock(&web_lock);
    if (local[0]) {
        id = smp_acquire_abs(local, base_midi);
        if (id >= 0) return id;
    }
    if (!url[0] || failed) return -1;
    if (!download_audio(url, path, (int)sizeof(path))) {
        def_mark_failed(key, url);
        log_console("samples: fetch failed %.80s", url);
        return -1;
    }
    pthread_mutex_lock(&web_lock);
    d = def_find(key, 0);
    if (d) {
        int i;
        for (i = 0; i < d->nslots; i++)
            if (strcmp(d->slots[i].url, url) == 0 && !d->slots[i].local[0])
                snprintf(d->slots[i].local, sizeof(d->slots[i].local), "%s", path);
    }
    pthread_mutex_unlock(&web_lock);
    log_console("samples: %s", key);
    return smp_acquire_file(path, base_midi);
}

int websamp_slot_for(const char *token, double midi) {
    char name[64]; int index;
    if (!token || strcspn(token,":") >= sizeof(name)) return -1;
    split_token(token,name,sizeof(name),&index);
    pthread_mutex_lock(&web_lock);
    int slot = select_slot(find_name(name,0),index,midi);
    pthread_mutex_unlock(&web_lock);
    return slot;
}

int websamp_acquire(const char *token, double midi) {
    char name[64];
    if (!token || strcspn(token,":") >= sizeof(name)) return -1;
    int index = -1;
    split_token(token, name, sizeof(name), &index);
    char url[WEB_URL];
    int base_midi = 36;
    unsigned gen = 0;
    ensure_defaults();
    pthread_mutex_lock(&web_lock);
    WebName *w = find_name(name, 0);
    if (!w || w->nslots <= 0) {
        pthread_mutex_unlock(&web_lock);
        int imported = user_sounds_acquire(name,index);
        if (imported != -2) return imported;
        return acquire_default(name, index, midi);
    }
    int pick = select_slot(w,index,midi);
    if (!w->slots[pick].ready || w->slots[pick].failed || !w->slots[pick].url[0]) {
        pthread_mutex_unlock(&web_lock); return -1;
    }
    if (w->slots[pick].local[0]) {
        char local[512];
        snprintf(local, sizeof(local), "%s", w->slots[pick].local);
        int bm = w->slots[pick].base_midi;
        pthread_mutex_unlock(&web_lock);
        return smp_acquire_file(local, bm);
    }
    snprintf(url, sizeof(url), "%s", w->slots[pick].url);
    base_midi = w->slots[pick].base_midi;
    gen = web_gen;
    char key[64];
    snprintf(key, sizeof(key), "%s", w->name);
    pthread_mutex_unlock(&web_lock);

    char path[512];
    if (!download_audio(url, path, (int)sizeof(path))) {
        pthread_mutex_lock(&web_lock);
        if (gen == web_gen) {
            WebName *nw = find_name(key, 0);
            if (nw) {
                for (int i = 0; i < nw->nslots; i++) {
                    if (strcmp(nw->slots[i].url, url) == 0) nw->slots[i].failed = 1;
                }
            }
        }
        pthread_mutex_unlock(&web_lock);
        log_console("samples: fetch failed %.80s", url);
        return -1;
    }
    pthread_mutex_lock(&web_lock);
    if (gen == web_gen) {
        WebName *nw = find_name(key, 0);
        if (nw) {
            for (int i = 0; i < nw->nslots; i++) {
                if (strcmp(nw->slots[i].url, url) == 0 && !nw->slots[i].local[0])
                    snprintf(nw->slots[i].local, sizeof(nw->slots[i].local), "%s", path);
            }
        }
    }
    pthread_mutex_unlock(&web_lock);
    log_console("samples: %s", key);
    return smp_acquire_file(path, base_midi);
}

int websamp_name_count(void) {
    pthread_mutex_lock(&web_lock);
    int n = web_n;
    pthread_mutex_unlock(&web_lock);
    return n;
}

int websamp_slot_count(const char *name) {
    pthread_mutex_lock(&web_lock);
    WebName *w = find_name(name, 0);
    int n = w ? w->nslots : 0;
    pthread_mutex_unlock(&web_lock);
    return n;
}

int websamp_slot_url(const char *name, int index, char *out, int outlen) {
    if (!out || outlen <= 0) return 0;
    out[0] = 0;
    pthread_mutex_lock(&web_lock);
    WebName *w = find_name(name, 0);
    int ok = w && index >= 0 && index < w->nslots && w->slots[index].ready;
    if (ok) snprintf(out, (size_t)outlen, "%s", w->slots[index].url);
    pthread_mutex_unlock(&web_lock);
    return ok;
}

int websamp_slot_midi(const char *name, int index) {
    pthread_mutex_lock(&web_lock);
    WebName *w = find_name(name, 0);
    int m = (w && index >= 0 && index < w->nslots) ? w->slots[index].base_midi : -1;
    pthread_mutex_unlock(&web_lock);
    return m;
}

int websamp_default_url(const char *name, int index, char *out, int outlen) {
    const char *src;
    DefName *d;
    if (!out || outlen <= 0) return 0;
    out[0] = 0;
    ensure_defaults();
    pthread_mutex_lock(&web_lock);
    d = def_find(name, 0);
    if (!d || index < 0 || index >= d->nslots) {
        pthread_mutex_unlock(&web_lock);
        return 0;
    }
    src = d->slots[index].url[0] ? d->slots[index].url : d->slots[index].local;
    if (!src || !src[0]) {
        pthread_mutex_unlock(&web_lock);
        return 0;
    }
    snprintf(out, (size_t)outlen, "%s", src);
    pthread_mutex_unlock(&web_lock);
    return out[0] != 0;
}

unsigned websamp_revision(void) {
    ensure_defaults();
    pthread_mutex_lock(&web_lock);
    unsigned result = web_gen*65537u+(unsigned)web_n*1031u;
    for (int i = 0; i < web_n; i++) result += (unsigned)web_names[i].nslots;
    pthread_mutex_unlock(&web_lock); return result;
}

int websamp_list_sounds(SoundInfo *out, int capacity) {
    ensure_defaults();
    pthread_mutex_lock(&web_lock);
    int total = 0;
    for (int source = 0; source < 2; source++) {
        WebName *table = source ? web_names : def_names;
        int count = source ? web_n : def_n;
        for (int i = 0; i < count; i++) {
            if (!table[i].nslots) continue;
            WebName *override = !source ? find_name(table[i].name,0) : NULL;
            if (override && override->nslots) continue;
            if (out && total < capacity) {
                snprintf(out[total].name,sizeof(out[total].name),"%.63s",table[i].name);
                out[total].slots = table[i].nslots;
                out[total].category = source ? SOUND_USER : table[i].machine ? SOUND_MACHINES : SOUND_SAMPLES;
                out[total].pitched = table[i].slots[0].pitched;
                if (!strncmp(table[i].name,"wt_",3)) { out[total].category=SOUND_WAVETABLES; out[total].pitched=1; }
            }
            total++;
        }
    }
    pthread_mutex_unlock(&web_lock); return total;
}
