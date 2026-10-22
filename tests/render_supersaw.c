/* Raw production supersaw plus event controls; no device or user state. */
#include "dsp.h"
#include "audio.h"
#include "js_audio.h"
#include <assert.h>
#include <stdio.h>

int main(int argc,char **argv) {
    assert(argc==3);
    FILE *file=fopen(argv[1],"rb"); assert(file);
    char text[8192]; size_t size=fread(text,1,sizeof(text)-1,file);
    assert(!ferror(file) && fgetc(file)==EOF); fclose(file); text[size]=0;
    cJSON *hap=cJSON_Parse(text); assert(hap);
    AudioEvent event; assert(js_audio_event(hap,0,1,&event));
    assert(event.inst==INST_SUPERSAW);
    DspState state; dsp_init(&state,&event.dsp);
    dsp_supersaw_init(&state,0xc0ffeeu);
    file=fopen(argv[2],"wb"); assert(file);
    /* A begin=0 worklet waits through the first render quantum. This raw
       source gate includes that silence explicitly; ADSR/scheduling are not
       under test here. The deployed synth's source gain is .3/sqrt(voices). */
    for (int frame=0;frame<44100+128;frame++) {
        float pcm[2]={0,0};
        if (frame>=128) {
            dsp_supersaw(&state,event.freq);
            pcm[0]=(float)(state.supersaw_l*.3);
            pcm[1]=(float)(state.supersaw_r*.3);
        }
        assert(fwrite(pcm,sizeof(float),2,file)==2);
    }
    assert(!fclose(file)); cJSON_Delete(hap);
    return 0;
}
