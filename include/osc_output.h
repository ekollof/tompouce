#ifndef OSC_OUTPUT_H
#define OSC_OUTPUT_H
#include <stdint.h>
#include <cjson/cJSON.h>
#define OSC_OUTPUT_BYTES 2048
typedef struct {
    int family;
    uint16_t port, size;
    unsigned char address[16], data[OSC_OUTPUT_BYTES];
} OscOutputMessage;
/* Prepare on the generator. Send on the consumer, never in audio. */
int osc_output_prepare(OscOutputMessage *message,const cJSON *value,double cycle,double cps,double duration);
int osc_output_send(const OscOutputMessage *message,double seconds_until_due);
void osc_output_shutdown(void);
#endif
