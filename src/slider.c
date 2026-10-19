#include "slider.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define SLIDER_MAX 64

typedef struct {
    int live;
    int offset;
    double init, min, max, step, value;
} Sld;

struct SliderBank { Sld slots[SLIDER_MAX]; int n, pass, refs; };
static SliderBank initial_bank={.refs=1};
static SliderBank *active_bank=&initial_bank;
static unsigned sl_revision = 1;
static pthread_mutex_t slider_lock = PTHREAD_MUTEX_INITIALIZER;

void slider_begin(void) {
    SliderBank *bank=slider_bank_create();
    if (!bank) return;
    slider_bank_activate(bank); slider_bank_free(bank);
}

SliderBank *slider_bank_create(void) {
    SliderBank *bank=calloc(1,sizeof(*bank));
    if (!bank) return NULL;
    pthread_mutex_lock(&slider_lock);
    memcpy(bank->slots,active_bank->slots,sizeof(bank->slots));
    bank->refs=1;
    pthread_mutex_unlock(&slider_lock);
    return bank;
}

static void release_locked(SliderBank *bank) {
    if (bank && --bank->refs==0 && bank!=&initial_bank) free(bank);
}

void slider_bank_free(SliderBank *bank) {
    pthread_mutex_lock(&slider_lock); release_locked(bank);
    pthread_mutex_unlock(&slider_lock);
}

void slider_bank_activate(SliderBank *bank) {
    if (!bank) return;
    pthread_mutex_lock(&slider_lock);
    bank->refs++; release_locked(active_bank); active_bank=bank;
    pthread_mutex_unlock(&slider_lock);
}

static double bind_locked(SliderBank *bank, double init, double mn, double mx, double step) {
    if (!isfinite(init) || !isfinite(mn) || !isfinite(mx) || !isfinite(step))
        return init;
    if (mx < mn) { double t = mn; mn = mx; mx = t; }
    if (step <= 0) step = (mx - mn) / 1000.0;
    if (step <= 0) step = 1;
    if (bank->pass >= SLIDER_MAX) {
        if (init < mn) init = mn;
        if (init > mx) init = mx;
        return init;
    }
    Sld *s = &bank->slots[bank->pass];
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
    bank->pass++;
    bank->n = bank->pass;
    return s->value;
}

double slider_bind(double init, double mn, double mx, double step) {
    pthread_mutex_lock(&slider_lock);
    double value = bind_locked(active_bank, init, mn, mx, step);
    pthread_mutex_unlock(&slider_lock);
    return value;
}

int slider_bank_bind(SliderBank *bank, double init, double mn, double mx, double step) {
    if (!bank) return -1;
    pthread_mutex_lock(&slider_lock);
    int index=bank->pass;
    (void)bind_locked(bank,init,mn,mx,step);
    if (bank->pass!=index) bank->slots[index].offset=-1;
    if (bank->pass==index) index=-1;
    pthread_mutex_unlock(&slider_lock);
    return index;
}

int slider_bank_bind_site(SliderBank *bank, int offset, double init, double mn, double mx, double step) {
    if (!bank || offset<0) return slider_bank_bind(bank,init,mn,mx,step);
    pthread_mutex_lock(&slider_lock);
    int index;
    for (index=0;index<bank->n;index++) if (bank->slots[index].offset==offset) break;
    if (index==bank->n) {
        index=bank->pass;
        (void)bind_locked(bank,init,mn,mx,step);
        if (bank->pass==index) index=-1;
    }
    if (index>=0) {
        Sld *slot=&bank->slots[index];
        slot->offset=offset; slot->init=init;
        slot->value=init; /* Web ref keeps the literal; the widget alone clamps. */
    }
    pthread_mutex_unlock(&slider_lock);
    return index;
}

int slider_source_offset(int index) {
    pthread_mutex_lock(&slider_lock);
    int offset=index>=0 && index<active_bank->n?active_bank->slots[index].offset:-1;
    pthread_mutex_unlock(&slider_lock);
    return offset;
}

unsigned slider_revision(void) {
    pthread_mutex_lock(&slider_lock); unsigned revision=sl_revision;
    pthread_mutex_unlock(&slider_lock); return revision;
}

int slider_count(void) {
    pthread_mutex_lock(&slider_lock);
    int count = active_bank->n;
    pthread_mutex_unlock(&slider_lock);
    return count;
}

static int info_locked(SliderBank *bank, int i, double *v, double *mn, double *mx, double *st) {
    if (!bank || i < 0 || i >= bank->n || !bank->slots[i].live) return 0;
    if (v) *v = bank->slots[i].value;
    if (mn) *mn = bank->slots[i].min;
    if (mx) *mx = bank->slots[i].max;
    if (st) *st = bank->slots[i].step;
    return 1;
}

static int nudge_locked(int i, int dir) {
    if (i < 0 || i >= active_bank->n || !active_bank->slots[i].live || dir == 0) return 0;
    Sld *s=&active_bank->slots[i];
    double current=fmax(s->min,fmin(s->max,s->value));
    double position=round((current-s->min)/s->step);
    double v = s->min+(position+(dir>0?1:-1))*s->step;
    if (v < s->min) v = s->min;
    if (v > s->max) v = s->max;
    if (v == s->value) return 0;
    s->value = v;
    if (++sl_revision==0) sl_revision=1;
    return 1;
}

static int set_locked(int i, double value) {
    if (i < 0 || i >= active_bank->n || !active_bank->slots[i].live || !isfinite(value)) return 0;
    Sld *s = &active_bank->slots[i];
    double steps = (value - s->min) / s->step;
    if (isfinite(steps)) value = s->min + round(steps) * s->step;
    if (value < s->min) value = s->min;
    if (value > s->max) value = s->max;
    if (value == s->value) return 0;
    s->value = value;
    if (++sl_revision==0) sl_revision=1;
    return 1;
}

int slider_info(int i, double *v, double *mn, double *mx, double *st) {
    pthread_mutex_lock(&slider_lock);
    int result = info_locked(active_bank,i, v, mn, mx, st);
    pthread_mutex_unlock(&slider_lock);
    return result;
}

int slider_bank_read(SliderBank *bank, int index, double *value) {
    pthread_mutex_lock(&slider_lock);
    int result=info_locked(bank,index,value,NULL,NULL,NULL);
    pthread_mutex_unlock(&slider_lock); return result;
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
