/* Offline templates must remain playable in the sole JavaScript language. */
#include "../src/ai.c"
#include "javascript.h"
#include <assert.h>

int main(void) {
    log_console_stderr(1);
    const char *prompts[]={"techno","ambient","happy","sad","funk","anything"};
    for (unsigned i=0;i<sizeof(prompts)/sizeof(prompts[0]);i++) {
        char *code=NULL; offline_generate(prompts[i],&code); assert(code);
        char error[512]; cJSON *metadata=NULL;
        JsProgram *program=javascript_compile(code,120,&metadata,error,sizeof(error));
        assert(program && metadata);
        cJSON *haps=javascript_query(program,0,1);
        if (!cJSON_IsArray(haps) || !cJSON_GetArraySize(haps))
            fprintf(stderr,"No offline AI events for %s: %s\n",prompts[i],code);
        assert(cJSON_IsArray(haps) && cJSON_GetArraySize(haps)>0);
        cJSON_Delete(haps); cJSON_Delete(metadata); javascript_free(program); free(code);
    }
    puts("All offline AI templates produce JavaScript Strudel events");
    return 0;
}
