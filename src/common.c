#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "common.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <ctype.h>
#include <errno.h>

static char console_buf[MAX_CONSOLE][MAX_LINE];
static int console_head = 0;
static int console_count = 0;
static pthread_mutex_t console_lock = PTHREAD_MUTEX_INITIALIZER;

double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* sanitize terminal-hostile bytes: CR/LF/TAB/controls become spaces
   (a \r in a ring line would yank the S-Lang cursor into another pane) */
static void scrub_line(char *dst, const char *src, size_t n) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < n; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '\t') {
            if (j + 2 < n) { dst[j++] = ' '; dst[j++] = ' '; }
        } else if (c < 0x20 || c == 0x7f) {
            dst[j++] = ' ';
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = 0;
}

static int log_to_stderr = 1;

/* stderr duplicate shares the terminal with the S-Lang UI and would
   scribble over panes; the UI disables it (ring buffer still shows) */
void log_console_stderr(int on) { log_to_stderr = on ? 1 : 0; }

void log_console(const char *fmt, ...) {
    char tmp[MAX_LINE];
    char clean[MAX_LINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, MAX_LINE, fmt, ap);
    va_end(ap);
    scrub_line(clean, tmp, sizeof(clean));
    pthread_mutex_lock(&console_lock);
    snprintf(console_buf[console_head], MAX_LINE, "%s", clean);
    console_head = (console_head + 1) % MAX_CONSOLE;
    if (console_count < MAX_CONSOLE) console_count++;
    pthread_mutex_unlock(&console_lock);
    if (log_to_stderr) fprintf(stderr, "%s\n", clean);
}

char *trim(char *s) {
    if (!s) return s;
    char *start = s;
    while (isspace((unsigned char)*start)) start++;
    if (*start == 0) { s[0] = 0; return s; }
    char *end = start + strlen(start) - 1;
    while (end > start && isspace((unsigned char)*end)) end--;
    *(end+1) = 0;
    if (start != s) memmove(s, start, strlen(start)+1);
    return s;
}

char *strdup_safe(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

int console_count_get(void) {
    pthread_mutex_lock(&console_lock);
    int n = console_count;
    pthread_mutex_unlock(&console_lock);
    return n;
}
int console_head_get(void) {
    pthread_mutex_lock(&console_lock);
    int h = console_head;
    pthread_mutex_unlock(&console_lock);
    return h;
}
const char *console_line(int idx) {
    const char *s = "";
    pthread_mutex_lock(&console_lock);
    if (idx >= 0 && idx < console_count) {
        int start = (console_count < MAX_CONSOLE) ? 0 : console_head;
        int pos = (start + idx) % MAX_CONSOLE;
        s = console_buf[pos];
    }
    pthread_mutex_unlock(&console_lock);
    return s;
}

/* absolute, no controls, no ".." segment */
static int env_base_ok(const char *s) {
    if (!s || s[0] != '/') return 0;
    size_t n = strlen(s);
    if (n == 0 || n >= 800) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7f) return 0;
    }
    for (const char *p = s; *p; ) {
        if (*p == '/') { p++; continue; }
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0')) return 0;
        while (*p && *p != '/') p++;
    }
    return 1;
}

void mkdir_p(const char *path) {
    char tmp[1024];
    if (!path || !path[0] || strlen(path) >= sizeof(tmp)) return;
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

void user_dir(char *out, size_t outlen, const char *xdg_env,
              const char *xdg_leaf, const char *home_leaf) {
    if (!out || outlen == 0) return;
    out[0] = 0;
    if (!xdg_leaf) xdg_leaf = "";
    if (!home_leaf) home_leaf = "";
    const char *base = NULL;
    const char *leaf = NULL;
    const char *x = xdg_env ? getenv(xdg_env) : NULL;
    if (env_base_ok(x)) {
        base = x;
        leaf = xdg_leaf;
    } else {
        const char *h = getenv("HOME");
        if (env_base_ok(h)) {
            base = h;
            leaf = home_leaf;
        }
    }
    if (!base) return;
    /* Migrate the legacy application root only when the new root is absent.
       Decide at the root, so config, sessions, modules and sounds agree. */
    const char *app = strstr(leaf, "tompouce");
    if (app && (app == leaf || app[-1] == '/') &&
        (app[8] == '/' || app[8] == '\0')) {
        char current[1024], legacy[1024];
        size_t prefix = (size_t)(app - leaf);
        struct stat st;
        int a = snprintf(current, sizeof(current), "%s/%.*stompouce", base, (int)prefix, leaf);
        int b = snprintf(legacy, sizeof(legacy), "%s/%.*sstrudelterm", base, (int)prefix, leaf);
        if (a > 0 && (size_t)a < sizeof(current) &&
            b > 0 && (size_t)b < sizeof(legacy) &&
            lstat(current, &st) != 0 && errno == ENOENT &&
            lstat(legacy, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (renameat2(AT_FDCWD, legacy, AT_FDCWD, current, RENAME_NOREPLACE) != 0 &&
                errno != EEXIST) {
                log_console("cannot migrate old application directory: %s", strerror(errno));
                return;
            }
        }
    }
    int n = snprintf(out, outlen, "%s/%s", base, leaf);
    if (n < 0 || (size_t)n >= outlen) out[0] = 0;
}
