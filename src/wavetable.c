#include "wavetable.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define WT_HARMONICS 64

typedef struct { char name[32]; float data[WT_SIZE]; } WtTable;

static WtTable tables[WT_MAX];
static int n_tables = 0;

static void normalize(int idx) {
    float peak = 0.0f;
    for (int i = 0; i < WT_SIZE; i++) {
        float a = tables[idx].data[i] < 0 ? -tables[idx].data[i] : tables[idx].data[i];
        if (a > peak) peak = a;
    }
    if (peak < 1e-6f) peak = 1.0f;
    float g = 0.9f / peak;
    for (int i = 0; i < WT_SIZE; i++) tables[idx].data[i] *= g;
}

static int begin_table(const char *name) {
    if (n_tables >= WT_MAX) return -1;
    int idx = n_tables++;
    snprintf(tables[idx].name, sizeof(tables[idx].name), "%s", name);
    memset(tables[idx].data, 0, sizeof(tables[idx].data));
    return idx;
}

/* add sine partial k with amplitude a (phase 0) */
static void partial(int idx, int k, double a) {
    for (int i = 0; i < WT_SIZE; i++) {
        double ph = 2.0 * M_PI * k * i / WT_SIZE;
        tables[idx].data[i] += (float)(a * sin(ph));
    }
}

void wt_init(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    n_tables = 0;
    int t;

    t = begin_table("sine");
    partial(t, 1, 1.0);
    normalize(t);

    t = begin_table("tri");
    for (int k = 1; k <= WT_HARMONICS; k += 2) {
        int m = (k - 1) / 2;
        partial(t, k, ((m % 2) ? -1.0 : 1.0) / ((double)k * k));
    }
    normalize(t);

    t = begin_table("saw");
    for (int k = 1; k <= WT_HARMONICS; k++) partial(t, k, 1.0 / k);
    normalize(t);

    t = begin_table("square");
    for (int k = 1; k <= WT_HARMONICS; k += 2) partial(t, k, 1.0 / k);
    normalize(t);

    t = begin_table("organ");
    partial(t, 1, 1.0);
    partial(t, 2, 0.5);
    partial(t, 3, 0.25);
    partial(t, 4, 0.125);
    normalize(t);

    t = begin_table("acid");
    for (int k = 1; k <= WT_HARMONICS; k++) partial(t, k, 1.0 / pow(k, 0.8));
    normalize(t);

    t = begin_table("voice");
    for (int k = 1; k <= 24; k++) {
        double formant = exp(-pow(k - 3.0, 2.0) / 4.0) + 0.6 * exp(-pow(k - 7.0, 2.0) / 6.0);
        partial(t, k, (0.3 + formant) / k);
    }
    normalize(t);

    t = begin_table("pluck");
    for (int k = 1; k <= WT_HARMONICS; k++) partial(t, k, 1.0 / ((double)k * k));
    normalize(t);

    t = begin_table("pad");
    partial(t, 1, 1.0);
    partial(t, 2, 0.4);
    partial(t, 3, 0.3);
    partial(t, 4, 0.2);
    partial(t, 5, 0.12);
    partial(t, 6, 0.08);
    normalize(t);

    t = begin_table("glass");
    for (int k = 1; k <= WT_HARMONICS; k++) partial(t, k, 1.0 / sqrt((double)k));
    normalize(t);
}

int wt_count(void) { return n_tables; }

const char *wt_name(int idx) {
    if (idx < 0 || idx >= n_tables) return "?";
    return tables[idx].name;
}

int wt_find(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < n_tables; i++)
        if (strcmp(tables[i].name, name) == 0) return i;
    return -1;
}

double wt_sample(int idx, double phase) {
    if (idx < 0 || idx >= n_tables) return 0.0;
    phase -= floor(phase);
    double pos = phase * WT_SIZE;
    int i0 = (int)pos;
    int i1 = i0 + 1;
    if (i1 >= WT_SIZE) i1 = 0;
    double frac = pos - i0;
    double a = tables[idx].data[i0];
    double b = tables[idx].data[i1];
    return a + (b - a) * frac;
}
