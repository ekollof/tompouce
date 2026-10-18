#include "javascript.h"
#include "common.h"
#include "slider.h"
#include <quickjs.h>
#include <ctype.h>
#include <limits.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "runtime.inc"

#define JS_MEMORY_LIMIT (128 * 1024 * 1024)
#define JS_TEXT_LIMIT (4 * 1024 * 1024)
#define JS_SPANS 2048

struct JsProgram {
    CsoundProgram *csound;
    atomic_uint refs;
    pthread_mutex_t lock;
    pthread_mutex_t spans_lock;
    int lock_ready;
    JSRuntime *runtime;
    JSContext *context;
    double deadline;
    char *source;
    struct { int offset, kind, id; unsigned mask; VisualOptions options; } visual_sites[32];
    int nvisual_sites;
    char *visual_requested, *visual_pending, *visual_source;
    unsigned visual_revision;
    struct HighlightRange { int begin, end, target; } *highlight_map;
    int nhighlight_map;
    struct VisualCachedNote { unsigned mask; int kind; VisualNote note; } *visual_notes;
    int nvisual_notes, visual_warned;
    double visual_next, visual_origin, visual_seconds;
    struct { double begin, end; int off, len; uint32_t color; unsigned flags; } spans[JS_SPANS];
    unsigned head;
    char warned[32][64];
    int nwarned;
    int loading_allowed;
};

static int interrupted(JSRuntime *runtime, void *opaque) {
    (void)runtime;
    JsProgram *p = opaque;
    return now_seconds() > p->deadline;
}

static JSValue host_log(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    if (argc) {
        const char *text = JS_ToCString(ctx, argv[0]);
        if (text) { log_console("js: %.1000s", text); JS_FreeCString(ctx, text); }
    }
    return JS_UNDEFINED;
}

static JSValue host_slider(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    double v[4] = {0, 0, 1, .05};
    for (int i = 0; i < argc && i < 4; i++)
        if (JS_ToFloat64(ctx, &v[i], argv[i]) < 0 || !isfinite(v[i]))
            return JS_ThrowTypeError(ctx, "slider arguments must be finite numbers");
    return JS_NewFloat64(ctx, slider_bind(v[0], v[1], v[2], v[3]));
}

static JSValue global_function(JSContext *ctx, const char *name) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue fn = JS_GetPropertyStr(ctx, global, name);
    JS_FreeValue(ctx, global);
    return fn;
}

static void exception_text(JSContext *ctx, JSValue error, char *out, int n) {
    const char *text = JS_ToCString(ctx, error);
    if (out && n > 0) snprintf(out, (size_t)n, "%s", text ? text : "JavaScript exception");
    if (text) JS_FreeCString(ctx, text);
}

static int local_name_safe(const char *name) {
    if (!name || !*name || strlen(name) > 240 || *name == '/' || strstr(name,"..")) return 0;
    for (const unsigned char *p = (const unsigned char *)name; *p; p++)
        if (!(isalnum(*p) || *p == '/' || *p == '_' || *p == '-' || *p == '.')) return 0;
    size_t n = strlen(name);
    return (n > 3 && !strcmp(name+n-3,".js")) || (n > 4 && !strcmp(name+n-4,".mjs"));
}

static char *module_normalize(JSContext *ctx, const char *base, const char *name, void *opaque) {
    (void)opaque;
    if (!strncmp(name,"@strudel/",9)) return js_strdup(ctx,name);
    char relative[256];
    int n;
    if (!strncmp(name,"local:",6)) n = snprintf(relative,sizeof(relative),"%s",name+6);
    else if (!strncmp(name,"./",2)) {
        const char *slash = !strncmp(base,"local:",6) ? strrchr(base+6,'/') : NULL;
        n = slash ? snprintf(relative,sizeof(relative),"%.*s/%s",(int)(slash-(base+6)),base+6,name+2)
                  : snprintf(relative,sizeof(relative),"%s",name+2);
    } else return JS_ThrowReferenceError(ctx,"module must be bundled or local: / ./ under the modules directory"), NULL;
    if (n < 0 || (size_t)n >= sizeof(relative) || !local_name_safe(relative))
        return JS_ThrowReferenceError(ctx,"unsafe local module name"), NULL;
    char normalized[264];
    snprintf(normalized,sizeof(normalized),"local:%s",relative);
    return js_strdup(ctx,normalized);
}

static char *local_module_source(JSContext *ctx, const char *name, size_t *size) {
    char root[1024], path[1400];
    user_dir(root,sizeof(root),"XDG_CONFIG_HOME","tompouce",".config/tompouce");
    int n = snprintf(path,sizeof(path),"%s/modules/%s",root,name);
    if (!*root || n < 0 || (size_t)n >= sizeof(path) || !local_name_safe(name)) {
        JS_ThrowReferenceError(ctx,"unsafe module path"); return NULL;
    }
    int dir = open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    char *save = NULL, *component = strtok_r(path,"/",&save);
    while (dir >= 0 && component) {
        char *next = strtok_r(NULL,"/",&save);
        int fd = openat(dir,component,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|(next ? O_DIRECTORY : O_NONBLOCK));
        close(dir); dir = fd; component = next;
    }
    struct stat st;
    if (dir < 0 || fstat(dir,&st) || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > 256*1024) {
        if (dir >= 0) close(dir);
        JS_ThrowReferenceError(ctx,"local module missing, linked, or larger than 256 KiB"); return NULL;
    }
    *size = (size_t)st.st_size;
    char *text = malloc(*size+1);
    if (!text) { close(dir); JS_ThrowOutOfMemory(ctx); return NULL; }
    size_t used = 0;
    while (used < *size) {
        ssize_t got = read(dir,text+used,*size-used);
        if (got <= 0) { free(text); close(dir); JS_ThrowReferenceError(ctx,"module read failed"); return NULL; }
        used += (size_t)got;
    }
    close(dir); text[used] = 0;
    return text;
}

static JSModuleDef *module_loader(JSContext *ctx, const char *name, void *opaque) {
    JsProgram *p = opaque;
    if (!strncmp(name,"local:",6)) {
        if (!p->loading_allowed) { JS_ThrowReferenceError(ctx,"load local modules during evaluation, before pattern queries"); return NULL; }
        size_t size;
        char *source = local_module_source(ctx,name+6,&size);
        if (!source) return NULL;
        JSValue module = JS_Eval(ctx,source,size,name,JS_EVAL_TYPE_MODULE|JS_EVAL_FLAG_COMPILE_ONLY);
        free(source);
        if (JS_IsException(module)) return NULL;
        JSModuleDef *result = JS_VALUE_GET_PTR(module);
        JS_FreeValue(ctx,module); return result;
    }
    JSValue fn = global_function(ctx, "__moduleSource");
    JSValue arg = JS_NewString(ctx, name);
    JSValue value = JS_Call(ctx, fn, JS_UNDEFINED, 1, &arg);
    JS_FreeValue(ctx, arg); JS_FreeValue(ctx, fn);
    if (JS_IsException(value)) return NULL;
    size_t n;
    const char *source = JS_ToCStringLen(ctx, &n, value);
    JS_FreeValue(ctx, value);
    if (!source) return NULL;
    JSValue module = JS_Eval(ctx, source, n, name, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
    JS_FreeCString(ctx, source);
    if (JS_IsException(module)) return NULL;
    JSModuleDef *result = JS_VALUE_GET_PTR(module);
    JS_FreeValue(ctx, module);
    return result;
}

void javascript_free(JsProgram *p) {
    if (!p) return;
    if (atomic_fetch_sub_explicit(&p->refs,1,memory_order_acq_rel) != 1) return;
    csound_program_free(p->csound);
    if (p->context) JS_FreeContext(p->context);
    if (p->runtime) JS_FreeRuntime(p->runtime);
    free(p->source);
    free(p->visual_notes);
    free(p->visual_requested); free(p->visual_pending); free(p->visual_source);
    free(p->highlight_map);
    if (p->lock_ready >= 2) pthread_mutex_destroy(&p->spans_lock);
    if (p->lock_ready >= 1) pthread_mutex_destroy(&p->lock);
    free(p);
}

void javascript_retain(JsProgram *p) {
    if (p) atomic_fetch_add_explicit(&p->refs,1,memory_order_relaxed);
}

int javascript_prepare_csound(JsProgram *p, const cJSON *metadata, char *error, size_t size) {
    const cJSON *loads = cJSON_GetObjectItemCaseSensitive(metadata,"csound");
    if (!cJSON_IsArray(loads)) return 1;
    p->csound = csound_program_create(loads,error,size);
    return p->csound != NULL;
}
CsoundProgram *javascript_csound(JsProgram *p) { return p ? p->csound : NULL; }

static int initialize(JsProgram *p, char *error, int n) {
    p->runtime = JS_NewRuntime();
    if (!p->runtime) return 0;
    JS_SetMemoryLimit(p->runtime, JS_MEMORY_LIMIT);
    JS_SetMaxStackSize(p->runtime, 1024 * 1024);
    JS_SetInterruptHandler(p->runtime, interrupted, p);
    JS_SetModuleLoaderFunc(p->runtime, module_normalize, module_loader, p);
    p->context = JS_NewContext(p->runtime);
    if (!p->context) return 0;
    JSContext *ctx = p->context;
    JSValue global = JS_GetGlobalObject(ctx);
    int a = JS_SetPropertyStr(ctx, global, "__hostLog", JS_NewCFunction(ctx, host_log, "__hostLog", 1));
    int b = JS_SetPropertyStr(ctx, global, "__hostSlider", JS_NewCFunction(ctx, host_slider, "__hostSlider", 4));
    JS_FreeValue(ctx, global);
    if (a < 0 || b < 0) return 0;
    const char *prelude = "globalThis.performance={now:()=>Date.now()};globalThis.console={log(){},warn(){},error:__hostLog,info(){},debug(){}};";
    JSValue value = JS_Eval(ctx, prelude, strlen(prelude), "host.js", JS_EVAL_TYPE_GLOBAL);
    if (!JS_IsException(value)) {
        JS_FreeValue(ctx, value);
        char *bundle = malloc((size_t)strudel_js_bundle_len + 1);
        if (!bundle) return 0;
        memcpy(bundle, strudel_js_bundle, strudel_js_bundle_len);
        bundle[strudel_js_bundle_len] = 0;
        value = JS_Eval(ctx, bundle, strudel_js_bundle_len, "strudel-runtime.js", JS_EVAL_TYPE_GLOBAL);
        free(bundle);
    }
    if (JS_IsException(value)) {
        JSValue exception = JS_GetException(ctx);
        exception_text(ctx, exception, error, n);
        JS_FreeValue(ctx, exception); JS_FreeValue(ctx, value);
        return 0;
    }
    JS_FreeValue(ctx, value);
    return 1;
}

/* Only the traditional line dialect bypasses the full JavaScript runtime. */
int javascript_is_code(const char *code) {
    if (!code) return 0;
    const char *p = code;
    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        if (*p == '#' || (p[0] == '/' && p[1] == '/')) {
            while (*p && *p != '\n') p++;
            continue;
        }
        if (!*p) return 0;
        const char *words[] = {"bpm", "drum", "drums", "notes", "note", "n", "sound", "s", "bass", "lead", "chord", "melody", "noise", "fx", "stop", NULL};
        for (int i = 0; words[i]; i++) {
            size_t n = strlen(words[i]);
            if (strncmp(p, words[i], n) != 0 || !isspace((unsigned char)p[n])) continue;
            const char *q = p + n;
            while (*q == ' ' || *q == '\t') q++;
            if (*q != '(') return 0;
        }
        return 1;
    }
    return 0;
}

int javascript_check(const char *code, char *error, int n, int *line) {
    JsProgram p = {0};
    p.deadline = now_seconds()+.03;
    p.runtime = JS_NewRuntime();
    if (!p.runtime) return 0;
    JS_SetMemoryLimit(p.runtime,16*1024*1024);
    JS_SetMaxStackSize(p.runtime,512*1024);
    JS_SetInterruptHandler(p.runtime,interrupted,&p);
    p.context = JS_NewContext(p.runtime);
    if (!p.context) { JS_FreeRuntime(p.runtime); return 0; }
    JSValue result = JS_Eval(p.context,code,strlen(code),"editor.js",JS_EVAL_TYPE_MODULE|JS_EVAL_FLAG_COMPILE_ONLY);
    int ok = !JS_IsException(result);
    if (!ok) {
        JSValue exception = JS_GetException(p.context);
        exception_text(p.context,exception,error,n);
        JSValue at = JS_GetPropertyStr(p.context,exception,"lineNumber");
        int32_t row = 0;
        if (JS_ToInt32(p.context,&row,at) < 0) row = 0;
        if (row < 1) {
            JSValue stack = JS_GetPropertyStr(p.context,exception,"stack");
            const char *text = JS_ToCString(p.context,stack);
            const char *position = text ? strstr(text,"editor.js:") : NULL;
            if (position) {
                long nline = strtol(position+10,NULL,10);
                if (nline >= 1 && nline <= 2147483647L) row = (int32_t)nline;
            }
            if (text) JS_FreeCString(p.context,text);
            JS_FreeValue(p.context,stack);
        }
        if (line) *line = row;
        JS_FreeValue(p.context,at); JS_FreeValue(p.context,exception);
    }
    JS_FreeValue(p.context,result);
    JS_FreeContext(p.context); JS_FreeRuntime(p.runtime);
    return ok;
}

static cJSON *json_value(JSContext *ctx, JSValue value) {
    size_t n;
    const char *text = JS_ToCStringLen(ctx, &n, value);
    if (!text) return NULL;
    cJSON *json = n <= JS_TEXT_LIMIT ? cJSON_ParseWithLength(text, n + 1) : NULL;
    JS_FreeCString(ctx, text);
    return json;
}

static int utf8_offset(const char *s, int units);
static int visual_source_matches(const char *compiled, const char *editor);

JsProgram *javascript_compile(const char *code, double bpm, cJSON **metadata, char *error, int n) {
    if (metadata) *metadata = NULL;
    if (error && n > 0) snprintf(error, (size_t)n, "could not initialize JavaScript runtime");
    if (!code || strlen(code) > (size_t)INT_MAX) return NULL;
    JsProgram *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    atomic_init(&p->refs,1);
    if (pthread_mutex_init(&p->lock,NULL) != 0) { free(p); return NULL; }
    p->lock_ready = 1;
    if (pthread_mutex_init(&p->spans_lock,NULL) != 0) { javascript_free(p); return NULL; }
    p->lock_ready = 2;
    p->deadline = now_seconds() + 2.0;
    p->loading_allowed = 1;
    p->source = strdup_safe(code);
    p->visual_source = strdup_safe(code);
    if (!p->source || !p->visual_source || !initialize(p, error, n)) { javascript_free(p); return NULL; }
    JSContext *ctx = p->context;
    JSValue fn = global_function(ctx, "__compile");
    JSValue args[2] = {JS_NewString(ctx,code),JS_NewFloat64(ctx,isfinite(bpm) ? bpm : 120)};
    JSValue value = JS_Call(ctx, fn, JS_UNDEFINED, 2, args);
    JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx,args[1]); JS_FreeValue(ctx, fn);
    while (!JS_IsException(value) && JS_PromiseState(ctx, value) == JS_PROMISE_PENDING) {
        JSContext *job_ctx = NULL;
        int result = JS_ExecutePendingJob(p->runtime, &job_ctx);
        if (result <= 0 || now_seconds() > p->deadline) break;
    }
    JSValue result = JS_UNDEFINED;
    if (JS_IsException(value)) result = JS_GetException(ctx);
    else if (JS_PromiseState(ctx, value) == JS_PROMISE_REJECTED) result = JS_PromiseResult(ctx, value);
    else if (JS_PromiseState(ctx, value) == JS_PROMISE_FULFILLED) {
        result = JS_PromiseResult(ctx, value);
        cJSON *json = json_value(ctx, result);
        JS_FreeValue(ctx, result); JS_FreeValue(ctx, value);
        if (json) {
            const cJSON *site;
            cJSON_ArrayForEach(site,cJSON_GetObjectItemCaseSensitive(json,"visualSites")) {
                const cJSON *offset = cJSON_GetObjectItemCaseSensitive(site,"offset");
                const cJSON *id = cJSON_GetObjectItemCaseSensitive(site,"id");
                const cJSON *kind = cJSON_GetObjectItemCaseSensitive(site,"kind");
                if (p->nvisual_sites >= 32 || !cJSON_IsNumber(offset) || !cJSON_IsNumber(id) || !cJSON_IsNumber(kind) ||
                    offset->valuedouble < 0 || offset->valuedouble > (double)strlen(code) ||
                    id->valuedouble < 0 || id->valuedouble >= 32 || floor(id->valuedouble) != id->valuedouble ||
                    kind->valuedouble < 1 || kind->valuedouble > VISUAL_KINDS || floor(kind->valuedouble) != kind->valuedouble) continue;
                int i = p->nvisual_sites++;
                p->visual_sites[i].offset = utf8_offset(code,offset->valueint);
                p->visual_sites[i].kind = kind->valueint;
                p->visual_sites[i].id = id->valueint;
                p->visual_sites[i].mask = 1u << id->valueint;
                visual_options(&p->visual_sites[i].options,kind->valueint,cJSON_GetObjectItemCaseSensitive(site,"options"));
            }
            if (p->nvisual_sites) {
                p->visual_notes = calloc(VISUAL_CACHE_NOTES,sizeof(*p->visual_notes));
                if (!p->visual_notes) { cJSON_Delete(json); javascript_free(p); return NULL; }
            }
            p->loading_allowed = 0;
            if (metadata) *metadata = json; else cJSON_Delete(json);
            return p;
        }
        javascript_free(p); return NULL;
    }
    if (JS_IsUndefined(result) && error && n > 0)
        snprintf(error,(size_t)n,"JavaScript evaluation did not settle within its execution budget");
    else exception_text(ctx, result, error, n);
    JS_FreeValue(ctx, result); JS_FreeValue(ctx, value);
    javascript_free(p); return NULL;
}

static cJSON *query_events(JsProgram *p, double begin, double end, double cps, int probes) {
    if (!p || !isfinite(begin) || !isfinite(end) || begin < 0 || end <= begin || end - begin > 1.001) return NULL;
    pthread_mutex_lock(&p->lock);
    JS_UpdateStackTop(p->runtime);
    p->deadline = now_seconds() + .2;
    JSContext *ctx = p->context;
    JSValue fn = global_function(ctx, "__query");
    JSValue args[4] = {JS_NewFloat64(ctx, begin), JS_NewFloat64(ctx, end),JS_NewFloat64(ctx,cps),JS_NewBool(ctx,probes)};
    JSValue value = JS_Call(ctx, fn, JS_UNDEFINED, 4, args);
    JS_FreeValue(ctx, fn);
    JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx,args[2]); JS_FreeValue(ctx,args[3]);
    cJSON *json = NULL;
    if (JS_IsException(value)) {
        char error[512];
        JSValue exception = JS_GetException(ctx);
        exception_text(ctx, exception, error, sizeof(error));
        JS_FreeValue(ctx, exception);
        log_console("JavaScript query: %s", error);
    } else json = json_value(ctx, value);
    JS_FreeValue(ctx, value);
    pthread_mutex_unlock(&p->lock);
    return json;
}

cJSON *javascript_query_cps(JsProgram *p, double begin, double end, double cps) {
    return query_events(p,begin,end,cps,0);
}

cJSON *javascript_query_audio(JsProgram *p, double begin, double end, double cps) {
    return query_events(p,begin,end,cps,1);
}

cJSON *javascript_query(JsProgram *p, double begin, double end) {
    return javascript_query_cps(p,begin,end,NAN);
}

static int utf8_offset(const char *s, int units) {
    int bytes = 0, at = 0;
    while (s[bytes] && at < units) {
        unsigned char c = (unsigned char)s[bytes];
        int n = c < 128 ? 1 : (c < 224 ? 2 : (c < 240 ? 3 : 4));
        for (int i = 1; i < n; i++) if (!s[bytes + i]) { n = i; break; }
        bytes += n; at += n == 4 ? 2 : 1;
    }
    return bytes;
}

void javascript_cache_spans(JsProgram *p, const cJSON *haps, double origin, double seconds) {
    if (!p) return;
    pthread_mutex_lock(&p->spans_lock);
    const cJSON *hap;
    cJSON_ArrayForEach(hap, haps) {
        const cJSON *a = cJSON_GetObjectItemCaseSensitive(hap, "begin");
        const cJSON *b = cJSON_GetObjectItemCaseSensitive(hap, "end");
        if (!cJSON_IsNumber(a) || !cJSON_IsNumber(b)) continue;
        VisualNote style;
        int styled=visual_note(&style,hap,origin,seconds);
        const cJSON *loc;
        cJSON_ArrayForEach(loc, cJSON_GetObjectItemCaseSensitive(hap, "locations")) {
            const cJSON *lo = cJSON_GetObjectItemCaseSensitive(loc, "start");
            const cJSON *hi = cJSON_GetObjectItemCaseSensitive(loc, "end");
            if (!cJSON_IsNumber(lo) || !cJSON_IsNumber(hi) || lo->valuedouble < 0 ||
                hi->valuedouble <= lo->valuedouble || hi->valuedouble > (double)INT_MAX) continue;
            unsigned i = p->head++ % JS_SPANS;
            p->spans[i].begin = origin + a->valuedouble * seconds;
            p->spans[i].end = origin + b->valuedouble * seconds;
            p->spans[i].off = utf8_offset(p->source, lo->valueint);
            p->spans[i].len = utf8_offset(p->source, hi->valueint) - p->spans[i].off;
            p->spans[i].color=styled?style.color:0;
            p->spans[i].flags=styled?style.flags:0;
        }
    }
    pthread_mutex_unlock(&p->spans_lock);
}

int javascript_play_spans(const JsProgram *p, double now, int *off, int *len, int max) {
    return javascript_play_styles(p,now,off,len,NULL,NULL,max);
}

int javascript_play_styles(const JsProgram *p, double now, int *off, int *len,
                           uint32_t *color, unsigned *flags, int max) {
    return javascript_editor_play_styles(p,NULL,now,off,len,color,flags,max);
}

int javascript_editor_play_styles(const JsProgram *p, const char *editor, double now, int *off, int *len,
                                  uint32_t *color, unsigned *flags, int max) {
    int n = 0;
    if (!p) return 0;
    pthread_mutex_lock((pthread_mutex_t *)&p->spans_lock);
    int identity=!editor || visual_source_matches(p->source,editor);
    int mapped=editor && visual_source_matches(p->visual_source,editor);
    if (!identity && !mapped) { pthread_mutex_unlock((pthread_mutex_t *)&p->spans_lock); return 0; }
    for (int i = 0; i < JS_SPANS && n < max; i++) {
        if (p->spans[i].len <= 0 || now < p->spans[i].begin || now >= p->spans[i].end) continue;
        int at=p->spans[i].off, length=p->spans[i].len;
        if (!identity) {
            int found=0;
            for (int j=0;j<p->nhighlight_map;j++) {
                const struct HighlightRange *range=&p->highlight_map[j];
                if (at>=range->begin && at<=range->end && length<=range->end-at) {
                    at=range->target+(at-range->begin); found=1; break;
                }
            }
            if (!found) continue;
        }
        off[n] = at; len[n] = length;
        if (color) color[n]=p->spans[i].color;
        if (flags) flags[n]=p->spans[i].flags;
        n++;
    }
    pthread_mutex_unlock((pthread_mutex_t *)&p->spans_lock);
    return n;
}

int javascript_matches(const JsProgram *p, const char *code) {
    return p && code && !strcmp(p->source,code);
}

unsigned javascript_scope_mask(const JsProgram *p, int offset, int kind) {
    if (!p) return 0;
    unsigned mask = 0;
    pthread_mutex_lock((pthread_mutex_t *)&p->spans_lock);
    for (int i = 0; i < p->nvisual_sites; i++)
        if (p->visual_sites[i].offset == offset && p->visual_sites[i].kind == kind) mask = p->visual_sites[i].mask;
    pthread_mutex_unlock((pthread_mutex_t *)&p->spans_lock);
    return mask;
}

void javascript_request_visuals(JsProgram *p, const char *source) {
    if (!p || !source) return;
    pthread_mutex_lock(&p->spans_lock);
    if (!p->visual_requested || strcmp(p->visual_requested,source)) {
        char *requested=strdup_safe(source), *pending=strdup_safe(source);
        if (requested && pending) {
            free(p->visual_requested); free(p->visual_pending);
            p->visual_requested=requested; p->visual_pending=pending;
        } else { free(requested); free(pending); }
    }
    pthread_mutex_unlock(&p->spans_lock);
}

/* editor_flatten appends a display-only final newline. */
static int visual_source_matches(const char *compiled, const char *editor) {
    if (!compiled || !editor) return 0;
    size_t a=strlen(compiled), b=strlen(editor);
    return (a==b && !memcmp(compiled,editor,a)) || (b==a+1 && editor[a]=='\n' && !memcmp(compiled,editor,a));
}

unsigned javascript_visual_revision(JsProgram *p) {
    if (!p) return 0;
    pthread_mutex_lock(&p->spans_lock);
    unsigned revision=p->visual_revision;
    pthread_mutex_unlock(&p->spans_lock);
    return revision;
}

unsigned javascript_live_scope_mask(JsProgram *p, const char *source, int offset, int kind) {
    if (!p || !source || offset<0) return 0;
    unsigned mask=0;
    pthread_mutex_lock(&p->spans_lock);
    if (visual_source_matches(p->visual_source,source))
        for (int i=0;i<p->nvisual_sites;i++)
            if (p->visual_sites[i].offset==offset && p->visual_sites[i].kind==kind) mask=p->visual_sites[i].mask;
    pthread_mutex_unlock(&p->spans_lock);
    return mask;
}

void javascript_warn_control(JsProgram *p, const char *name) {
    if (!p || !name || p->nwarned >= 32) return;
    for (int i = 0; i < p->nwarned; i++) if (!strcmp(p->warned[i],name)) return;
    snprintf(p->warned[p->nwarned++],64,"%.63s",name);
    log_console("native audio: control '%s' is not implemented",name);
}

int javascript_visual_options(const JsProgram *p, unsigned mask, VisualOptions *out) {
    return javascript_live_visual_options(p,mask,0,out);
}

int javascript_live_visual_options(const JsProgram *p, unsigned mask, int kind, VisualOptions *out) {
    if (!p || !out) return 0;
    int found=0;
    pthread_mutex_lock((pthread_mutex_t *)&p->spans_lock);
    for (int i = 0; i < p->nvisual_sites; i++) if (p->visual_sites[i].mask == mask && (!kind || p->visual_sites[i].kind==kind)) {
        *out = p->visual_sites[i].options; found=1; break;
    }
    pthread_mutex_unlock((pthread_mutex_t *)&p->spans_lock);
    return found;
}

/* Generator only. The UI queues text and reads the resulting metadata. */
static void configure_visuals(JsProgram *p) {
    pthread_mutex_lock(&p->spans_lock);
    char *source=p->visual_pending; p->visual_pending=NULL;
    pthread_mutex_unlock(&p->spans_lock);
    if (!source) return;
    pthread_mutex_lock(&p->lock);
    JS_UpdateStackTop(p->runtime); p->deadline=now_seconds()+.2;
    JSContext *ctx=p->context;
    JSValue fn=global_function(ctx,"__configureVisuals"), arg=JS_NewString(ctx,source);
    JSValue value=JS_Call(ctx,fn,JS_UNDEFINED,1,&arg);
    JS_FreeValue(ctx,fn); JS_FreeValue(ctx,arg);
    cJSON *json=NULL;
    if (JS_IsException(value)) JS_FreeValue(ctx,JS_GetException(ctx));
    else json=json_value(ctx,value);
    JS_FreeValue(ctx,value);
    pthread_mutex_unlock(&p->lock);
    const cJSON *sites=cJSON_GetObjectItemCaseSensitive(json,"sites");
    if (!cJSON_IsArray(sites)) { cJSON_Delete(json); free(source); return; }
    pthread_mutex_lock(&p->spans_lock);
    free(p->highlight_map); p->highlight_map=NULL; p->nhighlight_map=0;
    const cJSON *map=cJSON_GetObjectItemCaseSensitive(json,"highlightMap");
    int ranges=cJSON_IsArray(map) ? cJSON_GetArraySize(map) : 0;
    if (ranges>0 && ranges<=8192) p->highlight_map=calloc((size_t)ranges,sizeof(*p->highlight_map));
    if (p->highlight_map) {
        const cJSON *range;
        size_t original_length=strlen(p->source), editor_length=strlen(source);
        cJSON_ArrayForEach(range,map) {
            if (!cJSON_IsArray(range) || cJSON_GetArraySize(range)!=4) continue;
            int units[4], valid=1;
            for (int i=0;i<4;i++) {
                const cJSON *value=cJSON_GetArrayItem(range,i);
                double limit=(double)(i<2 ? original_length : editor_length);
                if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) || value->valuedouble<0 ||
                    value->valuedouble>limit || value->valuedouble>INT_MAX || floor(value->valuedouble)!=value->valuedouble) { valid=0; break; }
                units[i]=value->valueint;
            }
            if (!valid || units[0]>=units[1] || units[2]>=units[3]) continue;
            int begin=utf8_offset(p->source,units[0]), end=utf8_offset(p->source,units[1]);
            int target=utf8_offset(source,units[2]), finish=utf8_offset(source,units[3]);
            if (end-begin!=finish-target || memcmp(p->source+begin,source+target,(size_t)(end-begin))) continue;
            struct HighlightRange *out=&p->highlight_map[p->nhighlight_map++];
            out->begin=begin; out->end=end; out->target=target;
        }
    }
    if (!p->visual_notes) p->visual_notes=calloc(VISUAL_CACHE_NOTES,sizeof(*p->visual_notes));
    if (p->visual_notes) {
        p->nvisual_sites=p->nvisual_notes=0; p->visual_next=0;
        const cJSON *site;
        cJSON_ArrayForEach(site,sites) {
            const cJSON *offset=cJSON_GetObjectItemCaseSensitive(site,"offset"), *id=cJSON_GetObjectItemCaseSensitive(site,"id");
            const cJSON *kind=cJSON_GetObjectItemCaseSensitive(site,"kind"), *mask=cJSON_GetObjectItemCaseSensitive(site,"mask");
            if (p->nvisual_sites==32 || !cJSON_IsNumber(offset) || !cJSON_IsNumber(id) || !cJSON_IsNumber(kind) || !cJSON_IsNumber(mask) ||
                !isfinite(offset->valuedouble) || offset->valuedouble<0 || offset->valuedouble>(double)strlen(source) || floor(offset->valuedouble)!=offset->valuedouble ||
                !isfinite(id->valuedouble) || id->valuedouble<0 || id->valuedouble>=32 || floor(id->valuedouble)!=id->valuedouble ||
                !isfinite(kind->valuedouble) || kind->valuedouble<1 || kind->valuedouble>VISUAL_KINDS || floor(kind->valuedouble)!=kind->valuedouble ||
                !isfinite(mask->valuedouble) || mask->valuedouble<1 || mask->valuedouble>UINT32_MAX || floor(mask->valuedouble)!=mask->valuedouble) continue;
            int i=p->nvisual_sites++;
            p->visual_sites[i].offset=utf8_offset(source,offset->valueint);
            p->visual_sites[i].id=id->valueint; p->visual_sites[i].kind=kind->valueint;
            p->visual_sites[i].mask=(unsigned)mask->valuedouble;
            visual_options(&p->visual_sites[i].options,kind->valueint,cJSON_GetObjectItemCaseSensitive(site,"options"));
        }
        free(p->visual_source); p->visual_source=source; source=NULL;
        if (++p->visual_revision==0) p->visual_revision=1;
    }
    pthread_mutex_unlock(&p->spans_lock);
    cJSON_Delete(json); free(source);
}

void javascript_update_visuals(JsProgram *p, double now, double origin, double seconds) {
    if (!p) return;
    configure_visuals(p);
    if (!p->nvisual_sites || !isfinite(now) || !isfinite(origin) || !isfinite(seconds) || seconds <= 0) return;
    pthread_mutex_lock(&p->lock);
    double wall = now_seconds();
    if (wall < p->visual_next) { pthread_mutex_unlock(&p->lock); return; }
    p->visual_next = wall+.125;
    JS_UpdateStackTop(p->runtime); p->deadline = wall+.05;
    JSContext *ctx = p->context;
    JSValue fn = global_function(ctx,"__visualQuery");
    JSValue args[2] = {JS_NewFloat64(ctx,(now-origin)/seconds),JS_NewFloat64(ctx,1/seconds)};
    JSValue value = JS_Call(ctx,fn,JS_UNDEFINED,2,args);
    JS_FreeValue(ctx,fn); JS_FreeValue(ctx,args[0]); JS_FreeValue(ctx,args[1]);
    cJSON *json = NULL;
    if (JS_IsException(value)) {
        JSValue exception = JS_GetException(ctx);
        if (!p->visual_warned) {
            char error[256]; exception_text(ctx,exception,error,sizeof(error));
            log_console("Visual feedback query failed: %s",error); p->visual_warned = 1;
        }
        JS_FreeValue(ctx,exception);
    } else json = json_value(ctx,value);
    JS_FreeValue(ctx,value); pthread_mutex_unlock(&p->lock);
    if (!cJSON_IsArray(json)) { cJSON_Delete(json); return; }
    pthread_mutex_lock(&p->spans_lock);
    p->nvisual_notes = 0; p->visual_origin = origin; p->visual_seconds = seconds;
    const cJSON *hap;
    cJSON_ArrayForEach(hap,json) {
        if (p->nvisual_notes == VISUAL_CACHE_NOTES) break;
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(hap,"visualId");
        if (!cJSON_IsNumber(id) || !isfinite(id->valuedouble) || id->valuedouble < 0 || id->valuedouble >= 32 ||
            floor(id->valuedouble) != id->valuedouble) continue;
        unsigned mask = 0; int kind=0;
        for (int i = 0; i < p->nvisual_sites; i++) if (p->visual_sites[i].id==id->valueint) {
            mask=p->visual_sites[i].mask; kind=p->visual_sites[i].kind;
        }
        struct VisualCachedNote *note = &p->visual_notes[p->nvisual_notes];
        if (mask && visual_note(&note->note,hap,origin,seconds)) { note->mask = mask; note->kind=kind; p->nvisual_notes++; }
    }
    pthread_mutex_unlock(&p->spans_lock);
    cJSON_Delete(json);
}

int javascript_visual_copy(JsProgram *p, unsigned mask, VisualNote *out, int max, double *origin, double *seconds) {
    return javascript_live_visual_copy(p,mask,0,out,max,origin,seconds);
}

int javascript_live_visual_copy(JsProgram *p, unsigned mask, int kind, VisualNote *out, int max, double *origin, double *seconds) {
    if (!p || !out || max < 1) return 0;
    int n = 0;
    pthread_mutex_lock(&p->spans_lock);
    if (origin) *origin = p->visual_origin;
    if (seconds) *seconds = p->visual_seconds > 0 ? p->visual_seconds : 2;
    for (int i = 0; i < p->nvisual_notes && n < max; i++)
        if ((p->visual_notes[i].mask&mask) && (!kind || p->visual_notes[i].kind==kind)) out[n++] = p->visual_notes[i].note;
    pthread_mutex_unlock(&p->spans_lock);
    return n;
}
