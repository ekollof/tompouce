#ifndef JAVASCRIPT_H
#define JAVASCRIPT_H
#include <cjson/cJSON.h>
#include "csound_host.h"
#include "visual.h"
/* Confined, no-overwrite optional starter installation. The store entry
   point also permits owned fixtures to test persistence without networking. */
int javascript_install_prebake(const char *provider, char *error, size_t error_size);
int javascript_store_prebake(const char *text, char *error, size_t error_size);

typedef struct JsProgram JsProgram;
int javascript_check(const char *code, char *error, int errorlen, int *line);
JsProgram *javascript_compile(const char *code, double bpm, cJSON **metadata, char *error, int errorlen);
void javascript_free(JsProgram *program);
void javascript_retain(JsProgram *program);
/* Publish controls only after a successful program load. Failed compiles keep
   the currently playing program's live values and UI bindings intact. */
void javascript_activate_sliders(JsProgram *program);
const char *javascript_source(const JsProgram *program);
/* Owned JSON arrays. Generator queries run outside tracks_lock. */
cJSON *javascript_query(JsProgram *program, double begin, double end);
cJSON *javascript_query_cps(JsProgram *program, double begin, double end, double cps);
cJSON *javascript_query_audio(JsProgram *program, double begin, double end, double cps);
/* UI requests are parsed by the generator, without evaluating musical code. */
void javascript_request_visuals(JsProgram *program, const char *source);
unsigned javascript_visual_revision(JsProgram *program);
unsigned javascript_live_scope_mask(JsProgram *program, const char *source, int offset, int kind);
void javascript_cache_spans(JsProgram *program, const cJSON *haps, double origin, double cycle_seconds);
void javascript_cache_hap(JsProgram *program, const cJSON *hap, double origin, double cycle_seconds);
int javascript_play_spans(const JsProgram *program, double now, int *off, int *len, int max);
int javascript_play_styles(const JsProgram *program, double now, int *off, int *len,
                           uint32_t *color, unsigned *flags, int max);
int javascript_editor_play_styles(const JsProgram *program, const char *editor, double now, int *off, int *len,
                                  uint32_t *color, unsigned *flags, int max);
int javascript_matches(const JsProgram *program, const char *code);
unsigned javascript_scope_mask(const JsProgram *program, int byte_offset, int kind);
int javascript_visual_options(const JsProgram *program, unsigned mask, VisualOptions *out);
int javascript_live_visual_options(const JsProgram *program, unsigned mask, int kind, VisualOptions *out);
void javascript_update_visuals(JsProgram *program, double now, double origin, double seconds);
int javascript_visual_copy(JsProgram *program, unsigned mask, VisualNote *out, int max,
                           double *origin, double *seconds);
int javascript_live_visual_copy(JsProgram *program, unsigned mask, int kind, VisualNote *out, int max,
                                double *origin, double *seconds);
void javascript_warn_control(JsProgram *program, const char *name);
int javascript_prepare_csound(JsProgram *program, const cJSON *metadata, char *error, size_t size);
int javascript_prepare_csound_from(JsProgram *program, const cJSON *metadata,
                                   JsProgram *previous, char *error, size_t size);
CsoundProgram *javascript_csound(JsProgram *program);
#endif
