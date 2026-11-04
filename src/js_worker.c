#define _GNU_SOURCE
#include "js_worker.h"
#include "common.h"
#include <cjson/cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* The committed bundle is embedded for the QuickJS host; reuse it rather than
   reading js/dist at runtime, so the worker executes exactly what ships. */
extern unsigned char strudel_js_bundle[];
extern unsigned int strudel_js_bundle_len;

#define WORKER_FRAME_MAX (8u * 1024u * 1024u)
#define WORKER_COMPILE_BUDGET_MS 5000
#define WORKER_QUERY_BUDGET_MS 3000
#define WORKER_SOURCE_MAX (4u * 1024u * 1024u)

struct JsWorker {
    pid_t pid;
    int in_fd, out_fd;
    unsigned long long next_id;
    int failure;
    char version[32];
    char *line;
    size_t line_cap;
};

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void set_error(char *error, int len, const char *text) {
    if (!error || len <= 0) return;
    snprintf(error, (size_t)len, "%s", text && text[0] ? text : "worker failure");
}

/* Append a JSON string literal, escaping only what JSON requires. The bundle is
   UTF-8 text without control characters in practice, but escape defensively and
   bound the output. */
static int json_escape(char *out, size_t cap, size_t *used, const char *text, size_t len) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (*used + 8 >= cap) return 0;
        switch (c) {
        case '"': out[(*used)++] = '\\'; out[(*used)++] = '"'; break;
        case '\\': out[(*used)++] = '\\'; out[(*used)++] = '\\'; break;
        case '\n': out[(*used)++] = '\\'; out[(*used)++] = 'n'; break;
        case '\r': out[(*used)++] = '\\'; out[(*used)++] = 'r'; break;
        case '\t': out[(*used)++] = '\\'; out[(*used)++] = 't'; break;
        default:
            if (c < 0x20) {
                out[(*used)++] = '\\'; out[(*used)++] = 'u'; out[(*used)++] = '0'; out[(*used)++] = '0';
                out[(*used)++] = hex[c >> 4]; out[(*used)++] = hex[c & 15];
            } else out[(*used)++] = (char)c;
        }
    }
    return 1;
}

/* A worker can die between our poll and write. Block SIGPIPE around the write
   and drain the generated signal so a dead child is reported as a failure
   instead of terminating the engine. */
static int write_all(int fd, const char *data, size_t len) {
    sigset_t block, old, pending;
    sigemptyset(&block); sigaddset(&block, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &block, &old);
    int ok = 1;
    size_t used = 0;
    while (used < len) {
        ssize_t wrote = write(fd, data + used, len - used);
        if (wrote < 0) {
            if (errno == EINTR) continue;
            ok = 0;
            break;
        }
        if (wrote == 0) { ok = 0; break; }
        used += (size_t)wrote;
    }
    if (!ok) {
        struct timespec zero = {0, 0};
        while (sigtimedwait(&block, NULL, &zero) == SIGPIPE) {}
    } else if (sigpending(&pending) == 0 && sigismember(&pending, SIGPIPE) == 1) {
        struct timespec zero = {0, 0};
        while (sigtimedwait(&block, NULL, &zero) == SIGPIPE) {}
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    return ok;
}

/* Read one newline-terminated frame with a deadline. Returns 1 on success,
   0 on timeout (worker killed) and -1 when the process or protocol failed. */
static int read_frame(JsWorker *worker, long budget_ms, char *error, int errorlen) {
    if (!worker->line) {
        worker->line_cap = 65536;
        worker->line = malloc(worker->line_cap);
        if (!worker->line) { set_error(error, errorlen, "out of memory"); return -1; }
    }
    size_t used = 0;
    long long deadline = now_ms() + budget_ms;
    for (;;) {
        long long remaining = deadline - now_ms();
        if (remaining <= 0) {
            worker->failure = JS_WORKER_TIMEOUT;
            kill(worker->pid, SIGKILL);
            set_error(error, errorlen, "worker request timed out");
            return 0;
        }
        struct pollfd pfd = {.fd = worker->out_fd, .events = POLLIN};
        int ready = poll(&pfd, 1, (int)(remaining > 1000 ? 1000 : remaining));
        if (ready < 0) {
            if (errno == EINTR) continue;
            worker->failure = JS_WORKER_DEAD;
            set_error(error, errorlen, "worker poll failed");
            return -1;
        }
        if (ready == 0) continue;
        if (used + 4096 > worker->line_cap) {
            if (worker->line_cap >= WORKER_FRAME_MAX) {
                worker->failure = JS_WORKER_PROTOCOL;
                kill(worker->pid, SIGKILL);
                set_error(error, errorlen, "worker frame exceeds the limit");
                return -1;
            }
            size_t next = worker->line_cap * 2;
            if (next > WORKER_FRAME_MAX) next = WORKER_FRAME_MAX;
            char *grown = realloc(worker->line, next);
            if (!grown) { set_error(error, errorlen, "out of memory"); return -1; }
            worker->line = grown; worker->line_cap = next;
        }
        ssize_t got = read(worker->out_fd, worker->line + used, worker->line_cap - used - 1);
        if (got < 0) {
            if (errno == EINTR) continue;
            worker->failure = JS_WORKER_DEAD;
            set_error(error, errorlen, "worker read failed");
            return -1;
        }
        if (got == 0) {
            worker->failure = JS_WORKER_DEAD;
            set_error(error, errorlen, "worker exited");
            return -1;
        }
        size_t scan = used;
        used += (size_t)got;
        worker->line[used] = 0;
        for (size_t i = scan; i < used; i++) if (worker->line[i] == '\n') {
            worker->line[i] = 0;
            return 1;
        }
    }
}

/* Send one bounded request frame and parse the matching response. A stale or
   malformed response desynchronizes the stream, so the worker is killed rather
   than reused. */
static cJSON *request(JsWorker *worker, unsigned long long id, const char *frame,
    long budget_ms, char *error, int errorlen) {
    if (!worker || worker->pid <= 0) { set_error(error, errorlen, "worker not running"); return NULL; }
    if (strlen(frame) + 2 > WORKER_FRAME_MAX) {
        set_error(error, errorlen, "worker request exceeds the frame limit");
        return NULL;
    }
    if (!write_all(worker->in_fd, frame, strlen(frame)) ||
        !write_all(worker->in_fd, "\n", 1)) {
        worker->failure = JS_WORKER_DEAD;
        set_error(error, errorlen, "worker write failed");
        return NULL;
    }
    if (read_frame(worker, budget_ms, error, errorlen) != 1) return NULL;
    cJSON *response = cJSON_Parse(worker->line);
    if (!response) {
        if (getenv("TOMPOUCE_WORKER_DEBUG"))
            fprintf(stderr, "DBG worker line (%zu bytes): %.200s\n", strlen(worker->line), worker->line);
        worker->failure = JS_WORKER_PROTOCOL;
        kill(worker->pid, SIGKILL);
        set_error(error, errorlen, "malformed worker response"); return NULL;
    }
    const cJSON *response_id = cJSON_GetObjectItemCaseSensitive(response, "id");
    if (!cJSON_IsNumber(response_id) || response_id->valuedouble != (double)id) {
        worker->failure = JS_WORKER_PROTOCOL;
        kill(worker->pid, SIGKILL);
        cJSON_Delete(response);
        set_error(error, errorlen, "worker response id mismatch");
        return NULL;
    }
    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(response, "ok");
    if (!cJSON_IsTrue(ok)) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(response, "error");
        set_error(error, errorlen,
            cJSON_IsString(message) ? message->valuestring : "worker rejected the request");
        cJSON_Delete(response);
        return NULL;
    }
    return response;
}

static char *copy_string_field(const cJSON *response, const char *name) {
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(response, name);
    if (!cJSON_IsString(field) || !field->valuestring) return NULL;
    size_t len = strlen(field->valuestring);
    char *copy = malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, field->valuestring, len + 1);
    return copy;
}

static char *default_script_path(void) {
    const char *override = getenv("TOMPOUCE_QUERY_WORKER");
    if (override && override[0]) {
        char *copy = strdup(override);
        return copy;
    }
    const char *relative = "scripts/query-worker.mjs";
    if (access(relative, R_OK) == 0) return strdup(relative);
    char exe[1024];
    ssize_t got = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (got <= 0) return NULL;
    exe[got] = 0;
    char *slash = strrchr(exe, '/');
    if (!slash) return NULL;
    *slash = 0;
    char candidate[1200];
    snprintf(candidate, sizeof(candidate), "%s/scripts/query-worker.mjs", exe);
    if (access(candidate, R_OK) == 0) return strdup(candidate);
    return NULL;
}

JsWorker *js_worker_start(const char *node_path, const char *script_path,
    char *error, int errorlen) {
    const char *node = node_path && node_path[0] ? node_path : "node";
    char *script = script_path && script_path[0] ? strdup(script_path) : default_script_path();
    if (!script) { set_error(error, errorlen, "query worker script not found"); return NULL; }
    int in_pipe[2], out_pipe[2];
    if (pipe2(in_pipe, O_CLOEXEC) || pipe2(out_pipe, O_CLOEXEC)) {
        free(script); set_error(error, errorlen, "worker pipe creation failed"); return NULL;
    }
    pid_t pid = fork();
    if (pid < 0) {
        free(script); close(in_pipe[0]); close(in_pipe[1]); close(out_pipe[0]); close(out_pipe[1]);
        set_error(error, errorlen, "worker fork failed"); return NULL;
    }
    if (pid == 0) {
        /* Child: own process group so the parent can kill the whole worker. */
        setpgid(0, 0);
        if (dup2(in_pipe[0], STDIN_FILENO) < 0 || dup2(out_pipe[1], STDOUT_FILENO) < 0) _exit(127);
        close(in_pipe[0]); close(in_pipe[1]); close(out_pipe[0]); close(out_pipe[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        char *argv[] = {(char *)node, "--max-old-space-size=256", script, NULL};
        execvp(argv[0], argv);
        _exit(127);
    }
    close(in_pipe[0]); close(out_pipe[1]);
    free(script);
    JsWorker *worker = calloc(1, sizeof(*worker));
    if (!worker) {
        kill(pid, SIGKILL); close(in_pipe[1]); close(out_pipe[0]);
        set_error(error, errorlen, "out of memory"); return NULL;
    }
    worker->pid = pid;
    worker->in_fd = in_pipe[1];
    worker->out_fd = out_pipe[0];
    worker->failure = JS_WORKER_OK;

    /* Init frame carries the committed bundle exactly once. */
    size_t cap = (size_t)strudel_js_bundle_len * 2 + 256;
    char *frame = malloc(cap);
    if (!frame) {
        js_worker_stop(worker); set_error(error, errorlen, "out of memory"); return NULL;
    }
    int prefix = snprintf(frame, cap, "{\"id\":0,\"op\":\"init\",\"bundle\":\"");
    size_t used = prefix > 0 ? (size_t)prefix : 0;
    int ok = prefix > 0 && used < cap;
    ok = ok && json_escape(frame, cap, &used, (const char *)strudel_js_bundle, strudel_js_bundle_len);
    if (ok) {
        frame[used++] = '"'; frame[used++] = '}'; frame[used] = 0;
    } else {
        free(frame); js_worker_stop(worker);
        set_error(error, errorlen, "bundle frame exceeds the limit");
        return NULL;
    }
    cJSON *response = request(worker, 0, frame, WORKER_COMPILE_BUDGET_MS, error, errorlen);
    free(frame);
    if (!response) { js_worker_stop(worker); return NULL; }
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(response, "v8");
    if (cJSON_IsString(version)) snprintf(worker->version, sizeof(worker->version), "%s", version->valuestring);
    cJSON_Delete(response);
    log_console("query worker: v8 %s (pid %ld)", worker->version, (long)pid);
    return worker;
}

void js_worker_stop(JsWorker *worker) {
    if (!worker) return;
    if (worker->pid > 0) {
        kill(-worker->pid, SIGKILL);
        kill(worker->pid, SIGKILL);
        waitpid(worker->pid, NULL, 0);
        worker->pid = 0;
    }
    if (worker->in_fd > 0) close(worker->in_fd);
    if (worker->out_fd > 0) close(worker->out_fd);
    free(worker->line);
    free(worker);
}

int js_worker_compile(JsWorker *worker, const char *source, double bpm,
    char **metadata, char *error, int errorlen) {
    if (!worker || !source || !metadata) { set_error(error, errorlen, "invalid request"); return 0; }
    if (strlen(source) > WORKER_SOURCE_MAX) { set_error(error, errorlen, "source limit"); return 0; }
    size_t cap = strlen(source) * 2 + 256;
    char *frame = malloc(cap);
    if (!frame) { set_error(error, errorlen, "out of memory"); return 0; }
    int prefix = snprintf(frame, cap, "{\"id\":%llu,\"op\":\"compile\",\"bpm\":%.17g,\"source\":\"",
        ++worker->next_id, bpm);
    size_t used = prefix > 0 ? (size_t)prefix : 0;
    int ok = prefix > 0 && used < cap;
    ok = ok && json_escape(frame, cap, &used, source, strlen(source));
    if (ok) { frame[used++] = '"'; frame[used++] = '}'; frame[used] = 0; }
    if (!ok) { free(frame); set_error(error, errorlen, "request exceeds the frame limit"); return 0; }
    cJSON *response = request(worker, worker->next_id, frame, WORKER_COMPILE_BUDGET_MS, error, errorlen);
    free(frame);
    if (!response) return 0;
    *metadata = copy_string_field(response, "metadata");
    cJSON_Delete(response);
    if (!*metadata) { set_error(error, errorlen, "worker returned no metadata"); return 0; }
    return 1;
}

int js_worker_query(JsWorker *worker, double begin, double end, double cps,
    char **events, char *error, int errorlen) {
    if (!worker || !events) { set_error(error, errorlen, "invalid request"); return 0; }
    char frame[256];
    int written = snprintf(frame, sizeof(frame),
        "{\"id\":%llu,\"op\":\"query\",\"begin\":%.17g,\"end\":%.17g,\"cps\":%.17g}",
        ++worker->next_id, begin, end, cps);
    if (written <= 0 || (size_t)written >= sizeof(frame)) {
        set_error(error, errorlen, "invalid query"); return 0;
    }
    cJSON *response = request(worker, worker->next_id, frame, WORKER_QUERY_BUDGET_MS, error, errorlen);
    if (!response) return 0;
    *events = copy_string_field(response, "events");
    cJSON_Delete(response);
    if (!*events) { set_error(error, errorlen, "worker returned no events"); return 0; }
    return 1;
}

int js_worker_failure(const JsWorker *worker) { return worker ? worker->failure : JS_WORKER_DEAD; }
const char *js_worker_v8_version(const JsWorker *worker) { return worker ? worker->version : ""; }
