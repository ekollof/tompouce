#include "javascript.h"
#include "common.h"
#include "slider.h"
#include "midi.h"
#include "websamp.h"
#include <quickjs.h>
#include <ctype.h>
#include <limits.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/random.h>
#include <sys/stat.h>

#include "runtime.inc"

#define JS_MEMORY_LIMIT (128 * 1024 * 1024)
#define JS_TEXT_LIMIT (4 * 1024 * 1024)
#define JS_SPANS 2048

struct JsProgram {
    SliderBank *sliders;
    CsoundProgram *csound;
    char *csound_source;
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
    unsigned prebake_files;
    size_t prebake_bytes;
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

static int utf8_offset(const char *s, int units);

static JSValue host_slider_bind(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    double v[4]={0,0,1,.05};
    for (int i=0;i<argc && i<4;i++)
        if (JS_ToFloat64(ctx,&v[i],argv[i])<0 || !isfinite(v[i]))
            return JS_ThrowTypeError(ctx,"slider arguments must be finite numbers");
    JsProgram *program=JS_GetContextOpaque(ctx);
    int offset=-1;
    if (argc>4) {
        const char *id=JS_ToCString(ctx,argv[4]);
        if (id && !strncmp(id,"slider_",7)) {
            char *end; long position=strtol(id+7,&end,10);
            if (!*end && position>=0 && position<=INT_MAX) offset=utf8_offset(program->source,(int)position);
        }
        if (id) JS_FreeCString(ctx,id);
    }
    int index=slider_bank_bind_site(program->sliders,offset,v[0],v[1],v[2],v[3]);
    if (index<0) return JS_ThrowRangeError(ctx,"too many sliders");
    return JS_NewInt32(ctx,index);
}

static JSValue host_slider_read(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self; int index; double value;
    JsProgram *program=JS_GetContextOpaque(ctx);
    if (!argc || JS_ToInt32(ctx,&index,argv[0])<0 || !slider_bank_read(program->sliders,index,&value))
        return JS_ThrowRangeError(ctx,"invalid slider binding");
    return JS_NewFloat64(ctx,value);
}

/* MIDI controller input reads. The port argument only selects the
   subscription; values merge across senders like a single controller. */
static JSValue host_midi_open(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    if (!argc) return JS_NewInt32(ctx,0);
    if (JS_IsNumber(argv[0])) {
        int32_t ordinal;
        if (JS_ToInt32(ctx,&ordinal,argv[0])<0) return JS_ThrowTypeError(ctx,"midi input index must be an integer");
        return JS_NewInt32(ctx,midi_open_input(NULL,ordinal));
    }
    const char *name=JS_ToCString(ctx,argv[0]);
    if (!name) return JS_ThrowTypeError(ctx,"midi input must be a device name, index or empty");
    int open=name[0]?midi_open_input(name,-1):midi_open_input(NULL,0);
    JS_FreeCString(ctx,name);
    return JS_NewInt32(ctx,open);
}

static JSValue host_midi_read(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    int32_t cc=0,chan=0;
    if (argc>0 && JS_ToInt32(ctx,&cc,argv[0])<0) return JS_ThrowTypeError(ctx,"midi cc must be an integer");
    if (argc>1 && !JS_IsUndefined(argv[1]) && JS_ToInt32(ctx,&chan,argv[1])<0)
        return JS_ThrowTypeError(ctx,"midi channel must be an integer");
    return JS_NewFloat64(ctx,midi_cc_value(cc,chan));
}

static JSValue host_midi_keys(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    double horizon=1e12;
    if (argc>0) {
        if (JS_ToFloat64(ctx,&horizon,argv[0])<0) return JS_ThrowTypeError(ctx,"midi key horizon must be a number");
        if (!isfinite(horizon)) horizon=1e12;
    }
    MidiHeldKey keys[64];
    unsigned count=horizon<0 ? midi_peek_keys(keys,64) : midi_take_keys(keys,64,horizon);
    JSValue result=JS_NewArray(ctx);
    if (JS_IsException(result)) return result;
    for (unsigned i=0;i<count;i++) {
        JSValue entry=JS_NewObject(ctx);
        if (JS_IsException(entry)) {
            JS_FreeValue(ctx,result);
            return JS_ThrowOutOfMemory(ctx);
        }
        if (JS_SetPropertyStr(ctx,entry,"note",JS_NewInt32(ctx,keys[i].note))<0 ||
            JS_SetPropertyStr(ctx,entry,"velocity",JS_NewFloat64(ctx,keys[i].vel))<0 ||
            JS_SetPropertyStr(ctx,entry,"midichan",JS_NewInt32(ctx,keys[i].chan))<0 ||
            JS_SetPropertyStr(ctx,entry,"at",JS_NewFloat64(ctx,keys[i].at))<0) {
            JS_FreeValue(ctx,entry); JS_FreeValue(ctx,result);
            return JS_ThrowOutOfMemory(ctx);
        }
        /* The final call consumes entry in all paths. */
        if (JS_SetPropertyUint32(ctx,result,i,entry)<0) {
            JS_FreeValue(ctx,result);
            return JS_ThrowOutOfMemory(ctx);
        }
    }
    return result;
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

/* The application root and every child are opened without following links.
   Optional applies only to absence, never unreadable/non-regular inputs. */
static char *config_script_source(JSContext *ctx, const char *name, int optional, size_t *size) {
    char root[1024], path[1400];
    user_dir(root,sizeof(root),"XDG_CONFIG_HOME","tompouce",".config/tompouce");
    int n = snprintf(path,sizeof(path),"%s/%s",root,name);
    if (!*root) {
        if (!optional) JS_ThrowReferenceError(ctx,"configuration root unavailable");
        return NULL;
    }
    if (n < 0 || (size_t)n >= sizeof(path)) {
        JS_ThrowReferenceError(ctx,"unsafe script path"); return NULL;
    }
    int dir = open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    char *save = NULL, *component = strtok_r(path,"/",&save);
    while (dir >= 0 && component) {
        char *next = strtok_r(NULL,"/",&save);
        int fd = openat(dir,component,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|(next ? O_DIRECTORY : O_NONBLOCK));
        int saved_errno=errno;
        close(dir); dir = fd; component = next;
        if (dir<0 && optional && saved_errno==ENOENT) return NULL;
    }
    struct stat st;
    if (dir < 0 || fstat(dir,&st) || !S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > 256*1024) {
        if (dir >= 0) close(dir);
        JS_ThrowReferenceError(ctx,"configuration script missing, linked, non-regular, or larger than 256 KiB"); return NULL;
    }
    *size = (size_t)st.st_size;
    char *text = malloc(*size+1);
    if (!text) { close(dir); JS_ThrowOutOfMemory(ctx); return NULL; }
    size_t used = 0;
    while (used < *size) {
        ssize_t got = read(dir,text+used,*size-used);
        if (got<0 && errno==EINTR) continue;
        if (got <= 0) { free(text); close(dir); JS_ThrowReferenceError(ctx,"module read failed"); return NULL; }
        used += (size_t)got;
    }
    close(dir); text[used] = 0;
    return text;
}

static char *local_module_source(JSContext *ctx, const char *name, size_t *size) {
    if (!local_name_safe(name)) { JS_ThrowReferenceError(ctx,"unsafe module path"); return NULL; }
    char path[256];
    int n=snprintf(path,sizeof(path),"modules/%s",name);
    if (n<0 || (size_t)n>=sizeof(path)) { JS_ThrowReferenceError(ctx,"unsafe module path"); return NULL; }
    return config_script_source(ctx,path,0,size);
}

static int prebake_name_safe(const char *name) {
    if (!name || !*name || strlen(name)>240 || *name=='/' || strstr(name,"..")) return 0;
    for (const unsigned char *p=(const unsigned char *)name;*p;p++)
        if (!(isalnum(*p) || *p=='/' || *p=='_' || *p=='-' || *p=='.')) return 0;
    size_t n=strlen(name);
    return local_name_safe(name) || (n>8 && !strcmp(name+n-8,".strudel"));
}

static JSValue host_prebake(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    JsProgram *p=JS_GetContextOpaque(ctx);
    if (!p->loading_allowed || p->prebake_files>=16)
        return JS_ThrowReferenceError(ctx,"load at most 16 prebakes during evaluation, before pattern queries");
    if (argc!=1 || !JS_IsString(argv[0]))return JS_ThrowTypeError(ctx,"prebake requires a file name");
    size_t length; const char *name=JS_ToCStringLen(ctx,&length,argv[0]);
    if (!name)return JS_EXCEPTION;
    int safe=strlen(name)==length && prebake_name_safe(name);
    char path[256]; int n=snprintf(path,sizeof(path),"prebakes/%s",name);
    JS_FreeCString(ctx,name);
    if (!safe || n<0 || (size_t)n>=sizeof(path))return JS_ThrowReferenceError(ctx,"unsafe prebake name");
    p->prebake_files++;
    size_t size=0; char *text=config_script_source(ctx,path,0,&size);
    if (!text)return JS_EXCEPTION;
    if (size>1024*1024-p->prebake_bytes) {
        free(text);return JS_ThrowRangeError(ctx,"prebakes exceed 1 MiB total");
    }
    p->prebake_bytes+=size;
    JSValue result=JS_NewStringLen(ctx,text,size); free(text); return result;
}

/* Explicit opt-in fetch through the existing connection-address policy.
   Pin upstream instead of distributing its unlicensed source in this tree. */
int javascript_install_prebake(const char *provider, char *error, size_t error_size) {
    if (!provider || strcmp(provider,"switchangel")) {
        snprintf(error,error_size,"unknown prebake starter (available: switchangel)"); return 0;
    }
    const char *url="https://raw.githubusercontent.com/switchangel/strudel-scripts/706dd24814ac86193ceec6d0b263f432cba6e5ff/prebake.strudel";
    char *text=websamp_fetch_text(url);
    if (!text || strlen(text)>256*1024-256) {
        free(text);snprintf(error,error_size,"prebake download failed or exceeds 256 KiB");return 0;
    }
    int result=javascript_store_prebake(text,error,error_size);
    free(text); return result;
}

int javascript_store_prebake(const char *text, char *error, size_t error_size) {
    const char *header="// tompouce-prebake: switchangel\n// Switch Angel / strudel-scripts, revision 706dd24814ac86193ceec6d0b263f432cba6e5ff\n";
    if (!text || strlen(text)>256*1024-strlen(header)) {
        snprintf(error,error_size,"prebake exceeds 256 KiB");return 0;
    }
    char root[1024]; user_dir(root,sizeof(root),"XDG_CONFIG_HOME","tompouce/prebakes",".config/tompouce/prebakes");
    if (!*root || strstr(root,"/../") || strstr(root,"/./")) {
        snprintf(error,error_size,"invalid prebake storage root");return 0;
    }
    int dir=open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC), ok=0;
    char *save=NULL;
    for (char *part=strtok_r(root,"/",&save);dir>=0 && part;part=strtok_r(NULL,"/",&save)) {
        if (mkdirat(dir,part,0700) && errno!=EEXIST) { close(dir);dir=-1;break; }
        int next=openat(dir,part,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
        close(dir);dir=next;
    }
    uint64_t nonce; char temporary[64]="";
    int fd=-1;
    if (dir>=0 && getrandom(&nonce,sizeof(nonce),0)==sizeof(nonce)) {
        snprintf(temporary,sizeof(temporary),".prebake-%llx",(unsigned long long)nonce);
        fd=openat(dir,temporary,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    }
    if (fd>=0) {
        const char *parts[]={header,text}; ok=1;
        for (int i=0;i<2 && ok;i++) {
            size_t size=strlen(parts[i]),used=0;
            while (used<size) {
                ssize_t n=write(fd,parts[i]+used,size-used);
                if (n<0 && errno==EINTR)continue;
                if (n<=0) { ok=0;break; } used+=(size_t)n;
            }
        }
        if (fsync(fd))ok=0;
        if (close(fd))ok=0;
        if (ok && linkat(dir,temporary,dir,"switchangel.strudel",0))ok=0;
        if (ok && fsync(dir))log_console("prebake installed; directory sync failed");
    }
    if (dir>=0) {
        if (fd>=0)unlinkat(dir,temporary,0);
        close(dir);
    }
    if (!ok)snprintf(error,error_size,"cannot store starter (existing file, unsafe directory, or write failure)");
    return ok;
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
    free(p->csound_source);
    if (p->context) JS_FreeContext(p->context);
    if (p->runtime) JS_FreeRuntime(p->runtime);
    free(p->source);
    slider_bank_free(p->sliders);
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
    return javascript_prepare_csound_from(p,metadata,NULL,error,size);
}
/* Session Csound engine: the deployed page keeps one engine across
   evaluations, so loads that succeed before a later failure stay available
   for the next evaluation. A new program shares the surviving previous
   engine; when none exists, the stashed engine is adopted instead of
   discarding partial loads with the failed evaluation. */
static CsoundProgram *stashed_csound;
static pthread_mutex_t stashed_csound_lock = PTHREAD_MUTEX_INITIALIZER;
static void park_csound(JsProgram *p) {
    if (!p || !p->csound) return;
    pthread_mutex_lock(&stashed_csound_lock);
    if (stashed_csound!=p->csound) {
        csound_program_free(stashed_csound);
        stashed_csound=p->csound; csound_program_retain(stashed_csound);
    }
    pthread_mutex_unlock(&stashed_csound_lock);
}
int javascript_prepare_csound_from(JsProgram *p, const cJSON *metadata,
                                   JsProgram *previous, char *error, size_t size) {
    const cJSON *loads = cJSON_GetObjectItemCaseSensitive(metadata,"csound");
    if (cJSON_IsArray(loads) && cJSON_GetArraySize(loads)>16) {
        snprintf(error,size,"Csound: too many orchestra loads"); return 0;
    }
    if (!cJSON_IsArray(loads) && (!previous || !previous->csound)) {
        /* Evaluations without loads keep running without an engine, unless a
           failed evaluation parked one with surviving loads. */
        pthread_mutex_lock(&stashed_csound_lock);
        if (stashed_csound) {
            p->csound=stashed_csound; csound_program_retain(p->csound);
            pthread_mutex_unlock(&stashed_csound_lock);
        } else {
            pthread_mutex_unlock(&stashed_csound_lock);
            return 1;
        }
    }
    p->csound_source=cJSON_IsArray(loads)?cJSON_PrintUnformatted(loads):
        strdup_safe(previous&&previous->csound_source?previous->csound_source:"[]");
    if (!p->csound_source) return 0;
    if (previous && previous->csound) {
        p->csound=previous->csound; csound_program_retain(p->csound);
    } else {
        pthread_mutex_lock(&stashed_csound_lock);
        p->csound=stashed_csound; csound_program_retain(p->csound);
        pthread_mutex_unlock(&stashed_csound_lock);
    }
    if (!p->csound) {
        cJSON *empty=cJSON_CreateArray();
        if (!empty) return 0;
        p->csound=csound_program_create(empty,error,size); cJSON_Delete(empty);
        if (!p->csound) return 0;
    }
    /* Browser loaders apply sequentially to the same engine. Csound reports
       orchestra syntax errors through messages, while its JS Promise resolves;
       fetch/host failures still reject evaluation. Earlier successful loads
       remain available when a later load is rejected. */
    const cJSON *load;
    cJSON_ArrayForEach(load,loads) {
        cJSON *single=cJSON_CreateArray();
        if (!single || !cJSON_AddItemReferenceToArray(single,(cJSON *)load)) { cJSON_Delete(single); return 0; }
        CsoundUpdateResult result=csound_program_update(p->csound,single,error,size);
        cJSON_Delete(single);
        if (result==CSOUND_UPDATE_FAILED) { park_csound(p); return 0; }
        if (result==CSOUND_UPDATE_REJECTED) log_console("%s",error);
    }
    return 1;
}
CsoundProgram *javascript_csound(JsProgram *p) { return p ? p->csound : NULL; }
void javascript_activate_sliders(JsProgram *p) { if (p) slider_bank_activate(p->sliders); }
const char *javascript_source(const JsProgram *p) { return p?p->source:NULL; }

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
    JS_SetContextOpaque(ctx,p);
    JSValue global = JS_GetGlobalObject(ctx);
    int a = JS_SetPropertyStr(ctx, global, "__hostLog", JS_NewCFunction(ctx, host_log, "__hostLog", 1));
    int b = JS_SetPropertyStr(ctx, global, "__hostSlider", JS_NewCFunction(ctx, host_slider, "__hostSlider", 4));
    int c = JS_SetPropertyStr(ctx, global, "__hostSliderBind", JS_NewCFunction(ctx, host_slider_bind, "__hostSliderBind", 4));
    int d = JS_SetPropertyStr(ctx, global, "__hostSliderRead", JS_NewCFunction(ctx, host_slider_read, "__hostSliderRead", 1));
    int e = JS_SetPropertyStr(ctx, global, "__hostMidiOpen", JS_NewCFunction(ctx, host_midi_open, "__hostMidiOpen", 1));
    int f = JS_SetPropertyStr(ctx, global, "__hostMidiRead", JS_NewCFunction(ctx, host_midi_read, "__hostMidiRead", 2));
    int g = JS_SetPropertyStr(ctx, global, "__hostMidiKeys", JS_NewCFunction(ctx, host_midi_keys, "__hostMidiKeys", 1));
    int h = JS_SetPropertyStr(ctx, global, "__hostPrebake", JS_NewCFunction(ctx, host_prebake, "__hostPrebake", 1));
    JS_FreeValue(ctx, global);
    if (a < 0 || b < 0 || c < 0 || d < 0 || e < 0 || f < 0 || g < 0 || h < 0) return 0;
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
    p->sliders=slider_bank_create();
    if (!p->sliders) { free(p); return NULL; }
    if (pthread_mutex_init(&p->lock,NULL) != 0) { slider_bank_free(p->sliders); free(p); return NULL; }
    p->lock_ready = 1;
    if (pthread_mutex_init(&p->spans_lock,NULL) != 0) { javascript_free(p); return NULL; }
    p->lock_ready = 2;
    p->deadline = now_seconds() + 2.0;
    p->loading_allowed = 1;
    p->source = strdup_safe(code);
    p->visual_source = strdup_safe(code);
    if (!p->source || !p->visual_source || !initialize(p, error, n)) { javascript_free(p); return NULL; }
    JSContext *ctx = p->context;
    size_t prebake_length=0;
    char *prebake=config_script_source(ctx,"prebake.strudel",1,&prebake_length);
    p->prebake_bytes=prebake_length;
    if (!prebake && JS_HasException(ctx)) {
        JSValue failure=JS_GetException(ctx);
        exception_text(ctx,failure,error,n); JS_FreeValue(ctx,failure);
        javascript_free(p); return NULL;
    }
    JSValue fn = global_function(ctx, "__compile");
    JSValue args[3] = {JS_NewString(ctx,code),JS_NewFloat64(ctx,isfinite(bpm) ? bpm : 120),
        JS_NewStringLen(ctx,prebake?prebake:"",prebake_length)};
    free(prebake);
    JSValue value = JS_IsException(args[0]) || JS_IsException(args[1]) || JS_IsException(args[2]) ?
        JS_EXCEPTION : JS_Call(ctx, fn, JS_UNDEFINED, 3, args);
    JS_FreeValue(ctx, args[0]); JS_FreeValue(ctx,args[1]); JS_FreeValue(ctx,args[2]); JS_FreeValue(ctx, fn);
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
    double query_started=now_seconds();
    struct timespec cpu_started;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID,&cpu_started);
    p->deadline = query_started + .2;
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
        struct timespec cpu_ended;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID,&cpu_ended);
        double cpu=(cpu_ended.tv_sec-cpu_started.tv_sec)+(cpu_ended.tv_nsec-cpu_started.tv_nsec)*1e-9;
        log_console("JavaScript query: %s (arc %.17g..%.17g, cps %.9g, wall %.6fs, cpu %.6fs)",
            error,begin,end,cps,now_seconds()-query_started,cpu);
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

void javascript_cache_hap(JsProgram *p, const cJSON *hap, double origin, double seconds) {
    if (!hap) return;
    cJSON item=*hap; item.next=item.prev=NULL;
    cJSON array={.type=cJSON_Array,.child=&item};
    javascript_cache_spans(p,&array,origin,seconds);
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
