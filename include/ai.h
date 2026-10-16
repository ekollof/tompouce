#ifndef AI_H
#define AI_H
#include "common.h"
void ai_init(void);
void ai_shutdown(void);
/* async request; prompt user text. context_code may be NULL */
void ai_request_async(const char *prompt, const char *context_code);
/* returns malloc'd result string if ready (caller frees), else NULL */
char *ai_poll_result(void);
int ai_is_busy(void);
#endif