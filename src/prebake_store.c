#include "prebake_store.h"
#include "common.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#define ENABLED_FILE "enabled"
#define ENABLED_MAX_BYTES (64u * 1024u)

static int prebake_name_safe(const char *name) {
    if (!name || !*name || strlen(name) >= PREBAKE_STORE_NAME) return 0;
    if (*name == '/' || strstr(name, "..")) return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (!(isalnum(*p) || *p == '/' || *p == '_' || *p == '-' || *p == '.')) return 0;
    return 1;
}

static int prebake_suffix(const char *name) {
    size_t length = strlen(name);
    return (length > 8 && !strcmp(name + length - 8, ".strudel")) ||
        (length > 3 && !strcmp(name + length - 3, ".js")) ||
        (length > 4 && !strcmp(name + length - 4, ".mjs"));
}

/* Open the owned prebakes directory component by component without following
   symlinks. `create` may add missing directories with mode 0700. */
static int prebake_dir(int create) {
    char root[1024];
    user_dir(root, sizeof(root), "XDG_CONFIG_HOME", "tompouce/prebakes",
        ".config/tompouce/prebakes");
    if (!*root) return -1;
    int dir = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    char *save = NULL;
    for (char *part = strtok_r(root, "/", &save); dir >= 0 && part;
         part = strtok_r(NULL, "/", &save)) {
        if (create && mkdirat(dir, part, 0700) && errno != EEXIST) {
            close(dir);
            return -1;
        }
        int next = openat(dir, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(dir);
        dir = next;
    }
    return dir;
}

static int enabled_lookup(const char names[][PREBAKE_STORE_NAME], int count,
    const char *name) {
    for (int i = 0; i < count; i++)
        if (!strcmp(names[i], name)) return 1;
    return 0;
}

static int read_enabled(int dir, char names[][PREBAKE_STORE_NAME], int capacity) {
    int fd = openat(dir, ENABLED_FILE, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        st.st_size > (off_t)ENABLED_MAX_BYTES) {
        close(fd);
        return -1;
    }
    char *text = malloc((size_t)st.st_size + 1);
    if (!text) { close(fd); return -1; }
    size_t used = 0;
    while (used < (size_t)st.st_size) {
        ssize_t got = read(fd, text + used, (size_t)st.st_size - used);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) { free(text); close(fd); return -1; }
        used += (size_t)got;
    }
    close(fd);
    text[used] = 0;
    int count = 0;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        while (*line == ' ' || *line == '\t' || *line == '\r') line++;
        size_t length = strlen(line);
        while (length && (line[length - 1] == ' ' || line[length - 1] == '\t' ||
                          line[length - 1] == '\r')) line[--length] = 0;
        if (!*line || *line == '#') continue;
        if (!prebake_name_safe(line)) continue;
        if (enabled_lookup((const char (*)[PREBAKE_STORE_NAME])names, count, line)) continue;
        if (count >= capacity) break;
        snprintf(names[count++], PREBAKE_STORE_NAME, "%s", line);
    }
    free(text);
    return count;
}

static int contains_ci(const char *hay, const char *needle) {
    if (!needle || !*needle) return 1;
    size_t length = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t i = 0;
        while (i < length && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == length) return 1;
    }
    return 0;
}

static int entry_compare(const void *a, const void *b) {
    return strcmp(((const PrebakeEntry *)a)->name, ((const PrebakeEntry *)b)->name);
}

int prebake_store_list(const char *query, PrebakeEntry *out, int capacity) {
    if (!out || capacity <= 0) return 0;
    int dir = prebake_dir(0);
    if (dir < 0) return errno == ENOENT ? 0 : -1;
    char enabled[PREBAKE_STORE_MAX][PREBAKE_STORE_NAME];
    int enabled_count = read_enabled(dir, enabled, PREBAKE_STORE_MAX);
    if (enabled_count < 0) { close(dir); return -1; }
    int dir_copy = dup(dir);
    if (dir_copy < 0) { close(dir); return -1; }
    DIR *dp = fdopendir(dir_copy);
    if (!dp) { close(dir_copy); close(dir); return -1; }
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dp)) != NULL) {
        const char *name = entry->d_name;
        if (name[0] == '.') continue;
        if (!strcmp(name, "prebake.strudel") || !strcmp(name, ENABLED_FILE)) continue;
        if (!prebake_suffix(name) || !prebake_name_safe(name)) continue;
        if (!contains_ci(name, query)) continue;
        struct stat st;
        if (fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode)) continue;
        if (count >= capacity) break;
        snprintf(out[count].name, sizeof(out[count].name), "%s", name);
        out[count].enabled = enabled_lookup((const char (*)[PREBAKE_STORE_NAME])enabled,
            enabled_count, name);
        count++;
    }
    closedir(dp);
    close(dir);
    qsort(out, (size_t)count, sizeof(*out), entry_compare);
    return count;
}

static int write_enabled(int dir, char names[][PREBAKE_STORE_NAME], int count) {
    uint64_t nonce = 0;
    char temporary[64];
    if (getrandom(&nonce, sizeof(nonce), 0) != (ssize_t)sizeof(nonce))
        nonce = (uint64_t)now_seconds() * 1000000u ^ (uint64_t)getpid();
    snprintf(temporary, sizeof(temporary), ".enabled-%llx", (unsigned long long)nonce);
    int fd = openat(dir, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    int ok = 1;
    for (int i = 0; i < count && ok; i++) {
        char line[PREBAKE_STORE_NAME + 2];
        int length = snprintf(line, sizeof(line), "%s\n", names[i]);
        size_t used = 0;
        while (used < (size_t)length) {
            ssize_t written = write(fd, line + used, (size_t)length - used);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { ok = 0; break; }
            used += (size_t)written;
        }
    }
    if (ok && fsync(fd)) ok = 0;
    if (close(fd)) ok = 0;
    if (ok && renameat(dir, temporary, dir, ENABLED_FILE)) ok = 0;
    if (ok && fsync(dir)) log_console("prebakes: directory sync failed");
    if (!ok) unlinkat(dir, temporary, 0);
    return ok ? 0 : -1;
}

int prebake_store_set_enabled(const char *name, int enabled) {
    if (!prebake_name_safe(name)) return -1;
    int dir = prebake_dir(1);
    if (dir < 0) return -1;
    char names[PREBAKE_STORE_MAX][PREBAKE_STORE_NAME];
    int count = read_enabled(dir, names, PREBAKE_STORE_MAX);
    if (count < 0) { close(dir); return -1; }
    int found = -1;
    for (int i = 0; i < count; i++) if (!strcmp(names[i], name)) { found = i; break; }
    if (enabled) {
        if (found >= 0) { close(dir); return 0; }
        if (count >= PREBAKE_STORE_MAX) { close(dir); return -1; }
        snprintf(names[count++], PREBAKE_STORE_NAME, "%s", name);
    } else {
        if (found < 0) { close(dir); return 0; }
        memmove(names[found], names[found + 1],
            (size_t)(count - found - 1) * sizeof(names[0]));
        count--;
    }
    int result = write_enabled(dir, names, count);
    close(dir);
    return result ? result : 1;
}

int prebake_store_enabled_source(char **source, size_t *length) {
    if (!source || !length) return -1;
    *source = NULL;
    *length = 0;
    int dir = prebake_dir(0);
    if (dir < 0) return errno == ENOENT ? 0 : -1;
    char names[PREBAKE_STORE_MAX][PREBAKE_STORE_NAME];
    int count = read_enabled(dir, names, PREBAKE_STORE_MAX);
    close(dir);
    if (count <= 0) return count < 0 ? -1 : 0;
    size_t capacity = 0;
    for (int i = 0; i < count; i++) capacity += strlen(names[i]) + 32;
    char *text = malloc(capacity);
    if (!text) return -1;
    size_t used = 0;
    for (int i = 0; i < count; i++) {
        int written = snprintf(text + used, capacity - used,
            "await loadPrebake('%s');\n", names[i]);
        if (written < 0 || (size_t)written >= capacity - used) { free(text); return -1; }
        used += (size_t)written;
    }
    *source = text;
    *length = used;
    return 1;
}
