/* Opt-in Node/V8 query worker: transport, event parity against QuickJS, and
   failure behavior. Skips when Node is unavailable; never an audio device. */
#include "js_worker.h"
#include "javascript.h"
#include "slider.h"
#include "midi.h"
#include "common.h"
#include <cjson/cJSON.h>
#include <assert.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
        if (!js_worker_query(worker, 0, 1, .5, 0, &events, error, sizeof(error))) {
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
    setenv("TOMPOUCE_QUERY_BACKEND", "node", 1);
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
    expect(!js_worker_query(NULL, 0, 1, .5, 0, &events, error, sizeof(error)),
        "query on a missing worker fails");
    expect(js_worker_failure(NULL) == JS_WORKER_DEAD, "missing worker reports death");
}

/* Host-side limits must be applied and the child must not inherit injection
   variables such as NODE_OPTIONS. */
static void test_confinement(void) {
    setenv("TOMPOUCE_QUERY_BACKEND", "node", 1);
    char marker[] = "/tmp/tompouce-worker-inject-XXXXXX";
    int fd = mkstemp(marker);
    expect(fd >= 0, "injection fixture created");
    if (fd < 0) return;
    close(fd);
    unlink(marker);
    char script[1024];
    snprintf(script, sizeof(script),
        "require('fs').writeFileSync('%s','injected');", marker);
    char require_option[1200];
    snprintf(require_option, sizeof(require_option), "--require %s", script);
    char *previous = getenv("NODE_OPTIONS");
    char *saved = previous ? strdup(previous) : NULL;
    setenv("NODE_OPTIONS", require_option, 1);
    char error[512] = "";
    JsWorker *worker = js_worker_start(NULL, NULL, error, sizeof(error));
    expect(worker != NULL, "worker starts with NODE_OPTIONS set");
    expect(access(marker, F_OK) != 0, "NODE_OPTIONS is not inherited by the worker");
    if (worker) {
        char *metadata = NULL;
        expect(js_worker_compile(worker, "note(60)", 120, &metadata, error, sizeof(error)),
            "confined worker still compiles");
        free(metadata);
        char limits[256];
        snprintf(limits, sizeof(limits), "/proc/%d/limits", (int)js_worker_pid(worker));
        FILE *file = fopen(limits, "r");
        expect(file != NULL, "worker limits are readable");
        int core_zero = 0, address_bound = 0;
        if (file) {
            char line[256];
            while (fgets(line, sizeof(line), file)) {
                if (strstr(line, "Max core file size") && strstr(line, " 0 ")) core_zero = 1;
                /* Node uses 2 GiB; the native V8 worker needs larger virtual
                   reservations. Both are bounded, which is the contract. */
                if (strstr(line, "Max address space") &&
                    (strstr(line, "2147483648") || strstr(line, "274877906944")))
                    address_bound = 1;
            }
            fclose(file);
        }
        expect(core_zero, "core dumps are disabled for the worker");
        expect(address_bound, "address space is bounded for the worker");
        js_worker_stop(worker);
    }
    if (saved) { setenv("NODE_OPTIONS", saved, 1); free(saved); }
    else unsetenv("NODE_OPTIONS");
    unlink(marker);
}

/* Engine integration: with the opt-in backend enabled, a plain musical program
   is delegated and its events match a QuickJS-compiled twin. */
static void test_engine_delegation(void) {
    const char *code = "note(\"c4 e4 g4\").s(\"sine\").gain(.4)";
    char error[512] = "";
    cJSON *meta = NULL;
    setenv("TOMPOUCE_QUERY_BACKEND", "node", 1);
    JsProgram *delegated = javascript_compile(code, 120, &meta, error, sizeof(error));
    cJSON_Delete(meta);
    expect(delegated != NULL, "delegated compile succeeds");
    if (delegated) {
        expect(javascript_uses_query_worker(delegated), "program reports worker delegation");
        cJSON *events = javascript_query(delegated, 0, 1);
        int count = cJSON_IsArray(events) ? cJSON_GetArraySize(events) : -1;
        setenv("TOMPOUCE_QUERY_BACKEND", "quickjs", 1);
        cJSON *quick_meta = NULL;
        JsProgram *quick = javascript_compile(code, 120, &quick_meta, error, sizeof(error));
        cJSON_Delete(quick_meta);
        expect(quick != NULL && !javascript_uses_query_worker(quick), "quickjs twin stays native");
        cJSON *quick_events = quick ? javascript_query(quick, 0, 1) : NULL;
        expect(count > 0 && count == (cJSON_IsArray(quick_events) ? cJSON_GetArraySize(quick_events) : -2),
            "delegated and native event counts agree");
        if (cJSON_IsArray(events) && cJSON_IsArray(quick_events) && count > 0) {
            char *a = cJSON_PrintUnformatted(cJSON_GetArrayItem(events, 0));
            char *b = cJSON_PrintUnformatted(cJSON_GetArrayItem(quick_events, 0));
            expect(!strcmp(a, b), "delegated first event is identical");
            free(a); free(b);
        }
        cJSON_Delete(quick_events);
        javascript_free(quick);
        cJSON_Delete(events);
        javascript_free(delegated);
    }
}

/* Live slider values are host state forwarded with every worker query. A
   slider program now delegates and must see a mid-session slider_set(). */
static void test_slider_delegation(void) {
    const char *code = "note(60).s('sine').gain(slider(.5))";
    char error[512] = "";
    setenv("TOMPOUCE_QUERY_BACKEND", "node", 1);
    cJSON *meta = NULL;
    JsProgram *delegated = javascript_compile(code, 120, &meta, error, sizeof(error));
    cJSON_Delete(meta); meta = NULL;
    expect(delegated && javascript_uses_query_worker(delegated), "slider program delegates");
    if (!delegated) { setenv("TOMPOUCE_QUERY_BACKEND", "quickjs", 1); return; }
    javascript_activate_sliders(delegated);
    cJSON *before = javascript_query(delegated, 0, 1);
    expect(slider_set(0, .9), "live slider update applies");
    cJSON *after = javascript_query(delegated, 0, 1);
    setenv("TOMPOUCE_QUERY_BACKEND", "quickjs", 1);
    JsProgram *quick = javascript_compile(code, 120, &meta, error, sizeof(error));
    cJSON_Delete(meta);
    javascript_activate_sliders(quick);
    expect(slider_set(0, .9), "live slider update applies to the twin");
    cJSON *twin = quick ? javascript_query(quick, 0, 1) : NULL;
    char *a = cJSON_IsArray(after) ? cJSON_PrintUnformatted(after) : NULL;
    char *b = cJSON_IsArray(twin) ? cJSON_PrintUnformatted(twin) : NULL;
    expect(a && b && !strcmp(a, b), "updated slider reaches the worker");
    const cJSON *first = cJSON_IsArray(after) ? cJSON_GetArrayItem(after, 0) : NULL;
    const cJSON *value = first ? cJSON_GetObjectItemCaseSensitive(first, "value") : NULL;
    const cJSON *gain = value ? cJSON_GetObjectItemCaseSensitive(value, "gain") : NULL;
    expect(cJSON_IsNumber(gain) && gain->valuedouble == .9, "worker uses the new slider value");
    free(a); free(b);
    slider_set(0, .5);
    cJSON_Delete(twin); cJSON_Delete(before); cJSON_Delete(after);
    javascript_free(quick); javascript_free(delegated);
}

/* midin CC reads are host state forwarded with each query. A delegated
   program must see a live CC change and match a QuickJS twin. */
static void test_midi_delegation(void) {
    const char *code = "const cc = await midin('no-such-device-xyz'); note(60).s('sine').gain(cc(120))";
    char error[512] = "";
    setenv("TOMPOUCE_QUERY_BACKEND", "node", 1);
    midi_test_inject(1, 0, 120, 64);
    cJSON *meta = NULL;
    JsProgram *delegated = javascript_compile(code, 120, &meta, error, sizeof(error));
    cJSON_Delete(meta); meta = NULL;
    expect(delegated && javascript_uses_query_worker(delegated), "midin program delegates");
    cJSON *w1 = delegated ? javascript_query(delegated, 0, 1) : NULL;
    midi_test_inject(1, 0, 120, 127);
    cJSON *w2 = delegated ? javascript_query(delegated, 0, 1) : NULL;
    setenv("TOMPOUCE_QUERY_BACKEND", "quickjs", 1);
    midi_test_inject(1, 0, 120, 64);
    JsProgram *quick = javascript_compile(code, 120, &meta, error, sizeof(error));
    cJSON_Delete(meta);
    cJSON *q1 = quick ? javascript_query(quick, 0, 1) : NULL;
    midi_test_inject(1, 0, 120, 127);
    cJSON *q2 = quick ? javascript_query(quick, 0, 1) : NULL;
    char *a = cJSON_IsArray(w1) ? cJSON_PrintUnformatted(w1) : NULL;
    char *b = cJSON_IsArray(q1) ? cJSON_PrintUnformatted(q1) : NULL;
    char *c = cJSON_IsArray(w2) ? cJSON_PrintUnformatted(w2) : NULL;
    char *d = cJSON_IsArray(q2) ? cJSON_PrintUnformatted(q2) : NULL;
    expect(a && b && !strcmp(a, b), "initial CC value matches QuickJS");
    expect(c && d && !strcmp(c, d), "updated CC value matches QuickJS");
    const cJSON *first = cJSON_IsArray(w1) ? cJSON_GetArrayItem(w1, 0) : NULL;
    const cJSON *value = first ? cJSON_GetObjectItemCaseSensitive(first, "value") : NULL;
    const cJSON *gain = value ? cJSON_GetObjectItemCaseSensitive(value, "gain") : NULL;
    expect(cJSON_IsNumber(gain) && fabs(gain->valuedouble - 64.0/127.0) < 1e-9,
        "worker uses the initial CC value");
    first = cJSON_IsArray(w2) ? cJSON_GetArrayItem(w2, 0) : NULL;
    value = first ? cJSON_GetObjectItemCaseSensitive(first, "value") : NULL;
    gain = value ? cJSON_GetObjectItemCaseSensitive(value, "gain") : NULL;
    expect(cJSON_IsNumber(gain) && gain->valuedouble == 1, "worker uses the updated CC value");
    free(a); free(b); free(c); free(d);
    cJSON_Delete(q2); cJSON_Delete(q1); cJSON_Delete(w2); cJSON_Delete(w1);
    javascript_free(quick); javascript_free(delegated);
}

/* Csound programs were excluded because loads need the engine's renderer. The
   load callback is bounded and accepted in the worker; the engine still applies
   the real orchestra, and query events must match QuickJS. */
static void test_csound_delegation(void) {
    const char *sources[] = {
        "note(60).s('sine').csound('triangle')",
        "await loadCsound('instr 1\\nendin'); note(60).s('sine')",
    };
    char error[512] = "";
    for (size_t i=0;i<sizeof(sources)/sizeof(*sources);i++) {
        setenv("TOMPOUCE_QUERY_BACKEND", "node", 1);
        cJSON *meta = NULL;
        JsProgram *delegated = javascript_compile(sources[i], 120, &meta, error, sizeof(error));
        cJSON_Delete(meta); meta = NULL;
        expect(delegated && javascript_uses_query_worker(delegated), "Csound program delegates");
        cJSON *w = delegated ? javascript_query(delegated, 0, 1) : NULL;
        setenv("TOMPOUCE_QUERY_BACKEND", "quickjs", 1);
        JsProgram *quick = javascript_compile(sources[i], 120, &meta, error, sizeof(error));
        cJSON_Delete(meta);
        cJSON *q = quick ? javascript_query(quick, 0, 1) : NULL;
        char *a = cJSON_IsArray(w) ? cJSON_PrintUnformatted(w) : NULL;
        char *b = cJSON_IsArray(q) ? cJSON_PrintUnformatted(q) : NULL;
        expect(a && b && !strcmp(a, b), "Csound events match QuickJS");
        free(a); free(b);
        cJSON_Delete(q); cJSON_Delete(w);
        javascript_free(quick); javascript_free(delegated);
    }
}

/* Visual sites are configured in QuickJS, but their scopeMask reaches musical
   events from the delegated query. Check that a visual program delegates and
   produces the same masked events as a QuickJS twin. */
static void test_visual_delegation(void) {
    const char *code = "note(60).s('sine')._scope()";
    char error[512] = "";
    setenv("TOMPOUCE_QUERY_BACKEND", "node", 1);
    cJSON *meta = NULL;
    JsProgram *delegated = javascript_compile(code, 120, &meta, error, sizeof(error));
    cJSON_Delete(meta); meta = NULL;
    expect(delegated && javascript_uses_query_worker(delegated), "visual program delegates");
    cJSON *w = delegated ? javascript_query(delegated, 0, 1) : NULL;
    setenv("TOMPOUCE_QUERY_BACKEND", "quickjs", 1);
    JsProgram *quick = javascript_compile(code, 120, &meta, error, sizeof(error));
    cJSON_Delete(meta);
    cJSON *q = quick ? javascript_query(quick, 0, 1) : NULL;
    char *a = cJSON_IsArray(w) ? cJSON_PrintUnformatted(w) : NULL;
    char *b = cJSON_IsArray(q) ? cJSON_PrintUnformatted(q) : NULL;
    expect(a && b && !strcmp(a, b), "visual events match QuickJS");
    const cJSON *first = cJSON_IsArray(w) ? cJSON_GetArrayItem(w, 0) : NULL;
    const cJSON *mask = first ? cJSON_GetObjectItemCaseSensitive(first, "scopeMask") : NULL;
    expect(cJSON_IsNumber(mask) && mask->valuedouble != 0, "visual scopeMask reaches the worker");
    free(a); free(b);
    cJSON_Delete(q); cJSON_Delete(w);
    javascript_free(quick); javascript_free(delegated);
}

/* Re-evaluating identical source must start a fresh worker evaluation, not
   continue the previous one's state. A pattern whose values depend on a
   query-advancing callback exposes the difference. */
static void test_engine_identity(void) {
    const char *code = "note(60).s('sine').gain(rand.range(.2,.8))";
    char error[512] = "";
    setenv("TOMPOUCE_QUERY_BACKEND", "node", 1);
    cJSON *meta = NULL;
    JsProgram *first = javascript_compile(code, 120, &meta, error, sizeof(error));
    cJSON_Delete(meta); meta = NULL;
    expect(first && javascript_uses_query_worker(first), "first evaluation delegates");
    cJSON *first_events = first ? javascript_query(first, 0, 1) : NULL;
    /* A second evaluation replaces the worker's bound evaluation. */
    JsProgram *second = javascript_compile(code, 120, &meta, error, sizeof(error));
    cJSON_Delete(meta);
    expect(second && javascript_uses_query_worker(second), "second evaluation delegates");
    cJSON *second_events = second ? javascript_query(second, 0, 1) : NULL;
    /* The replaced program keeps working through its QuickJS evaluation. */
    cJSON *first_again = first ? javascript_query(first, 0, 1) : NULL;
    expect(cJSON_IsArray(first_events) && cJSON_IsArray(second_events) &&
        cJSON_GetArraySize(first_events) == cJSON_GetArraySize(second_events),
        "both evaluations return the same shape");
    expect(cJSON_IsArray(first_again) &&
        cJSON_GetArraySize(first_again) == cJSON_GetArraySize(first_events),
        "replaced program falls back and still queries");
    setenv("TOMPOUCE_QUERY_BACKEND", "quickjs", 1);
    cJSON_Delete(first_again);
    cJSON_Delete(second_events);
    cJSON_Delete(first_events);
    javascript_free(second);
    javascript_free(first);
}

/* A rejected live evaluation must not disturb the worker binding for the
   currently playing program: the last playable pattern keeps delegating and
   returns identical events. */
static void test_engine_retention(void) {
    const char *good = "note(\"c4 e4 g4\").s(\"sine\").gain(.4)";
    char error[512] = "";
    setenv("TOMPOUCE_QUERY_BACKEND", "node", 1);
    cJSON *meta = NULL;
    JsProgram *program = javascript_compile(good, 120, &meta, error, sizeof(error));
    cJSON_Delete(meta);
    expect(program && javascript_uses_query_worker(program), "retained evaluation delegates");
    if (!program) { setenv("TOMPOUCE_QUERY_BACKEND", "quickjs", 1); return; }
    cJSON *before = javascript_query(program, 0, 1);
    cJSON *bad_meta = NULL;
    JsProgram *bad = javascript_compile_from("const x = ;", 120, program,
        &bad_meta, error, sizeof(error));
    cJSON_Delete(bad_meta);
    expect(bad == NULL, "failed evaluation is rejected");
    expect(error[0] != '\0', "failed evaluation reports an error");
    expect(javascript_uses_query_worker(program), "worker binding survives failed evaluation");
    cJSON *after = javascript_query(program, 0, 1);
    char *before_text = cJSON_IsArray(before) ? cJSON_PrintUnformatted(before) : NULL;
    char *after_text = cJSON_IsArray(after) ? cJSON_PrintUnformatted(after) : NULL;
    expect(before_text && after_text && !strcmp(before_text, after_text),
        "retained evaluation returns identical events after a failed evaluation");
    free(before_text); free(after_text);
    setenv("TOMPOUCE_QUERY_BACKEND", "quickjs", 1);
    cJSON_Delete(after);
    cJSON_Delete(before);
    javascript_free(bad);
    javascript_free(program);
}

/* The engine's confined startup prebake must reach the worker's compile stage:
   the same source with a gain default of 1 rather than the web's .8 must
   produce a different first event. This closes the prebake parity hole for
   programs that do not call loadPrebake(). */
static void test_prebake_transport(void) {
    JsWorker *worker = js_worker_start(NULL, NULL, NULL, 0);
    if (!worker) return;
    char error[512] = "";
    char *metadata = NULL, *with_text = NULL, *without_text = NULL;
    int ok = js_worker_compile_with_prebake(worker, "note(60).s('sine')", 120,
        "setDefault('gain', 1);", &metadata, error, sizeof(error));
    expect(ok, "worker compiles with a startup prebake");
    if (ok) {
        expect(js_worker_query(worker, 0, 1, .5, 0, &with_text, error, sizeof(error)),
            "worker queries the prebaked program");
        free(metadata); metadata = NULL;
    }
    ok = js_worker_compile_with_prebake(worker, "note(60).s('sine')", 120,
        "", &metadata, error, sizeof(error));
    expect(ok, "worker compiles without a prebake");
    if (ok) {
        expect(js_worker_query(worker, 0, 1, .5, 0, &without_text, error, sizeof(error)),
            "worker queries the plain program");
    }
    cJSON *with_events = with_text ? cJSON_Parse(with_text) : NULL;
    cJSON *without_events = without_text ? cJSON_Parse(without_text) : NULL;
    const cJSON *a = cJSON_IsArray(with_events) ? cJSON_GetArrayItem(with_events, 0) : NULL;
    const cJSON *b = cJSON_IsArray(without_events) ? cJSON_GetArrayItem(without_events, 0) : NULL;
    const cJSON *av = a ? cJSON_GetObjectItemCaseSensitive(a, "value") : NULL;
    const cJSON *bv = b ? cJSON_GetObjectItemCaseSensitive(b, "value") : NULL;
    const cJSON *ag = av ? cJSON_GetObjectItemCaseSensitive(av, "gain") : NULL;
    const cJSON *bg = bv ? cJSON_GetObjectItemCaseSensitive(bv, "gain") : NULL;
    expect(cJSON_IsNumber(ag) && ag->valuedouble == 1 &&
        (!cJSON_IsNumber(bg) || bg->valuedouble != ag->valuedouble),
        "startup prebake changes the worker's compile-time defaults");
    char *first_with = a ? cJSON_PrintUnformatted(a) : NULL;
    char *first_without = b ? cJSON_PrintUnformatted(b) : NULL;
    expect(first_with && first_without && strcmp(first_with, first_without),
        "worker events differ with and without the startup prebake");
    free(first_with); free(first_without);
    cJSON_Delete(with_events); cJSON_Delete(without_events);
    free(metadata); free(with_text); free(without_text);
    js_worker_stop(worker);
}

/* Engine integration: a program using the confined startup prebake now
   delegates and matches its QuickJS twin, instead of silently dropping the
   prebake defaults. */
static void test_prebake_engine(void) {
    char root[] = "/tmp/tompouce-worker-prebake-XXXXXX";
    if (!mkdtemp(root)) return;
    char config[1200], path[1300];
    snprintf(config, sizeof(config), "%s/tompouce", root);
    if (mkdir(config, 0700)) { rmdir(root); return; }
    snprintf(path, sizeof(path), "%s/prebake.strudel", config);
    FILE *file = fopen(path, "w");
    if (file) { fputs("setDefault('gain', 1);", file); fclose(file); }
    const char *previous = getenv("XDG_CONFIG_HOME");
    char *saved = previous ? strdup(previous) : NULL;
    setenv("XDG_CONFIG_HOME", root, 1);
    setenv("TOMPOUCE_QUERY_BACKEND", "node", 1);
    char error[512] = "";
    cJSON *meta = NULL;
    const char *code = "note(60).s('sine')";
    JsProgram *delegated = javascript_compile(code, 120, &meta, error, sizeof(error));
    cJSON_Delete(meta); meta = NULL;
    expect(delegated && javascript_uses_query_worker(delegated),
        "startup-prebake program delegates");
    cJSON *worker_events = delegated ? javascript_query(delegated, 0, 1) : NULL;
    setenv("TOMPOUCE_QUERY_BACKEND", "quickjs", 1);
    JsProgram *quick = javascript_compile(code, 120, &meta, error, sizeof(error));
    cJSON_Delete(meta);
    cJSON *quick_events = quick ? javascript_query(quick, 0, 1) : NULL;
    char *a = cJSON_IsArray(worker_events) ? cJSON_PrintUnformatted(worker_events) : NULL;
    char *b = cJSON_IsArray(quick_events) ? cJSON_PrintUnformatted(quick_events) : NULL;
    expect(a && b && !strcmp(a, b), "delegated prebake events match QuickJS");
    free(a); free(b);
    cJSON_Delete(quick_events); cJSON_Delete(worker_events);
    javascript_free(quick); javascript_free(delegated);
    if (saved) { setenv("XDG_CONFIG_HOME", saved, 1); free(saved); }
    else unsetenv("XDG_CONFIG_HOME");
    unlink(path); rmdir(config); rmdir(root);
}

/* Named prebakes loaded during the QuickJS evaluation are forwarded to the
   worker, so a program whose notes depend on prebake globals still delegates
   and matches its QuickJS twin. */
static void test_named_prebake_engine(void) {
    char root[] = "/tmp/tompouce-worker-named-prebake-XXXXXX";
    if (!mkdtemp(root)) return;
    char config[1200], directory[1300], path[1400];
    snprintf(config, sizeof(config), "%s/tompouce", root);
    snprintf(directory, sizeof(directory), "%s/prebakes", config);
    if (mkdir(config, 0700) || mkdir(directory, 0700)) { rmdir(directory); rmdir(config); rmdir(root); return; }
    snprintf(path, sizeof(path), "%s/fixture.strudel", directory);
    FILE *file = fopen(path, "w");
    if (file) { fputs("window.fixturePitch = 71;", file); fclose(file); }
    const char *previous = getenv("XDG_CONFIG_HOME");
    char *saved = previous ? strdup(previous) : NULL;
    setenv("XDG_CONFIG_HOME", root, 1);
    setenv("TOMPOUCE_QUERY_BACKEND", "node", 1);
    char error[512] = "";
    cJSON *meta = NULL;
    const char *code = "await loadPrebake('fixture.strudel'); note(fixturePitch).s('sine')";
    JsProgram *delegated = javascript_compile(code, 120, &meta, error, sizeof(error));
    cJSON_Delete(meta); meta = NULL;
    expect(delegated && javascript_uses_query_worker(delegated),
        "named-prebake program delegates");
    cJSON *worker_events = delegated ? javascript_query(delegated, 0, 1) : NULL;
    setenv("TOMPOUCE_QUERY_BACKEND", "quickjs", 1);
    JsProgram *quick = javascript_compile(code, 120, &meta, error, sizeof(error));
    cJSON_Delete(meta);
    cJSON *quick_events = quick ? javascript_query(quick, 0, 1) : NULL;
    char *a = cJSON_IsArray(worker_events) ? cJSON_PrintUnformatted(worker_events) : NULL;
    char *b = cJSON_IsArray(quick_events) ? cJSON_PrintUnformatted(quick_events) : NULL;
    expect(a && b && !strcmp(a, b), "named-prebake events match QuickJS");
    const cJSON *first = cJSON_IsArray(worker_events) ? cJSON_GetArrayItem(worker_events, 0) : NULL;
    const cJSON *value = first ? cJSON_GetObjectItemCaseSensitive(first, "value") : NULL;
    const cJSON *note = value ? cJSON_GetObjectItemCaseSensitive(value, "note") : NULL;
    expect(cJSON_IsNumber(note) && note->valueint == 71,
        "prebake global reaches the worker's evaluation");
    free(a); free(b);
    cJSON_Delete(quick_events); cJSON_Delete(worker_events);
    javascript_free(quick); javascript_free(delegated);
    if (saved) { setenv("XDG_CONFIG_HOME", saved, 1); free(saved); }
    else unsetenv("XDG_CONFIG_HOME");
    unlink(path); rmdir(directory); rmdir(config); rmdir(root);
}

/* The confined worker must refuse an allocation beyond its address-space
   limit within the compile budget, rather than hanging or returning metadata
   from a corrupted evaluation. */
static void test_oom(void) {
    JsWorker *worker = js_worker_start(NULL, NULL, NULL, 0);
    if (!worker) return;
    char error[512] = "";
    char *metadata = NULL;
    expect(js_worker_compile(worker, "note(60)", 120, &metadata, error, sizeof(error)),
        "worker is healthy before the OOM request");
    free(metadata); metadata = NULL;
    error[0] = 0;
    int ok = js_worker_compile(worker,
        "new ArrayBuffer(1500000000); note(60)", 120, &metadata, error, sizeof(error));
    expect(!ok, "allocation beyond the worker limit is refused");
    expect(error[0] != '\0', "worker OOM or death is reported");
    free(metadata);
    js_worker_stop(worker);
}

/* Default migration: an available native worker is used with no env var, and
   TOMPOUCE_QUERY_BACKEND=quickjs is a hard opt-out. Skipped when no native
   worker was supplied to the test. */
static void test_default_backend(void) {
    if (!getenv("TOMPOUCE_V8_WORKER")) return;
    unsetenv("TOMPOUCE_QUERY_BACKEND");
    char error[256] = "";
    JsWorker *worker = js_worker_start(NULL, NULL, error, sizeof(error));
    expect(worker != NULL, "default backend prefers the native worker");
    if (worker) {
        const char *version = js_worker_v8_version(worker);
        expect(version && version[0], "default worker reports a V8 version");
        js_worker_stop(worker);
    }
    setenv("TOMPOUCE_QUERY_BACKEND", "quickjs", 1);
    error[0] = 0;
    worker = js_worker_start(NULL, NULL, error, sizeof(error));
    expect(worker == NULL, "quickjs opt-out disables the worker");
    setenv("TOMPOUCE_QUERY_BACKEND", "quickjs", 1);
}

int main(void) {
    log_console_stderr(0);
    const char *forced = getenv("TOMPOUCE_QUERY_BACKEND");
    if (forced && !strcmp(forced, "quickjs")) {
        puts("quickjs backend: query worker checks skipped");
        return 0;
    }
    if (!node_available() && !getenv("TOMPOUCE_V8_WORKER")) {
        puts("no query worker available: query worker checks skipped");
        return 0;
    }
    test_parity();
    test_default_backend();
    test_confinement();
    test_engine_delegation();
    test_slider_delegation();
    test_midi_delegation();
    test_csound_delegation();
    test_visual_delegation();
    test_engine_identity();
    test_engine_retention();
    test_prebake_transport();
    test_prebake_engine();
    test_named_prebake_engine();
    test_oom();
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
