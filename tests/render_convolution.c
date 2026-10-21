/* Offline shared-IR comparison helper: no devices, network or user state. */
#include "convolution.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc,char **argv) {
    if (argc!=3 && (argc!=4 || strcmp(argv[3],"normalized"))) return 2;
    FILE *input=fopen(argv[1],"rb"); if (!input) return 2;
    if (fseek(input,0,SEEK_END)) { fclose(input); return 2; }
    long bytes=ftell(input);
    if (bytes<8 || bytes>(long)CONV_MAX_FRAMES*8 || bytes%8 || fseek(input,0,SEEK_SET)) {
        fclose(input); return 2;
    }
    float *pcm=malloc((size_t)bytes); if (!pcm) { fclose(input); return 2; }
    int ok=fread(pcm,1,(size_t)bytes,input)==(size_t)bytes && !ferror(input);
    if (fclose(input)) ok=0;
    int frames=(int)(bytes/8);
    ConvKernel *kernel=ok?convolution_kernel(pcm,frames,2,argc==4):NULL; free(pcm);
    if (!kernel) return 2;
    ConvState *state=calloc(1,sizeof(*state));
    FILE *output=state?fopen(argv[2],"wb"):NULL;
    if (!output) { free(state); convolution_kernel_free(kernel); return 2; }
    convolution_set(state,kernel);
    for (int i=0;i<frames+CONV_BLOCK*2 && ok;i++) {
        double l,r;
        convolution_process(state,i==0?1:0,i==0?-.5:0,&l,&r);
        float pair[2]={(float)l,(float)r};
        ok=fwrite(pair,sizeof(float),2,output)==2;
    }
    if (fclose(output)) ok=0;
    free(state); convolution_kernel_free(kernel);
    return ok?0:2;
}
