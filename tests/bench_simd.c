/* Owned, device-free production-kernel benchmark. Not a timing acceptance test. */
#include "../src/audio.c"
#include "../src/convolution.c"
#include <assert.h>
#include <time.h>

static volatile double benchmark_sink;
static double cpu_seconds(void) {
    struct timespec t;
    assert(!clock_gettime(CLOCK_THREAD_CPUTIME_ID,&t));
    return t.tv_sec+t.tv_nsec*1e-9;
}

static void report(const char *name, int count, double cpu, double maximum,
                   double checksum) {
    assert(isfinite(checksum)); benchmark_sink=checksum;
    printf("%s,%d,%.9f,%.9f,%.12g\n",name,count,cpu,maximum,checksum);
}

static void callback_case(int count, int effects, int sample) {
    memset(voices,0,sizeof(voices)); audio_time=0; next_voice_onset=HUGE_VAL;
    audio_set_reference_mode(1); audio_toggle_play(1);
    for (int i=0;i<count;i++) {
        AudioEvent e={.duration=30,.inst=INST_SINE,.freq=110+i*3,
            .smp=sample,.smp_speed=1,.smp_gate=1,.wt=-1,.gain=.001,.att=.001,.dec=.01,.sus=1,.rel=.1,
            .midi_chan=-1,.midi_note=-1,.delay_prepared=1,.room_prepared=1};
        dsp_params_init(&e.dsp); e.dsp.enabled=1;
        if (sample>=0) { e.dsp.p[DSP_LOOP]=1; e.dsp.p[DSP_LOOPEND]=1; }
        if (effects) {
            e.dsp.p[DSP_LPF]=1800; e.dsp.p[DSP_LPQ]=1;
            e.dsp.p[DSP_PHASER]=.5; e.dsp.p[DSP_PHDEPTH]=.5;
            e.dsp.p[DSP_PHCENTER]=1000; e.dsp.p[DSP_PHSWEEP]=1000;
        }
        audio_trigger_event(&e);
    }
    float out[256]; double sum=0,maximum=0;
    for (int b=0;b<100;b++) pa_callback(NULL,out,128,NULL,0,NULL);
    double begin=cpu_seconds();
    for (int b=0;b<1500;b++) {
        double start=cpu_seconds(); pa_callback(NULL,out,128,NULL,0,NULL);
        double elapsed=cpu_seconds()-start; if (elapsed>maximum) maximum=elapsed;
        for (int i=0;i<256;i++) sum+=out[i];
    }
    assert(audio_active_voices()==count);
    report(sample>=0?"callback-sample":effects?"callback-filter-phaser":"callback-sine",count,
           cpu_seconds()-begin,maximum,sum);
    memset(voices,0,sizeof(voices));
}

static void convolution_case(int frames) {
    float *pcm=calloc((size_t)frames*2,sizeof(*pcm)); assert(pcm);
    for (int i=0;i<frames;i++) {
        pcm[i*2]=(float)(sin(i*.173)*exp(-6.0*i/frames)*.01);
        pcm[i*2+1]=(float)(cos(i*.137)*exp(-6.0*i/frames)*.01);
    }
    double begin=cpu_seconds();
    ConvKernel *k=convolution_kernel(pcm,frames,2,0); assert(k); free(pcm);
    report("kernel-prepare",frames,cpu_seconds()-begin,0,k->head[0][0]);
    ConvState *s=calloc(1,sizeof(*s)); assert(s); convolution_set(s,k);
    double l,r,sum=0,maximum=0;
    /* Fill history before timing; use a stationary nonzero input. */
    for (int i=0;i<frames+256;i++) convolution_process(s,.01,-.02,&l,&r);
    begin=cpu_seconds();
    for (int b=0;b<1000;b++) {
        double start=cpu_seconds();
        for (int i=0;i<128;i++) { convolution_process(s,.01,-.02,&l,&r); sum+=l+r; }
        double elapsed=cpu_seconds()-start; if (elapsed>maximum) maximum=elapsed;
    }
    report("convolution-full",frames,cpu_seconds()-begin,maximum,sum);
    /* Production tail block isolated from direct head and sample dispatch. */
    if (k->parts) {
        begin=cpu_seconds();
        for (int i=0;i<1000;i++) process_block(s);
        report("convolution-tail-blocks",frames,cpu_seconds()-begin,0,s->wet[0][0]);
    }
    free(s); convolution_kernel_free(k);
}

static void fft_case(void) {
    ConvComplex input[CONV_FFT],data[CONV_FFT]; double sum=0;
    for (int i=0;i<CONV_FFT;i++) input[i]=(ConvComplex){(float)sin(i*.1),0};
    double begin=cpu_seconds();
    for (int i=0;i<200000;i++) {
        memcpy(data,input,sizeof(data)); transform(data,0); transform(data,1);
        sum+=data[i%CONV_FFT].re;
    }
    report("fft-roundtrip",200000,cpu_seconds()-begin,0,sum);
}

static int owned_sample(void) {
    unsigned char header[44]={
        'R','I','F','F',0x34,0xb1,2,0,'W','A','V','E','f','m','t',' ',
        16,0,0,0,1,0,2,0,0x44,0xac,0,0,0x10,0xb1,2,0,4,0,16,0,
        'd','a','t','a',0x10,0xb1,2,0};
    char path[]="/tmp/tompouce-simd-sample-XXXXXX.wav";
    int fd=mkstemps(path,4); assert(fd>=0);
    FILE *f=fdopen(fd,"wb"); assert(f);
    assert(fwrite(header,1,sizeof(header),f)==sizeof(header));
    for (int i=0;i<44100;i++) {
        int16_t value=(int16_t)(sin(i*.1)*10000);
        unsigned char frame[4]={(unsigned char)value,(unsigned char)((uint16_t)value>>8),
            (unsigned char)value,(unsigned char)((uint16_t)value>>8)};
        assert(fwrite(frame,1,4,f)==4);
    }
    assert(!fclose(f)); int sample=smp_acquire_abs(path,60);
    assert(sample>=0); assert(!unlink(path)); return sample;
}

int main(int argc, char **argv) {
    log_console_stderr(0);
    convolution_simd_init();
    fprintf(stderr,"Convolution backend: %s\n",convolution_simd_name());
    puts("workload,size,cpu_seconds,max_block_cpu_seconds,checksum");
    if (argc==2 && !strcmp(argv[1],"--convolution")) {
        convolution_case(128); convolution_case(4410); convolution_case(44100);
        convolution_case(132300); return 0;
    }
    if (argc==2 && !strcmp(argv[1],"--samples")) {
        int sample=owned_sample();
        callback_case(1,0,sample); callback_case(32,0,sample); callback_case(128,0,sample);
        return 0;
    }
    callback_case(1,0,-1); callback_case(32,0,-1); callback_case(128,0,-1);
    callback_case(32,1,-1); callback_case(128,1,-1);
    convolution_case(128); convolution_case(4410); convolution_case(44100);
    convolution_case(132300); fft_case();
    return 0;
}
