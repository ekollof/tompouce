#ifndef CSOUND_HOST_H
#define CSOUND_HOST_H
#include "common.h"
#include <cjson/cJSON.h>
typedef struct CsoundProgram CsoundProgram;
CsoundProgram *csound_program_create(const cJSON *loads, char *error, size_t size);
typedef enum { CSOUND_UPDATE_FAILED, CSOUND_UPDATE_OK, CSOUND_UPDATE_REJECTED } CsoundUpdateResult;
CsoundUpdateResult csound_program_update(CsoundProgram *program, const cJSON *loads, char *error, size_t size);
void csound_program_retain(CsoundProgram *program);
void csound_program_free(CsoundProgram *program);
void csound_program_activate(CsoundProgram *program, double origin);
int csound_program_schedule(CsoundProgram *program, const cJSON *value, double time, double duration, double frequency);
void csound_program_sample(CsoundProgram *program, double *left, double *right);
int csound_worker_main(void);
#endif
