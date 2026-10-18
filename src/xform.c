#include "xform.h"
#include "harmony.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

int xform_wants_rev(int rev, int palindrome, int cycle) {
    int flip = palindrome && (cycle & 1);
    return rev ? !flip : flip;
}

static unsigned mix32(unsigned x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/* 1 when this event is chosen. Stable for a cycle so lookahead does not flicker. */
static int coin(int cycle, double start, const char *tok, double p) {
    if (p <= 0) return 0;
    if (p >= 1) return 1;
    unsigned h = mix32((unsigned)(cycle < 0 ? 0 : cycle) * 0x9e3779b9u);
    h = mix32(h ^ (unsigned)(start * 100000.0 + 0.5));
    if (tok && tok[0]) h = mix32(h ^ (unsigned char)tok[0]);
    double u = (h & 0xFFFFFFu) / (double)0x1000000u;
    return u < p;
}

static int truthy(const char *t) {
    if (!t || !t[0]) return 0;
    if (strcmp(t, "~") == 0 || strcmp(t, "-") == 0 || strcmp(t, "rest") == 0) return 0;
    if (strcmp(t, "0") == 0 || strcmp(t, "false") == 0) return 0;
    return 1;
}

static void copy_event(PatEventList *dst, const PatEvent *e, double start, double dur) {
    patlist_push(dst, e->token, start, dur);
    if (dst->n < 1) return;
    PatEvent *n = &dst->items[dst->n - 1];
    n->extra = e->extra;
    n->rate = e->rate;
    n->send = e->send;
    n->src = e->src;
    n->src_n = e->src_n;
    n->pan = e->pan;
    n->inst = e->inst;
}

void xform_mirror(PatEventList *list) {
    if (!list) return;
    for (int i = 0; i < list->n; i++) {
        double s = list->items[i].start;
        double d = list->items[i].dur;
        list->items[i].start = 1.0 - (s + d);
        if (list->items[i].start < 0) list->items[i].start = 0;
    }
}

void xform_jux(PatEventList *list, int rev_right, int right_inst) {
    if (!list) return;
    int n0 = list->n;
    for (int i = 0; i < n0; i++) list->items[i].pan = 0.0;
    for (int i = 0; i < n0; i++) {
        PatEvent e = list->items[i];
        double s = e.start;
        if (rev_right) {
            s = 1.0 - (e.start + e.dur);
            if (s < 0) s = 0;
        }
        copy_event(list, &e, s, e.dur);
        if (list->n < 1) return;
        PatEvent *n = &list->items[list->n - 1];
        n->pan = 1.0;
        if (right_inst >= 0) n->inst = right_inst;
    }
}

static void add_degree(char *tok, int add) {
    int deg = 0, acc = 0;
    if (!harmony_degree(tok, &deg, &acc)) return;
    deg += add;
    char tmp[64];
    int k = snprintf(tmp, sizeof(tmp), "%d", deg);
    if (k < 0) return;
    while (acc > 0 && k + 1 < (int)sizeof(tmp)) { tmp[k++] = '#'; acc--; }
    while (acc < 0 && k + 1 < (int)sizeof(tmp)) { tmp[k++] = 'b'; acc++; }
    tmp[k] = 0;
    snprintf(tok, 64, "%s", tmp);
}

/* sometimes(fast/slow): the function runs on the events sometimesBy keeps
   (the complement of degrade). fast repeats inside the event's own slot. */
static void apply_sometimes(const XForm *x, PatEventList *list, int cycle) {
    int fast = x->sometimes_fast;
    double slow = x->sometimes_slow;
    if (fast < 2 && slow <= 1.0001) return;
    if (fast > 16) fast = 16;
    if (slow > 8) slow = 8;
    PatEventList out;
    patlist_init(&out);
    for (int i = 0; i < list->n; i++) {
        PatEvent *e = &list->items[i];
        int hit = coin(cycle, e->start, e->token, x->sometimes_p);
        if (!hit) {
            copy_event(&out, e, e->start, e->dur);
            continue;
        }
        if (fast >= 2) {
            double d = e->dur / fast;
            for (int k = 0; k < fast; k++)
                copy_event(&out, e, e->start + k * d, d);
        } else {
            double d = e->dur * slow;
            if (d > 4) d = 4;
            copy_event(&out, e, e->start, d);
        }
    }
    patlist_free(list);
    *list = out;
}

static void apply_off(const XForm *x, PatEventList *list) {
    if (!x->has_off) return;
    int n0 = list->n;
    int add = (int)lround(x->off_add);
    for (int i = 0; i < n0; i++) {
        PatEvent e = list->items[i];
        e.start += x->off_time;
        if (e.start >= 2.0 || e.start < -0.001) continue;
        if (x->off_degrees) add_degree(e.token, add);
        else e.extra += add;
        copy_event(list, &e, e.start, e.dur);
    }
}

static int covers(const PatEvent *e, double t) {
    return t + 1e-9 >= e->start && t < e->start + e->dur + 1e-9;
}

/* struct: timing from the structure, value from the source at the hit's midpoint. */
static void apply_struct(Node *st, PatEventList *list, int cycle) {
    if (!st) return;
    PatEventList shape, out;
    patlist_init(&shape);
    patlist_init(&out);
    expand_node(st, &shape, 0.0, 1.0, cycle);
    for (int i = 0; i < shape.n; i++) {
        if (!truthy(shape.items[i].token)) continue;
        double mid = shape.items[i].start + shape.items[i].dur * 0.5;
        for (int j = 0; j < list->n; j++) {
            if (!covers(&list->items[j], mid)) continue;
            copy_event(&out, &list->items[j], shape.items[i].start, shape.items[i].dur);
        }
    }
    patlist_free(&shape);
    patlist_free(list);
    *list = out;
}

static void apply_mask(Node *mask, PatEventList *list, int cycle) {
    if (!mask) return;
    PatEventList gate, out;
    patlist_init(&gate);
    patlist_init(&out);
    expand_node(mask, &gate, 0.0, 1.0, cycle);
    for (int i = 0; i < list->n; i++) {
        double mid = list->items[i].start + list->items[i].dur * 0.5;
        int on = 0;
        int saw = 0;
        for (int j = 0; j < gate.n; j++) {
            if (!covers(&gate.items[j], mid)) continue;
            saw = 1;
            on = truthy(gate.items[j].token);
        }
        if (saw && on) copy_event(&out, &list->items[i], list->items[i].start, list->items[i].dur);
    }
    patlist_free(&gate);
    patlist_free(list);
    *list = out;
}

static void apply_degrade(const XForm *x, PatEventList *list, int cycle) {
    if (x->degrade < 0) return;
    PatEventList out;
    patlist_init(&out);
    for (int i = 0; i < list->n; i++) {
        PatEvent *e = &list->items[i];
        if (coin(cycle, e->start, e->token, x->degrade)) continue;
        copy_event(&out, e, e->start, e->dur);
    }
    patlist_free(list);
    *list = out;
}

int xform_number_at(Node *pat, int cycle, double when, double *out) {
    if (!pat || !out) return 0;
    PatEventList l;
    patlist_init(&l);
    expand_node(pat, &l, 0.0, 1.0, cycle);
    int hit = 0;
    double v = 0;
    for (int i = 0; i < l.n; i++) {
        double a = l.items[i].start;
        double b = a + l.items[i].dur;
        if (when + 1e-9 < a || when > b + 1e-6) continue;
        char *end = NULL;
        double x = strtod(l.items[i].token, &end);
        if (end == l.items[i].token || !end || *end != 0 || !isfinite(x)) continue;
        v = x;
        hit = 1;
    }
    patlist_free(&l);
    if (!hit) return 0;
    *out = v;
    return 1;
}

static void apply_rarely(const XForm *x, PatEventList *list, int cycle) {
    if (x->rarely_p < 0) return;
    for (int i = 0; i < list->n; i++) {
        PatEvent *e = &list->items[i];
        if (!coin(cycle + 17, e->start, e->token, x->rarely_p)) continue;
        if (x->rarely_speed) {
            double v = 0;
            /* 0 stays "engine default". A real zero speed would be silence. */
            if (xform_number_at(x->rarely_speed, cycle, e->start, &v) && v != 0.0)
                e->rate = (float)v;
        } else if (x->rarely_rate > 0) {
            e->rate = (float)x->rarely_rate;
        }
        if (x->rarely_send >= 0) e->send = (float)x->rarely_send;
    }
}

void xform_events(const XForm *x, PatEventList *list, int cycle) {
    if (!x || !list) return;
    if (x->sometimes_p >= 0) apply_sometimes(x, list, cycle);
    apply_off(x, list);
    apply_struct(x->structure, list, cycle);
    apply_mask(x->mask, list, cycle);
    apply_degrade(x, list, cycle);
    apply_rarely(x, list, cycle);
}

static void fill_base(const XForm *x, PatEventList *out, int cycle) {
    patlist_init(out);
    if (!x->pat) return;
    expand_node(x->pat, out, 0.0, 1.0, cycle);
    if (xform_wants_rev(x->rev, x->palindrome, cycle)) xform_mirror(out);
    if (x->jux) xform_jux(out, x->jux_rev, x->jux_inst);
}

/* Semitones from add(note("…")) at an absolute cycle position. */
static int addend_at(Node *pat, double abs_t, double scalar) {
    int fallback = (int)lround(scalar);
    if (!pat || !isfinite(abs_t)) return fallback;
    if (abs_t < 0) abs_t = 0;
    int cyc = (int)floor(abs_t);
    if (cyc < 0) cyc = 0;
    if (cyc > 1000000) cyc = 1000000;
    double frac = abs_t - floor(abs_t);
    if (frac < 0) frac = 0;
    PatEventList l;
    patlist_init(&l);
    expand_node(pat, &l, 0.0, 1.0, cyc);
    int add = fallback;
    for (int i = 0; i < l.n; i++) {
        double a = l.items[i].start;
        double b = a + l.items[i].dur;
        if (frac + 1e-9 < a || frac > b + 1e-9) continue;
        char *end = NULL;
        long v = strtol(l.items[i].token, &end, 10);
        if (end != l.items[i].token && end && *end == 0) add = (int)v;
    }
    patlist_free(&l);
    return add;
}

void xform_off_arrow(const XForm *x, PatEventList *list, int cycle) {
    if (!x || !list || !x->off_fn || !x->pat) return;
    double T = x->off_time;
    double S = x->off_slow;
    double L = x->off_late;
    if (!isfinite(T)) T = 0;
    if (T > 8) T = 8;
    if (T < -8) T = -8;
    if (!isfinite(S) || S < 0.01) S = 1;
    if (S > 64) S = 64;
    if (!isfinite(L)) L = 0;
    if (L > 8) L = 8;
    if (L < -8) L = -8;
    double u0 = ((double)cycle - L) / S;
    double u1 = ((double)cycle + 1.0 - L) / S;
    int lo = (int)floor(u0 - T) - 1;
    int hi = (int)floor(u1 - T) + 1;
    if (lo < 0) lo = 0;
    if (hi > cycle + 16) hi = cycle + 16;
    if (hi < lo) return;
    if (hi - lo > 32) hi = lo + 32;
    for (int N = lo; N <= hi; N++) {
        PatEventList base;
        fill_base(x, &base, N);
        for (int i = 0; i < base.n; i++) {
            PatEvent e = base.items[i];
            double a0 = (double)N + e.start + T;
            double a1 = a0 + e.dur;
            if (a1 <= u0 + 1e-12 || a0 >= u1 - 1e-12) continue;
            double c0 = a0 < u0 ? u0 : a0;
            double c1 = a1 > u1 ? u1 : a1;
            double out_s = c0 * S + L - (double)cycle;
            double out_d = (c1 - c0) * S;
            if (out_s < 0) {
                out_d += out_s;
                out_s = 0;
            }
            if (out_s >= 1.0 || out_d <= 1e-9) continue;
            if (out_s + out_d > 1.0) out_d = 1.0 - out_s;
            if (out_d <= 1e-9) continue;
            e.extra += addend_at(x->off_add_pat, a0, x->off_add);
            if (x->off_inst >= 0) e.inst = x->off_inst;
            copy_event(list, &e, out_s, out_d);
        }
        patlist_free(&base);
    }
}

static double fade(double t) { return t * t * (3.0 - 2.0 * t); }

static double hash01(int n) {
    unsigned u = mix32((unsigned)n * 747796405u + 2891336453u);
    return (u & 0xFFFFFFu) / (double)0x1000000u;
}

static double perlin01(double t) {
    double fl = floor(t);
    int i = (int)fl;
    double f = t - fl;
    double a = hash01(i);
    double b = hash01(i + 1);
    return a + (b - a) * fade(f);
}

double xform_signal(int kind, double slow, double fast, int has_range,
                    double lo, double hi, int cycle, double when) {
    if (slow < 0.01) slow = 1;
    if (fast < 0.01) fast = 1;
    double cyc = cycle < 0 ? 0 : (double)cycle;
    double phase = (cyc + when) * fast / slow;
    double u;
    if (kind == 2) u = perlin01(phase);
    else if (kind == 3) u = hash01((int)floor(phase * 64.0));
    else u = 0.5 + 0.5 * sin(2.0 * M_PI * phase);
    if (u < 0) u = 0;
    if (u > 1) u = 1;
    if (!has_range) return u;
    return lo + (hi - lo) * u;
}
