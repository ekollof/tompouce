#include "slider.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define SLIDER_MAX 64

typedef struct {
    int live;
    double init, min, max, step, value;
} Sld;

static Sld sl[SLIDER_MAX];
static int sl_n = 0;
static int sl_pass = 0;
static pthread_mutex_t slider_lock = PTHREAD_MUTEX_INITIALIZER;

void slider_begin(void) {
    pthread_mutex_lock(&slider_lock);
    sl_pass = 0;
    pthread_mutex_unlock(&slider_lock);
}

static const char *skip(const char *p) {
    while (p && (*p == ' ' || *p == '\t')) p++;
    return p ? p : "";
}

int slider_parse(const char **pp, double *init, double *mn, double *mx, double *st) {
    if (!pp || !*pp || !init || !mn || !mx || !st) return 0;
    const char *p = skip(*pp);
    if (strncmp(p, "slider", 6) != 0) return 0;
    p += 6;
    p = skip(p);
    if (*p != '(') return 0;
    p++;
    char *end = NULL;
    double a = strtod(p, &end);
    if (end == p || !isfinite(a)) return 0;
    p = skip(end);
    double b = 0, c = 1, d = 0;
    int nargs = 1;
    if (*p == ',') {
        p = skip(p + 1);
        b = strtod(p, &end);
        if (end == p || !isfinite(b)) return 0;
        p = skip(end);
        if (*p != ',') return 0;
        p = skip(p + 1);
        c = strtod(p, &end);
        if (end == p || !isfinite(c)) return 0;
        p = skip(end);
        nargs = 3;
        if (*p == ',') {
            p = skip(p + 1);
            d = strtod(p, &end);
            if (end == p || !isfinite(d)) return 0;
            p = skip(end);
            nargs = 4;
        }
    }
    if (*p != ')') return 0;
    p++;
    if (nargs < 3) { b = 0; c = 1; }
    if (c < b) { double t = b; b = c; c = t; }
    if (nargs < 4 || d <= 0) {
        d = (c - b) / 20.0;
        if (d < 1e-6) d = 1e-6;
    }
    *init = a;
    *mn = b;
    *mx = c;
    *st = d;
    *pp = p;
    return 1;
}

static double bind_locked(double init, double mn, double mx, double step) {
    if (!isfinite(init) || !isfinite(mn) || !isfinite(mx) || !isfinite(step))
        return init;
    if (mx < mn) { double t = mn; mn = mx; mx = t; }
    if (step <= 0) step = (mx - mn) / 20.0;
    if (step < 1e-9) step = 1e-9;
    if (sl_pass >= SLIDER_MAX) {
        if (init < mn) init = mn;
        if (init > mx) init = mx;
        return init;
    }
    Sld *s = &sl[sl_pass];
    int same = s->live && fabs(s->init - init) < 1e-9 && fabs(s->min - mn) < 1e-9
        && fabs(s->max - mx) < 1e-9 && fabs(s->step - step) < 1e-12;
    if (!same) {
        s->live = 1;
        s->init = init;
        s->value = init;
        s->min = mn;
        s->max = mx;
        s->step = step;
    }
    if (s->value < s->min) s->value = s->min;
    if (s->value > s->max) s->value = s->max;
    sl_pass++;
    sl_n = sl_pass;
    return s->value;
}

double slider_bind(double init, double mn, double mx, double step) {
    pthread_mutex_lock(&slider_lock);
    double value = bind_locked(init, mn, mx, step);
    pthread_mutex_unlock(&slider_lock);
    return value;
}

int slider_or_number(const char *tok, double *out) {
    if (!tok || !out) return 0;
    if (strncmp(tok, "slider", 6) == 0) {
        const char *p = tok;
        double init, mn, mx, st;
        if (!slider_parse(&p, &init, &mn, &mx, &st)) return 0;
        if (*p != 0) return 0;
        *out = slider_bind(init, mn, mx, st);
        return 1;
    }
    char *end = NULL;
    double v = strtod(tok, &end);
    if (end == tok || (end && *end) || !isfinite(v)) return 0;
    *out = v;
    return 1;
}

int slider_count(void) {
    pthread_mutex_lock(&slider_lock);
    int count = sl_n;
    pthread_mutex_unlock(&slider_lock);
    return count;
}

static int info_locked(int i, double *v, double *mn, double *mx, double *st) {
    if (i < 0 || i >= sl_n || !sl[i].live) return 0;
    if (v) *v = sl[i].value;
    if (mn) *mn = sl[i].min;
    if (mx) *mx = sl[i].max;
    if (st) *st = sl[i].step;
    return 1;
}

static int nudge_locked(int i, int dir) {
    if (i < 0 || i >= sl_n || !sl[i].live || dir == 0) return 0;
    double v = sl[i].value + (dir > 0 ? sl[i].step : -sl[i].step);
    if (v < sl[i].min) v = sl[i].min;
    if (v > sl[i].max) v = sl[i].max;
    if (v == sl[i].value) return 0;
    sl[i].value = v;
    return 1;
}

static int set_locked(int i, double value) {
    if (i < 0 || i >= sl_n || !sl[i].live || !isfinite(value)) return 0;
    Sld *s = &sl[i];
    double steps = (value - s->min) / s->step;
    if (isfinite(steps)) value = s->min + round(steps) * s->step;
    if (value < s->min) value = s->min;
    if (value > s->max) value = s->max;
    if (value == s->value) return 0;
    s->value = value;
    return 1;
}

int slider_info(int i, double *v, double *mn, double *mx, double *st) {
    pthread_mutex_lock(&slider_lock);
    int result = info_locked(i, v, mn, mx, st);
    pthread_mutex_unlock(&slider_lock);
    return result;
}

int slider_nudge(int i, int dir) {
    pthread_mutex_lock(&slider_lock);
    int result = nudge_locked(i, dir);
    pthread_mutex_unlock(&slider_lock);
    return result;
}

int slider_set(int i, double value) {
    pthread_mutex_lock(&slider_lock);
    int result = set_locked(i, value);
    pthread_mutex_unlock(&slider_lock);
    return result;
}
