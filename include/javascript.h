#ifndef JAVASCRIPT_H
#define JAVASCRIPT_H
#include <cjson/cJSON.h>
#include "csound_host.h"
#include "visual.h"

typedef struct JsProgram JsProgram;
int javascript_is_code(const char *code);
int javascript_check(const char *code, char *error, int errorlen, int *line);
JsProgram *javascript_compile(const char *code, double bpm, cJSON **metadata, char *error, int errorlen);
void javascript_free(JsProgram *program);
void javascript_retain(JsProgram *program);
/* Owned JSON array. Called from the generator, with tracks_lock held. */
cJSON *javascript_query(JsProgram *program, double begin, double end);
cJSON *javascript_query_cps(JsProgram *program, double begin, double end, double cps);
void javascript_cache_spans(JsProgram *program, const cJSON *haps, double origin, double cycle_seconds);
int javascript_play_spans(const JsProgram *program, double now, int *off, int *len, int max);
int javascript_play_styles(const JsProgram *program, double now, int *off, int *len,
                           uint32_t *color, unsigned *flags, int max);
int javascript_matches(const JsProgram *program, const char *code);
unsigned javascript_scope_mask(const JsProgram *program, int byte_offset, int kind);
int javascript_visual_options(const JsProgram *program, unsigned mask, VisualOptions *out);
void javascript_update_visuals(JsProgram *program, double now, double origin, double seconds);
int javascript_visual_copy(JsProgram *program, unsigned mask, VisualNote *out, int max,
                           double *origin, double *seconds);
void javascript_warn_control(JsProgram *program, const char *name);
int javascript_prepare_csound(JsProgram *program, const cJSON *metadata, char *error, size_t size);
CsoundProgram *javascript_csound(JsProgram *program);
#endif
