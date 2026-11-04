/* Opt-in Node/V8 query worker: transport, event parity against QuickJS, and
   failure behavior. Skips when Node is unavailable; never an audio device. */
#include "js_worker.h"
#include "javascript.h"
#include "common.h"
#include <cjson/cJSON.h>
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;

static void expect(int ok, const char *name) {
    if (!ok) { fprintf(stderr, "FAIL %s\n", name); fails++; }
}

static int node_available(void) {
    const char *path = getenv("PATH");
    if (!path) return 0;
    char buffer[4096];
    snprintf(buffer, sizeof(buffer), "%s", path);
    for (char *dir = strtok(buffer, ":"); dir; dir = strtok(NULL, ":")) {
        char candidate[1200];
        if (snprintf(candidate, sizeof(candidate), "%s/node", dir) >= (int)sizeof(candidate)) continue;
        if (access(candidate, X_OK) == 0) return 1;
    }
    return 0;
}

static void test_parity(void) {
    JsWorker *worker = NULL;
    char error[512] = "";
    worker = js_worker_start(NULL, NULL, error, sizeof(error));
    expect(worker != NULL, "worker starts");
    if (!worker) { fprintf(stderr, "start: %s\n", error); return; }

    const char *sources[] = {
        "note(\"c4 e4 g4\").s(\"sine\").gain(.4)",
        "s(\"[bd sd]*2\").bank(\"RolandTR909\")",
        "note(\"c3 eb3 g3\").s(\"sawtooth\").lpf(1200).room(.3).delay(.2)",
    };
    for (unsigned i = 0; i < sizeof(sources) / sizeof(*sources); i++) {
        char *metadata = NULL;
        if (!js_worker_compile(worker, sources[i], 120, &metadata, error, sizeof(error))) {
            fprintf(stderr, "compile: %s\n", error);
            expect(0, "worker compile");
            break;
        }
        cJSON *worker_metadata = cJSON_Parse(metadata);
        free(metadata);
        expect(cJSON_IsObject(worker_metadata), "worker metadata is JSON");
        cJSON_Delete(worker_metadata);

        char *events = NULL;
        if (!js_worker_query(worker, 0, 1, .5, &events, error, sizeof(error))) {
            fprintf(stderr, "query: %s\n", error);
            expect(0, "worker query");
            break;
        }
        cJSON *worker_events = cJSON_Parse(events);
        free(events);
        expect(cJSON_IsArray(worker_events), "worker events are JSON");
        int worker_count = cJSON_GetArraySize(worker_events);

        cJSON *quick_metadata = NULL;
        JsProgram *program = javascript_compile(sources[i], 120, &quick_metadata, error, sizeof(error));
        expect(program != NULL, "quickjs compiles the same source");
        if (program) {
            cJSON *quick_events = javascript_query(program, 0, 1);
            int quick_count = cJSON_IsArray(quick_events) ? cJSON_GetArraySize(quick_events) : -1;
            expect(worker_count == quick_count, "worker and QuickJS agree on event count");
            /* The first event's value keys and note must agree; both backends
               run the same committed bundle. */
            cJSON *first_worker = cJSON_GetArrayItem(worker_events, 0);
            cJSON *first_quick = cJSON_GetArrayItem(quick_events, 0);
            const cJSON *worker_value = cJSON_GetObjectItemCaseSensitive(first_worker, "value");
            const cJSON *quick_value = cJSON_GetObjectItemCaseSensitive(first_quick, "value");
            expect(cJSON_IsObject(worker_value) && cJSON_IsObject(quick_value), "both events carry values");
            if (cJSON_IsObject(worker_value) && cJSON_IsObject(quick_value)) {
                char *worker_text = cJSON_PrintUnformatted(worker_value);
                char *quick_text = cJSON_PrintUnformatted(quick_value);
                expect(worker_text && quick_text && !strcmp(worker_text, quick_text),
                    "first event value is identical");
                free(worker_text); free(quick_text);
            }
            cJSON_Delete(quick_events);
            javascript_free(program);
        }
        cJSON_Delete(quick_metadata);
        cJSON_Delete(worker_events);
    }
    expect(js_worker_failure(worker) == JS_WORKER_OK, "no failure recorded");
    js_worker_stop(worker);
}

static void test_rejections(void) {
    JsWorker *worker = js_worker_start(NULL, NULL, NULL, 0);
    if (!worker) return;
    char error[512] = "";
    char *metadata = NULL;
    expect(!js_worker_compile(worker, "const x = ;", 120, &metadata, error, sizeof(error)),
        "syntax error is rejected");
    expect(strstr(error, "SyntaxError") != NULL || strstr(error, "error") != NULL,
        "compile error is reported");
    /* A huge source is refused by the parent before any frame is written. */
    size_t big = 4u * 1024u * 1024u + 8;
    char *huge = malloc(big + 1);
    if (huge) {
        memset(huge, ' ', big); huge[big] = 0;
        expect(!js_worker_compile(worker, huge, 120, &metadata, error, sizeof(error)),
            "oversized source is rejected");
        free(huge);
    }
    js_worker_stop(worker);
}

static void test_watchdog(void) {
    JsWorker *worker = js_worker_start(NULL, NULL, NULL, 0);
    if (!worker) return;
    char error[512] = "";
    char *metadata = NULL;
    /* The worker's own compile watchdog (4 s) fires first. */
    expect(!js_worker_compile(worker, "while (true) {}", 120, &metadata, error, sizeof(error)),
        "infinite loop does not return metadata");
    expect(js_worker_failure(worker) != JS_WORKER_OK || strlen(error) > 0,
        "watchdog failure is observable");
    js_worker_stop(worker);
}

static void test_death(void) {
    char error[512] = "";
    /* A node executable that exits immediately must fail startup, not hang. */
    JsWorker *dead = js_worker_start("/bin/false", "scripts/query-worker.mjs", error, sizeof(error));
    expect(dead == NULL, "worker startup with a dead executable fails");
    expect(strlen(error) > 0, "dead startup reports an error");
    /* A missing script makes node exit too. */
    error[0] = 0;
    dead = js_worker_start(NULL, "/nonexistent/query-worker.mjs", error, sizeof(error));
    expect(dead == NULL && strlen(error) > 0, "missing worker script fails startup");
    /* The transport also refuses a null worker instead of crashing. */
    char *events = NULL;
    expect(!js_worker_query(NULL, 0, 1, .5, &events, error, sizeof(error)),
        "query on a missing worker fails");
    expect(js_worker_failure(NULL) == JS_WORKER_DEAD, "missing worker reports death");
}

int main(void) {
    log_console_stderr(0);
    if (!node_available()) {
        puts("node unavailable: query worker checks skipped");
        return 0;
    }
    test_parity();
    test_rejections();
    test_watchdog();
    test_death();
    if (fails) {
        fprintf(stderr, "%d failed\n", fails);
        return 1;
    }
    puts("query worker transport, event parity and failure handling ok");
    return 0;
}
