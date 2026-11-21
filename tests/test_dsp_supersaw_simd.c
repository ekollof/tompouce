/* Exactness of the supersaw unison kernel against its portable reference. */
#include "../src/dsp_supersaw.c"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static uint32_t random_state=0x9e3779b9u;
static double random_value(void) {
    random_state=random_state*1664525u+1013904223u;
    return (double)(random_state>>8)/16777216.0;
}

#if DSP_SUPERSAW_HAVE_AVX2
static void compare(unsigned voices, double frequency) {
    DspState reference={0}, vector={0};
    reference.supersaw_voices=vector.supersaw_voices=voices;
    for (unsigned i=0;i<voices;i++) {
        double phase=random_value();
        reference.saw_phase[i]=vector.saw_phase[i]=phase;
        double ratio;
        if (i%23==0) ratio=0;
        else if (i%29==0) ratio=NAN;
        else if (i%31==0) ratio=-1;
        else ratio=pow(2.0,(random_value()*4-2)/12);
        reference.saw_ratio[i]=vector.saw_ratio[i]=ratio;
    }
    for (int frame=0;frame<64;frame++) {
        double left[DSP_SUPERSAW_VOICES], right[DSP_SUPERSAW_VOICES];
        dsp_supersaw_values_c(&reference,frequency,left);
        dsp_supersaw_values_avx2(&vector,frequency,right);
        assert(!memcmp(left,right,sizeof(double)*voices));
        assert(!memcmp(reference.saw_phase,vector.saw_phase,
            sizeof(double)*voices));
    }
}

int main(void) {
    __builtin_cpu_init();
    if (!__builtin_cpu_supports("avx2")) {
        puts("supersaw SIMD skipped (no AVX2)");
        return 0;
    }
    const double frequencies[]={20,220,440,5000,20000,100000};
    for (unsigned voices=1;voices<=DSP_SUPERSAW_VOICES;voices++)
        for (unsigned i=0;i<sizeof(frequencies)/sizeof(*frequencies);i++)
            compare(voices,frequencies[i]);
    puts("supersaw SIMD exactness ok");
    return 0;
}
#else
int main(void) {
    puts("supersaw SIMD unavailable");
    return 0;
}
#endif
