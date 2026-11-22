/* Minimal LV2 gain used by the confined-worker tests. No external plugin
   runtime or UI is required, so the same fixture works under ASan/UBSan. */
#include <lv2.h>
#include <stdlib.h>

typedef struct {
    const float *gain;
    const float *input;
    float *output;
} TestGain;

static LV2_Handle instantiate(const LV2_Descriptor *descriptor, double rate,
                              const char *bundle_path,
                              const LV2_Feature *const *features) {
    (void)descriptor; (void)rate; (void)bundle_path; (void)features;
    return calloc(1,sizeof(TestGain));
}

static void connect_port(LV2_Handle handle, uint32_t port, void *data) {
    TestGain *gain=handle;
    if (!gain) return;
    if (port==0) gain->gain=data;
    else if (port==1) gain->input=data;
    else if (port==2) gain->output=data;
}

static void run(LV2_Handle handle, uint32_t sample_count) {
    TestGain *state=handle;
    if (!state || !state->gain || !state->input || !state->output) return;
    float amount=*state->gain;
    for (uint32_t i=0;i<sample_count*2;i++)
        state->output[i]=state->input[i]*amount;
}

static void cleanup(LV2_Handle handle) { free(handle); }

static const LV2_Descriptor descriptor={
    "urn:tompouce:test:gain",
    instantiate,
    connect_port,
    NULL,
    run,
    NULL,
    cleanup,
    NULL
};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index) {
    return index==0?&descriptor:NULL;
}
