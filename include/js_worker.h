#ifndef JS_WORKER_H
#define JS_WORKER_H
#include <stddef.h>

/* Opt-in pattern-query worker running the committed bundle under Node's V8.
   QuickJS stays the default backend; this transport exists so the engine can
   delegate compile/query work to a separate process without linking V8.

   The worker is untrusted from the engine's point of view: every frame is
   bounded, every request has a deadline, and a timeout or dead process is
   reported instead of blocking playback. Node/V8 is not a sandbox; the parent
   owns confinement (see docs/v8-assessment.md). */
typedef struct JsWorker JsWorker;

enum { JS_WORKER_OK = 0, JS_WORKER_TIMEOUT = 1, JS_WORKER_DEAD = 2, JS_WORKER_PROTOCOL = 3 };

/* Start a worker. node_path defaults to "node" from PATH; script_path
   defaults to $TOMPOUCE_QUERY_WORKER or scripts/query-worker.mjs next to the
   source tree. Returns NULL with a message on failure. */
JsWorker *js_worker_start(const char *node_path, const char *script_path,
    char *error, int errorlen);
/* Kill the worker if it is still running. Safe on NULL. */
void js_worker_stop(JsWorker *worker);

/* Compile source and return the metadata JSON (caller frees) or NULL. */
int js_worker_compile(JsWorker *worker, const char *source, double bpm,
    char **metadata, char *error, int errorlen);
/* Query [begin,end) at cps and return the events JSON (caller frees) or NULL. */
int js_worker_query(JsWorker *worker, double begin, double end, double cps,
    char **events, char *error, int errorlen);

/* Last failure class (JS_WORKER_*), for diagnostics. */
int js_worker_failure(const JsWorker *worker);
/* V8 version reported at init, or "" when unknown. */
const char *js_worker_v8_version(const JsWorker *worker);

#endif
