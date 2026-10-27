#include "signal_wave.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

static int same(double got, double want) {
    if (got == want) return got != 0 || signbit(got) == signbit(want);
    double scale = fabs(want) > 1 ? fabs(want) : 1;
    return fabs(got - want) <= 1e-12 * scale;
}

static void check(int kind, double time, double seed, double want) {
    double got = 0;
    assert(!signal_wave_fill(&kind, &time, 1, seed, &got));
    if (!same(got, want)) {
        fprintf(stderr, "kind %d t %.17g seed %.17g got %.17g (%a) want %.17g (%a)\n",
                kind, time, seed, got, got, want, want);
        assert(same(got, want));
    }
}

int main(void) {
    check(SIGNAL_SAW, 0, 0, 0);
    check(SIGNAL_SAW, 0.2, 0, 0.2);
    check(SIGNAL_SAW, -0.2, 0, -0.2);
    check(SIGNAL_SAW, -1, 0, -0.0);
    check(SIGNAL_SAW, 1.5, 0, 0.5);
    check(SIGNAL_ISAW, -0.2, 0, 1.2);
    check(SIGNAL_ISAW, -1, 0, 1);
    check(SIGNAL_SINE2, 0.2, 0, 0.9510565162951535);
    check(SIGNAL_SINE2, 10.25, 0, 1);
    check(SIGNAL_SQUARE, 0.2, 0, 0);
    check(SIGNAL_SQUARE, -0.2, 0, -1);
    check(SIGNAL_SQUARE, -1, 0, -0.0);
    check(SIGNAL_SQUARE, 1.5, 0, 1);
    check(SIGNAL_PERLIN, 0, 0, 0);
    check(SIGNAL_PERLIN, 0.2, 0, 0.03009188220262528);
    check(SIGNAL_PERLIN, -0.2, 1, 0.48945028289794956);
    check(SIGNAL_PERLIN, 300, 1, 0.5195421651005745);
    check(SIGNAL_PERLIN, 7.125, -2, 0.0909359181733862);
    check(SIGNAL_BERLIN, 0.2, 0, 0.05195421651005745);
    check(SIGNAL_BERLIN, -0.2, -2, 0.4924035282805562);
    check(SIGNAL_RAND, 0.2, 1, 0.46175408735871315);
    check(SIGNAL_RAND, 10.25, -2, 0.3289676383137703);
    int bad = SIGNAL_WAVES;
    double unused = 0;
    assert(signal_wave_fill(&bad, &unused, 1, 0, &unused));
    puts("native signal waves ok");
    return 0;
}
