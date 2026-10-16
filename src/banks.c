#include "banks.h"
#include "common.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>
#include <unistd.h>
#include <leveldb/c.h>

#define BANKS_MAX_ENTRIES 150000
#define BANKS_PATH_LEN 512
#define BANKS_MAX_DIRS 200000
#define LDB_VERSION "2" /* bump when playable set or schema changes */

static char **entries = NULL;
static int n_entries = 0;
static int cap_entries = 0;
static int scan_done = 0;
static int scan_failed = 0;
static pthread_mutex_t banks_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t banks_tid;
static int banks_started = 0;
static char banks_root[1024] = {0};

static int playable(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    return strcasecmp(dot, ".wav") == 0 || strcasecmp(dot, ".sf2") == 0 ||
           strcasecmp(dot, ".sf3") == 0 || strcasecmp(dot, ".sfz") == 0 ||
           strcasecmp(dot, ".mp3") == 0 || strcasecmp(dot, ".flac") == 0 ||
           strcasecmp(dot, ".fla") == 0;
}

static int rel_safe(const char *rel) {
    if (!rel || !*rel || rel[0] == '/') return 0;
    if (strstr(rel, "..")) return 0;
    if (strlen(rel) >= BANKS_PATH_LEN) return 0;
    return 1;
}

/* per-dir records collected during scan for the LevelDB store */
typedef struct { char *rel; long sec; long nsec; int start; int count; } DirRec;
static DirRec *scanrecs = NULL;
static int n_scanrecs = 0;
static int cap_scanrecs = 0;

static void scanrec_add(const char *rel, long sec, long nsec, int start) {
    if (n_scanrecs >= BANKS_MAX_DIRS) return;
    if (n_scanrecs == cap_scanrecs) {
        int nc = cap_scanrecs ? cap_scanrecs * 2 : 1024;
        if (nc > BANKS_MAX_DIRS) nc = BANKS_MAX_DIRS;
        DirRec *np = realloc(scanrecs, (size_t)nc * sizeof(*np));
        if (!np) return;
        scanrecs = np;
        cap_scanrecs = nc;
    }
    char *d = malloc(strlen(rel) + 1);
    if (!d) return;
    memcpy(d, rel, strlen(rel) + 1);
    scanrecs[n_scanrecs].rel = d;
    scanrecs[n_scanrecs].sec = sec;
    scanrecs[n_scanrecs].nsec = nsec;
    scanrecs[n_scanrecs].start = start;
    scanrecs[n_scanrecs].count = 0;
    n_scanrecs++;
}

static void scanrecs_free(void) {
    for (int i = 0; i < n_scanrecs; i++) free(scanrecs[i].rel);
    free(scanrecs);
    scanrecs = NULL;
    n_scanrecs = 0;
    cap_scanrecs = 0;
}

/* LevelDB persistence (defined below) */
static void ldb_path(char *out, size_t outlen);
static leveldb_t *ldb_open(const char *path);
static void ldb_store(leveldb_t *db, const char *root);

static void add_entry(const char *rel) {
    if (n_entries >= BANKS_MAX_ENTRIES) return;
    if (strlen(rel) >= BANKS_PATH_LEN) return;
    if (n_entries == cap_entries) {
        int nc = cap_entries ? cap_entries * 2 : 4096;
        if (nc > BANKS_MAX_ENTRIES) nc = BANKS_MAX_ENTRIES;
        char **np = realloc(entries, (size_t)nc * sizeof(*np));
        if (!np) return;
        entries = np;
        cap_entries = nc;
    }
    char *d = malloc(strlen(rel) + 1);
    if (!d) return;
    memcpy(d, rel, strlen(rel) + 1);
    entries[n_entries++] = d;
}

/* iterative walk to avoid deep recursion; cap depth at 32 */
static void scan(void) {
    /* dynamic FIFO queue of dirs (cap total visited) */
    char **queue = NULL;
    int qhead = 0, qtail = 0, qcap = 0, visited = 0;
    int ndirs = 0;
    (void)ndirs;
    /* seed queue with root */
    {
        char *r = malloc(strlen(banks_root) + 1);
        if (!r) return;
        memcpy(r, banks_root, strlen(banks_root) + 1);
        qcap = 64;
        queue = malloc(sizeof(*queue) * (size_t)qcap);
        if (!queue) { free(r); return; }
        queue[qtail++] = r;
    }
    while (qhead < qtail) {
        if (visited++ > 200000) break;
        char *dir = queue[qhead++];
        DIR *dp = opendir(dir);
        if (!dp) { free(dir); continue; }
        /* record dir for LevelDB store */
        struct stat dst;
        int recidx = -1;
        if (stat(dir, &dst) == 0 && S_ISDIR(dst.st_mode)) {
            const char *rrel = dir + strlen(banks_root);
            while (*rrel == '/') rrel++;
            if (strlen(rrel) < BANKS_PATH_LEN && !strstr(rrel, "..")) {
                pthread_mutex_lock(&banks_lock);
                int s0 = n_entries;
                pthread_mutex_unlock(&banks_lock);
                scanrec_add(rrel, (long)dst.st_mtim.tv_sec, (long)dst.st_mtim.tv_nsec, s0);
                recidx = n_scanrecs - 1;
            }
        }
        struct dirent *de;
        while ((de = readdir(dp)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            if (strcmp(de->d_name, ".git") == 0) continue;
            char full[2048];
            snprintf(full, sizeof(full), "%.1023s/%.255s", dir, de->d_name);
            struct stat st;
            if (lstat(full, &st) != 0) continue;
            if (S_ISLNK(st.st_mode)) continue; /* no symlink escapes */
            if (S_ISDIR(st.st_mode)) {
                /* depth guard: count slashes beyond root */
                int extra = 0;
                for (const char *p = full + strlen(banks_root); *p; p++)
                    if (*p == '/') extra++;
                if (extra < 32) {
                    if (qtail == qcap) {
                        int nc = qcap * 2;
                        if (nc > 300000) continue;
                        char **nq = realloc(queue, sizeof(*queue) * (size_t)nc);
                        if (!nq) continue;
                        queue = nq;
                        qcap = nc;
                    }
                    char *d = malloc(strlen(full) + 1);
                    if (!d) continue;
                    memcpy(d, full, strlen(full) + 1);
                    queue[qtail++] = d;
                }
                continue;
            }
            if (!S_ISREG(st.st_mode)) continue;
            if (!playable(de->d_name)) continue;
            /* relative path from banks root */
            const char *rel = full + strlen(banks_root);
            while (*rel == '/') rel++;
            pthread_mutex_lock(&banks_lock);
            add_entry(rel);
            pthread_mutex_unlock(&banks_lock);
        }
        closedir(dp);
        if (recidx >= 0) {
            pthread_mutex_lock(&banks_lock);
            int s1 = n_entries;
            pthread_mutex_unlock(&banks_lock);
            scanrecs[recidx].count = s1 - scanrecs[recidx].start;
            if (scanrecs[recidx].count < 0) scanrecs[recidx].count = 0;
        }
        free(dir);
    }
    for (int i = qhead; i < qtail; i++) free(queue[i]);
    free(queue);
}

static void *banks_worker(void *arg) {
    (void)arg;
    scan();
    char dbpath[2048];
    ldb_path(dbpath, sizeof(dbpath));
    if (dbpath[0]) {
        leveldb_t *db = ldb_open(dbpath);
        if (db) {
            ldb_store(db, banks_root);
            leveldb_close(db);
        }
    }
    scanrecs_free();
    pthread_mutex_lock(&banks_lock);
    scan_done = 1;
    pthread_mutex_unlock(&banks_lock);
    return NULL;
}

static int contains_ci(const char *hay, const char *needle) {
    if (!*needle) return 1;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i;
        for (i = 0; i < nl && p[i]; i++)
            if (tolower((unsigned char)p[i]) != tolower((unsigned char)needle[i])) break;
        if (i == nl) return 1;
    }
    return 0;
}

static void start_worker_locked(void) {
    /* banks_lock NOT held on entry */
    pthread_t tid;
    if (pthread_create(&tid, NULL, banks_worker, NULL) == 0) {
        banks_tid = tid;
        pthread_detach(tid);
    } else {
        pthread_mutex_lock(&banks_lock);
        scan_failed = 1;
        scan_done = 1;
        pthread_mutex_unlock(&banks_lock);
    }
}

void banks_start_index(const char *root) {
    pthread_mutex_lock(&banks_lock);
    if (banks_started) { pthread_mutex_unlock(&banks_lock); return; }
    banks_started = 1;
    if (root) snprintf(banks_root, sizeof(banks_root), "%s", root);
    struct stat st;
    if (!root || stat(banks_root, &st) != 0 || !S_ISDIR(st.st_mode)) {
        scan_failed = 1;
        scan_done = 1;
        pthread_mutex_unlock(&banks_lock);
        return;
    }
    pthread_mutex_unlock(&banks_lock);
    start_worker_locked();
}

/* re-scan from scratch (picks up added/removed banks); 1 if started */
int banks_rescan(void) {
    pthread_mutex_lock(&banks_lock);
    if (!scan_done) { pthread_mutex_unlock(&banks_lock); return 0; }
    for (int i = 0; i < n_entries; i++) free(entries[i]);
    n_entries = 0;
    scan_done = 0;
    scan_failed = 0;
    pthread_mutex_unlock(&banks_lock);
    start_worker_locked();
    return 1;
}

int banks_ready(void) {
    int d;
    pthread_mutex_lock(&banks_lock);
    d = scan_done;
    pthread_mutex_unlock(&banks_lock);
    return d;
}

int banks_count(void) {
    int n;
    pthread_mutex_lock(&banks_lock);
    n = n_entries;
    pthread_mutex_unlock(&banks_lock);
    return n;
}

int banks_failed(void) {
    int f;
    pthread_mutex_lock(&banks_lock);
    f = scan_failed;
    pthread_mutex_unlock(&banks_lock);
    return f;
}

int banks_search(const char *query, const char **out, int max) {
    if (!query) query = "";
    if (max < 0) max = 0;
    int total = 0;
    pthread_mutex_lock(&banks_lock);
    for (int i = 0; i < n_entries; i++) {
        if (!contains_ci(entries[i], query)) continue;
        if (total < max && out) out[total] = entries[i];
        total++;
    }
    pthread_mutex_unlock(&banks_lock);
    return total;
}

int banks_search_dirs(const char *query, char dirs[][512], int *counts, int max) {
    if (!query) query = "";
    if (max < 0) max = 0;
    /* distinct matching dirs with subtree file counts.
       A dir matches when it (or anything under it) contains the query;
       children with a matching parent are collapsed away. */
    char (*tmp)[512] = malloc(4096 * 512);
    int *cnt = malloc(4096 * sizeof(int));
    if (!tmp || !cnt) { free(tmp); free(cnt); return 0; }
    int ntmp = 0;
    pthread_mutex_lock(&banks_lock);
    for (int i = 0; i < n_entries; i++) {
        const char *slash = strrchr(entries[i], '/');
        size_t dl = slash ? (size_t)(slash - entries[i]) : 0;
        if (dl == 0 || dl >= 512) continue;
        char dir[512];
        memcpy(dir, entries[i], dl);
        dir[dl] = 0;
        /* walk ancestors (dir itself first, up to 8 levels) */
        char up[512];
        memcpy(up, dir, dl + 1);
        for (int lvl = 0; lvl < 8; lvl++) {
            if (up[0] && contains_ci(up, query)) {
                int found = -1;
                for (int k = 0; k < ntmp; k++) {
                    if (strcmp(tmp[k], up) == 0) { found = k; break; }
                }
                if (found >= 0) cnt[found]++;
                else if (ntmp < 4096) {
                    memcpy(tmp[ntmp], up, strlen(up) + 1);
                    cnt[ntmp] = 1;
                    ntmp++;
                }
            }
            char *s = strrchr(up, '/');
            if (!s) break;
            *s = 0;
            if (!up[0]) break;
        }
    }
    /* collapse: drop dirs with a proper-prefix ancestor in the set */
    char keep[4096];
    for (int i = 0; i < ntmp; i++) keep[i] = 1;
    for (int i = 0; i < ntmp; i++) {
        if (!keep[i]) continue;
        size_t li = strlen(tmp[i]);
        for (int j = 0; j < ntmp; j++) {
            if (i == j || !keep[j]) continue;
            if (strncmp(tmp[j], tmp[i], li) == 0 && tmp[j][li] == '/') {
                keep[j] = 0; /* child of i */
            }
        }
    }
    int total = 0;
    for (int i = 0; i < ntmp; i++) {
        if (!keep[i]) continue;
        if (total < max) {
            memcpy(dirs[total], tmp[i], strlen(tmp[i]) + 1);
            if (counts) counts[total] = cnt[i];
        }
        total++;
    }
    pthread_mutex_unlock(&banks_lock);
    free(tmp);
    free(cnt);
    return total;
}

static int cmp_path(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

int banks_list_dir(const char *reldir, const char *query, char out[][512], int max) {
    if (!reldir) reldir = "";
    if (!query) query = "";
    if (max <= 0) return 0;
    if (strstr(reldir, "..") || reldir[0] == '/') return -1;
    if (strlen(reldir) >= 900) return -1;
    pthread_mutex_lock(&banks_lock);
    char full[2048];
    if (reldir[0]) snprintf(full, sizeof(full), "%.1023s/%.1023s", banks_root, reldir);
    else snprintf(full, sizeof(full), "%s", banks_root);
    DIR *dp = opendir(full);
    if (!dp) { pthread_mutex_unlock(&banks_lock); return -1; }
    /* collect matching subdirs (bounded), then sort */
    static char tmp[1024][512];
    int n = 0;
    struct dirent *de;
    while ((de = readdir(dp)) != NULL && n < 1024) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (strcmp(de->d_name, ".git") == 0) continue;
        char fp[2048];
        snprintf(fp, sizeof(fp), "%.1023s/%.255s", full, de->d_name);
        struct stat st;
        if (lstat(fp, &st) != 0 || S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) continue;
        if (query[0] && !contains_ci(de->d_name, query)) continue;
        if (strlen(reldir) + 1 + strlen(de->d_name) >= 512) continue;
        snprintf(tmp[n], sizeof(tmp[n]), "%s", de->d_name);
        n++;
    }
    closedir(dp);
    pthread_mutex_unlock(&banks_lock);
    qsort(tmp, (size_t)n, 512, cmp_path);
    int nout = n < max ? n : max;
    for (int i = 0; i < nout; i++) {
        /* lengths validated (<512 combined) during collect */
        size_t rl = reldir[0] ? strlen(reldir) : 0;
        size_t bl = strnlen(tmp[i], 511);
        if (rl + (reldir[0] ? 1 : 0) + bl >= 512) continue;
        if (reldir[0]) {
            memcpy(out[i], reldir, rl);
            out[i][rl] = '/';
            memcpy(out[i] + rl + 1, tmp[i], bl);
            out[i][rl + 1 + bl] = 0;
        } else {
            memcpy(out[i], tmp[i], bl);
            out[i][bl] = 0;
        }
    }
    return n;
}

/* ---------- LevelDB persistence ---------- */

static void ldb_err(char *err, const char *what) {
    if (err) {
        log_console("banks db %s: %.160s", what, err);
        leveldb_free(err);
    }
}

static leveldb_t *ldb_open(const char *path) {
    char *err = NULL;
    leveldb_options_t *opt = leveldb_options_create();
    leveldb_options_set_create_if_missing(opt, 1);
    leveldb_t *db = leveldb_open(opt, path, &err);
    leveldb_options_destroy(opt);
    if (err) { ldb_err(err, "open"); return NULL; }
    return db;
}

/* cache dir: $XDG_CACHE_HOME/strudelterm or ~/.cache/strudelterm */
static void ldb_path(char *out, size_t outlen) {
    char base[1024];
    if (!out || outlen == 0) return;
    out[0] = 0;
    user_dir(base, sizeof(base), "XDG_CACHE_HOME", "strudelterm", ".cache/strudelterm");
    if (!base[0]) return;
    mkdir_p(base);
    int n = snprintf(out, outlen, "%s/banks.ldb", base);
    if (n < 0 || (size_t)n >= outlen) out[0] = 0;
}

/* validate all d: records against live mtimes; on full match populate
   entries and return 1 */
static int ldb_load(leveldb_t *db, const char *root) {
    char *err = NULL;
    leveldb_readoptions_t *ro = leveldb_readoptions_create();
    /* meta check */
    size_t vlen = 0;
    char *ver = leveldb_get(db, ro, "meta/version", 12, &vlen, &err);
    if (err) { ldb_err(err, "get"); leveldb_readoptions_destroy(ro); return 0; }
    int ok = (ver && vlen == strlen(LDB_VERSION) && memcmp(ver, LDB_VERSION, vlen) == 0);
    if (ver) leveldb_free(ver);
    if (!ok) { leveldb_readoptions_destroy(ro); return 0; }
    char *rt = leveldb_get(db, ro, "meta/root", 9, &vlen, &err);
    if (err) { ldb_err(err, "get"); leveldb_readoptions_destroy(ro); return 0; }
    ok = (rt && strlen(root) == vlen && memcmp(rt, root, vlen) == 0);
    if (rt) leveldb_free(rt);
    if (!ok) { leveldb_readoptions_destroy(ro); return 0; }

    /* pass 1: validate every dir mtime */
    leveldb_iterator_t *it = leveldb_create_iterator(db, ro);
    int nrec = 0, allok = 1;
    for (leveldb_iter_seek(it, "d:", 2); leveldb_iter_valid(it); leveldb_iter_next(it)) {
        size_t klen = 0;
        const char *k = leveldb_iter_key(it, &klen);
        if (klen < 2 || memcmp(k, "d:", 2) != 0) break;
        size_t vl = 0;
        const char *v = leveldb_iter_value(it, &vl);
        if (!v || vl == 0 || vl > 4 * 1024 * 1024) { allok = 0; break; }
        /* first line: sec.nsec */
        const char *nl = memchr(v, '\n', vl);
        if (!nl) { allok = 0; break; }
        char mline[64];
        size_t ml = (size_t)(nl - v);
        if (ml >= sizeof(mline)) { allok = 0; break; }
        memcpy(mline, v, ml);
        mline[ml] = 0;
        long sec = atol(mline);
        long nsec = 0;
        const char *dot = strchr(mline, '.');
        if (dot) nsec = atol(dot + 1);
        /* reldir = key minus "d:" */
        char rel[1024];
        size_t rl = klen - 2;
        if (rl >= sizeof(rel)) { allok = 0; break; }
        memcpy(rel, k + 2, rl);
        rel[rl] = 0;
        if (strstr(rel, "..")) { allok = 0; break; }
        char full[2048];
        if (rel[0]) snprintf(full, sizeof(full), "%.1023s/%.1023s", root, rel);
        else snprintf(full, sizeof(full), "%s", root);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode) ||
            (long)st.st_mtim.tv_sec != sec || (long)st.st_mtim.tv_nsec != nsec) {
            allok = 0;
            break;
        }
        nrec++;
        if (nrec > BANKS_MAX_DIRS) { allok = 0; break; }
    }
    if (!allok || nrec == 0) {
        leveldb_iter_destroy(it);
        leveldb_readoptions_destroy(ro);
        return 0;
    }
    /* pass 2: populate entries */
    int added = 0;
    for (leveldb_iter_seek(it, "d:", 2); leveldb_iter_valid(it); leveldb_iter_next(it)) {
        size_t klen = 0;
        const char *k = leveldb_iter_key(it, &klen);
        if (klen < 2 || memcmp(k, "d:", 2) != 0) break;
        size_t vl = 0;
        const char *v = leveldb_iter_value(it, &vl);
        if (!v || vl == 0) continue;
        const char *nl = memchr(v, '\n', vl);
        if (!nl) continue;
        const char *p = nl + 1;
        const char *end = v + vl;
        char line[1024];
        while (p < end) {
            const char *e = memchr(p, '\n', (size_t)(end - p));
            if (!e) e = end;
            size_t ll = (size_t)(e - p);
            if (ll > 0 && ll < sizeof(line)) {
                memcpy(line, p, ll);
                line[ll] = 0;
                if (rel_safe(line)) {
                    pthread_mutex_lock(&banks_lock);
                    add_entry(line);
                    pthread_mutex_unlock(&banks_lock);
                    added++;
                    if (added >= BANKS_MAX_ENTRIES) break;
                }
            }
            p = e + 1;
        }
        if (added >= BANKS_MAX_ENTRIES) break;
    }
    leveldb_iter_destroy(it);
    leveldb_readoptions_destroy(ro);
    return added > 0;
}

static void ldb_store(leveldb_t *db, const char *root) {
    char *err = NULL;
    leveldb_readoptions_t *ro = leveldb_readoptions_create();
    leveldb_writeoptions_t *wo = leveldb_writeoptions_create();
    /* snapshot existing d: keys so stale dirs can be deleted */
    char (*oldkeys)[1024] = NULL;
    int nold = 0, capold = 0;
    leveldb_iterator_t *it = leveldb_create_iterator(db, ro);
    for (leveldb_iter_seek(it, "d:", 2); leveldb_iter_valid(it); leveldb_iter_next(it)) {
        size_t klen = 0;
        const char *k = leveldb_iter_key(it, &klen);
        if (klen < 2 || memcmp(k, "d:", 2) != 0 || klen >= 1024) break;
        if (nold == capold) {
            int nc = capold ? capold * 2 : 1024;
            char (*nq)[1024] = realloc(oldkeys, (size_t)nc * 1024);
            if (!nq) break;
            oldkeys = nq;
            capold = nc;
        }
        memcpy(oldkeys[nold], k, klen);
        oldkeys[nold][klen] = 0;
        nold++;
    }
    leveldb_iter_destroy(it);
    /* meta */
    leveldb_put(db, wo, "meta/version", 12, LDB_VERSION, strlen(LDB_VERSION), &err);
    if (err) { ldb_err(err, "put"); err = NULL; }
    leveldb_put(db, wo, "meta/root", 9, root, strlen(root), &err);
    if (err) { ldb_err(err, "put"); err = NULL; }
    /* records */
    pthread_mutex_lock(&banks_lock);
    for (int i = 0; i < n_scanrecs; i++) {
        char key[1024];
        snprintf(key, sizeof(key), "d:%s", scanrecs[i].rel);
        /* value: "sec.nsec\n" + file rels */
        size_t vcap = 64, vlen = 0;
        char *val = malloc(vcap);
        if (!val) continue;
        int w = snprintf(val, vcap, "%ld.%ld\n", scanrecs[i].sec, scanrecs[i].nsec);
        if (w < 0) { free(val); continue; }
        vlen = (size_t)w;
        for (int e = 0; e < scanrecs[i].count; e++) {
            int idx = scanrecs[i].start + e;
            if (idx < 0 || idx >= n_entries) break;
            size_t el = strlen(entries[idx]);
            while (vlen + el + 2 > vcap) {
                vcap *= 2;
                if (vcap > 64 * 1024 * 1024) break;
                char *nv = realloc(val, vcap);
                if (!nv) break;
                val = nv;
            }
            if (vlen + el + 2 > vcap) break;
            memcpy(val + vlen, entries[idx], el);
            vlen += el;
            val[vlen++] = '\n';
        }
        leveldb_put(db, wo, key, strlen(key), val, vlen, &err);
        if (err) { ldb_err(err, "put"); err = NULL; }
        free(val);
        /* not stale */
        for (int o = 0; o < nold; o++) {
            if (strcmp(oldkeys[o], key) == 0) { oldkeys[o][0] = 0; break; }
        }
    }
    pthread_mutex_unlock(&banks_lock);
    /* delete stale */
    for (int o = 0; o < nold; o++) {
        if (!oldkeys[o][0]) continue;
        leveldb_delete(db, wo, oldkeys[o], strlen(oldkeys[o]), &err);
        if (err) { ldb_err(err, "delete"); err = NULL; }
    }
    free(oldkeys);
    leveldb_readoptions_destroy(ro);
    leveldb_writeoptions_destroy(wo);
}

/* resolve banks root into out; 1 if a usable dir was found */
static int resolve_root(char *out, size_t outlen) {
    struct stat st;
    const char *e = getenv("STRUDELTERM_BANKS");
    if (e && strlen(e) < 900) {
        snprintf(out, outlen, "%s", e);
        out[outlen - 1] = 0;
        if (stat(out, &st) == 0 && S_ISDIR(st.st_mode)) return 1;
    }
    snprintf(out, outlen, "%s", "./assets/banks");
    out[outlen - 1] = 0;
    if (stat(out, &st) == 0 && S_ISDIR(st.st_mode)) return 1;
    /* binary lives in the repo root when installed beside assets/ */
    char exe[1024];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0 && (size_t)n < sizeof(exe)) {
        exe[n] = 0;
        char *slash = strrchr(exe, '/');
        if (slash) {
            *slash = 0;
            int w = snprintf(out, outlen, "%s/assets/banks", exe);
            if (w > 0 && (size_t)w < outlen && stat(out, &st) == 0 && S_ISDIR(st.st_mode))
                return 1;
        }
    }
    return 0;
}

void banks_ensure_default(void) {
    static int started = 0;
    if (started) return;
    started = 1;
    char root[1024];
    if (!resolve_root(root, sizeof(root))) {
        banks_start_index("./assets/banks"); /* reports failure */
        return;
    }
    /* fast path: LevelDB snapshot */
    char dbpath[2048];
    ldb_path(dbpath, sizeof(dbpath));
    if (dbpath[0]) {
        leveldb_t *db = ldb_open(dbpath);
        if (db) {
            int hit = ldb_load(db, root);
            leveldb_close(db);
            if (hit) {
                pthread_mutex_lock(&banks_lock);
                if (!banks_started) {
                    snprintf(banks_root, sizeof(banks_root), "%s", root);
                    banks_started = 1;
                }
                scan_done = 1;
                pthread_mutex_unlock(&banks_lock);
                log_console("banks index loaded (%d files)", banks_count());
                return;
            }
        }
    }
    banks_start_index(root);
}

const char *banks_root_dir(void) {
    return banks_root;
}
