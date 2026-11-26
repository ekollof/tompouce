#include "dsp.h"
#include "dsp_supersaw.h"
#include "voice_soa_dsp.h"
#include "common.h"
#include <float.h>
#ifdef TOMPOUCE_RENDER_PROFILE
#include <stdatomic.h>
#include <time.h>
static _Atomic unsigned long long dsp_calls, dsp_total_ns, dsp_max_ns;
static unsigned long long dsp_cycles(void);
#endif

typedef struct { const char *name; double initial, min, max; } Parameter;
static const Parameter parameters[] = {
    {"cutoff",0,0,20000}, {"resonance",1,.05,50}, {"lpenv",0,-16,16},
    {"lpattack",.005,0,30}, {"lpdecay",.14,0,30}, {"lpsustain",0,0,1}, {"lprelease",.1,0,30},
    {"hcutoff",0,0,20000}, {"hresonance",1,.05,50}, {"hpenv",0,-16,16},
    {"hpattack",.005,0,30}, {"hpdecay",.14,0,30}, {"hpsustain",0,0,1}, {"hprelease",.1,0,30},
    {"bandf",0,0,20000}, {"bandq",1,.05,50}, {"bpenv",0,-16,16},
    {"bpattack",.005,0,30}, {"bpdecay",.14,0,30}, {"bpsustain",0,0,1}, {"bprelease",.1,0,30},
    {"fanchor",0,0,1}, {"ftype",0,0,2}, {"vowel",-1,-1,14},
    {"coarse",1,1,1024}, {"crush",16,1,24}, {"shape",0,0,.99},
    {"distort",0,0,100}, {"postgain",1,0,16}, {"tremolo",0,0,20000},
    {"tremolodepth",1,0,1}, {"tremoloskew",.5,.001,.999}, {"tremolophase",0,-64,64},
    {"tremoloshape",0,0,4}, {"phaserrate",0,0,200}, {"phaserdepth",.75,0,1},
    {"phasercenter",1000,20,16000}, {"phasersweep",2000,0,16000},
    {"penv",0,-96,96}, {"pattack",.2,0,30}, {"pdecay",.001,0,30},
    {"psustain",1,0,1}, {"prelease",.001,0,30}, {"panchor",1,0,1},
    {"vib",0,0,200}, {"vibmod",.1,0,48}, {"room",0,0,1},
    {"roomsize",2,0,10}, {"dry",1,0,1}, {"begin",0,0,1}, {"end",1,0,1},
    {"loop",0,0,1}, {"loopBegin",0,0,1}, {"loopEnd",1,0,1},
    {"cut",0,0,65535}, {"clip",1,0,64}, {"pw",.5,.01,.99},
    {"pwrate",0,0,20000}, {"pwsweep",0,0,1}, {"unison",5,1,DSP_SUPERSAW_VOICES}, {"spread",.6,0,1},
    {"distorttype",0,0,6}, {"distortvol",1,0,16},
    {"compressor",0,-100,0}, {"compressorRatio",10,1,20}, {"compressorKnee",10,0,40},
    {"compressorAttack",.005,0,1}, {"compressorRelease",.05,0,1}, {"pcurve",0,0,1},
    {"transient",0,-1,1}, {"transsustain",0,-1,1},
    {"chorus",0,0,1},{"shapevol",1,0,16},
    {"lprate",0,0,20000},{"lpdepth",1,0,16},{"lpdepthfrequency",0,0,20000},
    {"lpshape",0,0,4},{"lpdc",-.5,-1,1},{"lpskew",.5,.001,.999},{"lpcurve",1,.001,64},
    {"hprate",0,0,20000},{"hpdepth",1,0,16},{"hpdepthfrequency",0,0,20000},
    {"hpshape",0,0,4},{"hpdc",-.5,-1,1},{"hpskew",.5,.001,.999},{"hpcurve",1,.001,64},
    {"bprate",0,0,20000},{"bpdepth",1,0,16},{"bpdepthfrequency",0,0,20000},
    {"bpshape",0,0,4},{"bpdc",-.5,-1,1},{"bpskew",.5,.001,.999},{"bpcurve",1,.001,64},
    {"roomfade",.1,0,10},{"roomlp",15000,0,22050},{"roomdim",1000,0,22050}
};
_Static_assert(sizeof(parameters)/sizeof(parameters[0]) == DSP_FMBASE,
               "DSP parameter names must match their enum indices");

static double clamp(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static double number(const cJSON *object, const char *name, double initial, double lo, double hi) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsNumber(v) && isfinite(v->valuedouble) ? clamp(v->valuedouble, lo, hi) : initial;
}

void dsp_read_adsr(const cJSON *value, const char *const names[4], const double defaults[4], double out[4]) {
    int present[4], any = 0;
    for (int i = 0; i < 4; i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(value,names[i]);
        present[i] = cJSON_IsNumber(v) && isfinite(v->valuedouble);
        any |= present[i];
    }
    if (!any) { memcpy(out,defaults,4*sizeof(*out)); return; }
    out[0] = number(value,names[0],.001,.001,30);
    out[1] = number(value,names[1],.001,.001,30);
    out[2] = number(value,names[2],present[1] ? .001 : 1,0,1);
    out[3] = number(value,names[3],.01,.01,30);
}

void dsp_params_init(DspParams *p) {
    memset(p, 0, sizeof(*p));
    p->supersaw_detune = .18;
    p->lfo_start_time = -1;
    for (int i = 0; i < DSP_FMBASE; i++) p->p[i] = parameters[i].initial;
    for (int i = 0; i < 8; i++) {
        double *op = p->p + DSP_FMBASE + i * 8;
        op[1] = 1; op[2] = .001; op[3] = .001; op[4] = 1; op[5] = .01; op[6] = 1;
    }
}

static int symbol(const cJSON *obj, const char *key, const char *const *names, int n) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(v)) return -1;
    for (int i = 0; i < n; i++) if (!strcmp(v->valuestring, names[i])) return i;
    return -1;
}

int dsp_parameter_lookup(const char *name, int *index, double *minimum,
                         double *maximum) {
    if (!name) return 0;
    for (int i=0;i<DSP_FMBASE;i++) if (!strcmp(parameters[i].name,name)) {
        if (index) *index=i;
        if (minimum) *minimum=parameters[i].min;
        if (maximum) *maximum=parameters[i].max;
        return 1;
    }
    return 0;
}

void dsp_params_read(DspParams *p, const cJSON *value) {
    dsp_params_init(p); p->enabled = 1;
    /* Superdough's supersaw uses detune ?? n ?? .18 as total frequency
       spread in semitones, separate from pitch-envelope/vibrato cents. */
    p->supersaw_detune = number(value,"detune",number(value,"n",.18,-9600,9600),-9600,9600);
    p->noise_mix=number(value,"noise",0,0,1);
    for (int i = 0; i < DSP_FMBASE; i++)
        p->p[i] = number(value, parameters[i].name, p->p[i], parameters[i].min, parameters[i].max);
    /* An explicit threshold (even 0) installs the deployed compressor node
       with its structural latency; the untouched default installs nothing. */
    if (cJSON_HasObjectItem(value,"compressor")) p->compressor_on=1;
    /* The transient node exists only when the hap carries transient;
       transsustain alone installs nothing. */
    if (cJSON_HasObjectItem(value,"transient")) p->transient_on=1;
    /* Superdough getADSRValues: specifying any segment changes the
       defaults of the other segments, even when only attack is given. */
    for (int base = 0; base < 21; base += 7) {
        int a = cJSON_HasObjectItem(value,parameters[base+3].name);
        int d = cJSON_HasObjectItem(value,parameters[base+4].name);
        int s = cJSON_HasObjectItem(value,parameters[base+5].name);
        int r = cJSON_HasObjectItem(value,parameters[base+6].name);
        if (!(a || d || s || r)) continue;
        const char *names[4];
        const double defaults[4] = {.005,.14,0,.1};
        for (int i = 0; i < 4; i++) names[i] = parameters[base+3+i].name;
        dsp_read_adsr(value,names,defaults,p->p+base+3);
        if (!cJSON_HasObjectItem(value,parameters[base+2].name)) p->p[base+2] = 1;
    }
    const char *const pitch_names[]={"pattack","pdecay","psustain","prelease"};
    const double pitch_defaults[]={.2,.001,1,.001};
    dsp_read_adsr(value,pitch_names,pitch_defaults,p->p+DSP_PATT);
    p->p[DSP_PANCHOR]=number(value,"panchor",p->p[DSP_PSUS],0,1);
    if (!cJSON_HasObjectItem(value,"penv")) for (int i=0;i<4;i++) {
        const cJSON *control=cJSON_GetObjectItemCaseSensitive(value,pitch_names[i]);
        if (cJSON_IsNumber(control) && isfinite(control->valuedouble)) { p->p[DSP_PENV]=1; break; }
    }
    const char *const vowels[] = {"a","e","i","o","u","ae","aa","oe","ue","y","uh","un","en","an","on"};
    int v = symbol(value, "vowel", vowels, 15);
    if (v < 0) {
        const cJSON *x = cJSON_GetObjectItemCaseSensitive(value,"vowel");
        const char *aliases[] = {"æ","å","ɑ","ø","ö","ı","ʌ","œ̃","ɛ̃","ɑ̃","ɔ̃"};
        const int index[] = {5,6,6,7,7,9,10,11,12,13,14};
        if (cJSON_IsString(x))
            for (int i = 0; i < 11; i++) if (!strcmp(x->valuestring,aliases[i])) { v = index[i]; break; }
    }
    if (v >= 0) p->p[DSP_VOWEL] = v;
    const char *const types[] = {"12db", "ladder", "24db"};
    v = symbol(value, "ftype", types, 3);
    if (v >= 0) p->p[DSP_FTYPE] = v;
    const char *const waves[] = {"sine", "tri", "square", "saw", "ramp"};
    v = symbol(value, "tremoloshape", waves, 5);
    if (v >= 0) p->p[DSP_TSHAPE] = v;
    /* The deployed tremolo defaults to a triangle LFO; a numeric shape uses
       the worklet numbering (tri 0, sine 1, ramp 2, saw 3, square 4), while
       shape names already resolve to the native wave numbering. */
    if (p->p[DSP_TREM] != 0 || cJSON_HasObjectItem(value, "tremolosync")) {
        const cJSON *tsh = cJSON_GetObjectItemCaseSensitive(value, "tremoloshape");
        if (cJSON_IsNumber(tsh) && isfinite(tsh->valuedouble)) {
            int w = (int)lround(tsh->valuedouble);
            if (w >= 0 && w <= 4) p->p[DSP_TSHAPE] = ((const int[]){1,0,4,3,2})[w];
            else p->p[DSP_TSHAPE] = 1;
        } else if (!cJSON_IsString(tsh) || v < 0) {
            p->p[DSP_TSHAPE] = 1;
        }
        if (!cJSON_HasObjectItem(value, "tremoloskew")) p->p[DSP_TSKEW] = 1;
    }
    const char *shape_names[] = {"lpshape","hpshape","bpshape"};
    for (int i = 0; i < 3; i++) {
        v = symbol(value,shape_names[i],waves,5);
        if (v >= 0) p->p[DSP_LPSHAPE+i*7] = v;
    }
    const char *const disttypes[] = {"tanh","fold","chebyshev","scurve","diode","asym","sinefold"};
    v = symbol(value,"distorttype",disttypes,7);
    if (v >= 0) p->p[DSP_DISTTYPE] = v;
    p->p[DSP_SIZE] = number(value,"size",p->p[DSP_SIZE],0,10);
    const cJSON *partials = cJSON_GetObjectItemCaseSensitive(value,"partials");
    const cJSON *phases = cJSON_GetObjectItemCaseSensitive(value,"phases");
    const cJSON *sound = cJSON_GetObjectItemCaseSensitive(value,"s");
    const char *s = cJSON_IsString(sound) ? sound->valuestring : "triangle";
    int basic=!strcmp(s,"saw") || !strcmp(s,"sawtooth") || !strcmp(s,"square") ||
        !strcmp(s,"triangle") || !strcmp(s,"tri") || !strcmp(s,"user");
    if (basic) {
        p->periodic_kind=(!strcmp(s,"saw") || !strcmp(s,"sawtooth"))?1:
            !strcmp(s,"square")?2:!strcmp(s,"user")?4:3;
        if (!partials || cJSON_IsNull(partials)) partials=cJSON_GetObjectItemCaseSensitive(value,"n");
    }
    if (basic && cJSON_IsNumber(partials) && isfinite(partials->valuedouble) && partials->valuedouble!=0) {
        p->periodic_numeric=1;
        p->n_partials=partials->valuedouble<1?-1:(int)fmin(2047,floor(partials->valuedouble));
        for (int i=0;i<32;i++) {
            const cJSON *phase=cJSON_GetArrayItem(phases,i);
            if (cJSON_IsNumber(phase) && isfinite(phase->valuedouble)) p->phases[i]=phase->valuedouble-floor(phase->valuedouble);
        }
    } else if (basic && cJSON_IsArray(partials) && cJSON_GetArraySize(partials)) {
        p->n_partials = cJSON_GetArraySize(partials);
        if (p->n_partials > 32) p->n_partials = -1;
        for (int i = 0; i < p->n_partials; i++) {
            const cJSON *x = cJSON_GetArrayItem(partials,i);
            const cJSON *y = cJSON_GetArrayItem(phases,i);
            p->partials[i] = cJSON_IsNumber(x) && isfinite(x->valuedouble) ? clamp(x->valuedouble,-1000000,1000000) : 0;
            if (cJSON_IsNumber(y) && isfinite(y->valuedouble)) p->phases[i] = y->valuedouble-floor(y->valuedouble);
        }
    }
    if (!strcmp(s,"pulse")) {
        p->p[DSP_PW]=number(value,"pw",.5,-.99,.99);
        if (cJSON_HasObjectItem(value,"pwrate") && !cJSON_HasObjectItem(value,"pwsweep")) p->p[DSP_PWSWEEP]=.3;
        if (cJSON_HasObjectItem(value,"pwsweep") && !cJSON_HasObjectItem(value,"pwrate")) p->p[DSP_PWRATE]=1;
    }
    const char *fields[] = {"fmi", "fmh", "fmattack", "fmdecay", "fmsustain", "fmrelease", "fmenv", "fmwave"};
    const double maximum[] = {64,64,30,30,1,30,1,6};
    for (int i = 0; i < 8; i++) {
        char adsr_names[4][32]; const char *adsr_keys[4];
        for (int j=0;j<4;j++) {
            snprintf(adsr_names[j],sizeof(adsr_names[j]),"%s%d",fields[j+2],i+1);
            adsr_keys[j]=i==0 && !cJSON_HasObjectItem(value,adsr_names[j])?
                fields[j+2]:adsr_names[j];
        }
        const double defaults[4]={.001,.001,1,.01};
        for (int j = 0; j < 8; j++) {
            char name[32];
            snprintf(name, sizeof(name), "%s%d", fields[j], i + 1);
            double *dest = p->p + DSP_FMBASE + i * 8 + j;
            if (j>=2 && j<=5 && (cJSON_HasObjectItem(value,name) ||
                (i==0 && cJSON_HasObjectItem(value,fields[j])))) p->fm_envelopes|=1u<<i;
            double minimum=j<2?-64:0;
            if (i == 0) *dest = number(value, fields[j], *dest, minimum, maximum[j]);
            *dest = number(value, name, *dest, minimum, maximum[j]);
            if (j == 6) {
                const cJSON *x = cJSON_GetObjectItemCaseSensitive(value, name);
                if (!x && i == 0) x = cJSON_GetObjectItemCaseSensitive(value, fields[j]);
                if (cJSON_IsString(x)) *dest = strcmp(x->valuestring, "exp") == 0;
            } else if (j == 7) {
                const char *const shapes[] = {"sine","triangle","square","sawtooth","white","pink","brown"};
                int kind = symbol(value,name,shapes,7);
                if (kind < 0 && i == 0) kind = symbol(value,fields[j],shapes,7);
                if (kind >= 0) *dest = kind;
            }
        }
        dsp_read_adsr(value,adsr_keys,defaults,p->p+DSP_FMBASE+i*8+2);
        for (int j = 0; j < 9; j++) {
            char name[32];
            snprintf(name, sizeof(name), "fmi%d%d", i + 1, j);
            /* Upstream reserves 1->0, 2->1, ... for the numbered index;
               explicit matrix aliases do not override these default edges. */
            p->matrix[i][j] = j==i?p->p[DSP_FMBASE+i*8]:number(value, name, 0, -64, 64);
        }
    }
}

int dsp_prepare_fm(DspParams *p) {
    if (!p) return 0;
    for (int i = 0; i < 8; i++) {
        int shape = (int)p->p[DSP_FMBASE + i * 8 + 7];
        if (shape < 4 || shape > 6) continue;
        const float *buffer = dsp_prepare_noise(shape - 4);
        if (!buffer) return 0;
        p->fm_noise[shape - 4] = buffer;
    }
    return 1;
}

static double held_envelope(double t, double a, double d, double s) {
    if (t < a) return a > 0 ? t / a : 1;
    if (t < a + d) return 1 - (1 - s) * (t - a) / d;
    return s;
}

static double dsp_envelope_impl(double age, double gate, double a, double d, double s, double r) {
    if (age < 0) return 0;
    if (age <= gate) return held_envelope(age, a, d, s);
    if (r <= 0 || age >= gate + r) return 0;
    return held_envelope(gate, a, d, s) * (1 - (age - gate) / r);
}

#ifdef TOMPOUCE_RENDER_PROFILE
static _Atomic unsigned long long env_calls, env_total_cycles;
void dsp_envelope_profile_stats(unsigned long long *calls,
                                unsigned long long *total_cycles) {
    if (calls) *calls=atomic_load(&env_calls);
    if (total_cycles) *total_cycles=atomic_load(&env_total_cycles);
}
double dsp_envelope(double age, double gate, double a, double d, double s, double r) {
    unsigned long long started=dsp_cycles();
    double result=dsp_envelope_impl(age,gate,a,d,s,r);
    atomic_fetch_add(&env_calls,1);
    atomic_fetch_add(&env_total_cycles,dsp_cycles()-started);
    return result;
}
#else
double dsp_envelope(double age, double gate, double a, double d, double s, double r) {
    return dsp_envelope_impl(age,gate,a,d,s,r);
}
#endif

static double exponential_ramp(double from, double to, double fraction) {
    from = fmax(.001,from); to = fmax(.001,to);
    return from*pow(to/from,clamp(fraction,0,1));
}

/* Match the frequency automation endpoints of Superdough getParamADSR,
   including arithmetic sustain levels and notes shorter than a segment. */
double dsp_filter_frequency_values(double cutoff, double depth, double anchor,
    double attack, double decay, double sustain_ratio, double release,
    double age, double gate) {
    double adepth = fabs(depth);
    double low = fmin(20000,cutoff*pow(2,-adepth*anchor));
    double high = fmin(20000,cutoff*pow(2,adepth*(1-anchor)));
    double start = depth < 0 ? high : low;
    double peak = depth < 0 ? low : high;
    double sustain = start+(peak-start)*sustain_ratio;
    double a = attack, d = decay, r = release;
    double attack_end = fmin(a,gate);
    double attack_value = a > 0 ? start+(peak-start)*attack_end/a : peak;
    double decay_end = fmin(a+d,gate);
    double decay_value = d > 0 ? peak+(sustain-peak)*(decay_end-a)/d : sustain;
    double held;
    double t = fmin(age,gate);
    if (t < attack_end) held = exponential_ramp(start,attack_value,t/attack_end);
    else if (t < decay_end) held = exponential_ramp(peak,decay_value,(t-a)/(decay_end-a));
    else held = gate < a ? attack_value : gate < a+d ? decay_value : sustain;
    if (age <= gate) return held;
    return r > 0 ? exponential_ramp(held,start,(age-gate)/r) : start;
}

double dsp_filter_frequency(const DspParams *params, int type, double age, double gate) {
    if (!params || type < 0 || type > 2) return 0;
    const double *p = params->p;
    int base = type*7;
    return dsp_filter_frequency_values(p[base],p[base+2],p[DSP_ANCHOR],
        p[base+3],p[base+4],p[base+5],p[base+6],age,gate);
}

static void coefficients(DspBiquad *f, int type, double hz, double q) {
    double w = 2 * M_PI * clamp(hz, 20, SR * .45) / SR;
    /* WebAudio low/high-pass Q is expressed in dB; band-pass uses
       the linear quality factor. */
    double quality = type == 2 ? clamp(q,.05,50) : pow(10,clamp(q,.05,50)/20);
    double c = cos(w), alpha = sin(w) / (2 * quality);
    double a0 = 1 + alpha;
    if (type == 0) { f->b0 = (1-c)/2/a0; f->b1 = (1-c)/a0; f->b2 = f->b0; }
    else if (type == 1) { f->b0 = (1+c)/2/a0; f->b1 = -(1+c)/a0; f->b2 = f->b0; }
    else { f->b0 = alpha/a0; f->b1 = 0; f->b2 = -alpha/a0; }
    f->a1 = -2*c/a0; f->a2 = (1-alpha)/a0;
}

void dsp_filter_coefficients(const DspParams *params, int type, double hz,
                             DspBiquad *filter) {
    if (!params || !filter || type<0 || type>2) return;
    coefficients(filter,type,hz,params->p[type*7+1]);
}

void dsp_filter_q(DspBiquad *filter, int type, double hz, double q) {
    if (!filter || type<0 || type>2) return;
    coefficients(filter,type,hz,q);
}

static double biquad(DspBiquad *f, double x);

double dsp_biquad_process(DspBiquad *filter, double sample) {
    return biquad(filter,sample);
}

int dsp_single_filter(const DspState *state) {
    if (!state || !state->control.enabled || !state->effects_active) return -1;
    const double *p=state->control.p;
    int type=-1, active=0;
    for (int i=0;i<3;i++) if (p[i*7]>0) { type=i; active++; }
    if (active!=1) return -1;
    if (p[DSP_VOWEL]>=0 || p[DSP_FTYPE]!=0 || (int)p[DSP_COARSE]>1 ||
        p[DSP_CRUSH]<16 || p[DSP_SHAPE]>0 || p[DSP_DISTORT]>0 ||
        p[DSP_TREM]>0 || p[DSP_COMP]<0 || state->control.compressor_on ||
        state->control.transient_on || p[DSP_PHASER]>0 ||
        p[DSP_CHORUS]>0 || p[DSP_POST]!=1 || p[DSP_CUT]>0 ||
        p[DSP_LPRATE+type*7]>0 || p[DSP_PWRATE]>0 ||
        state->control.noise_mix>0 || state->control.source_noise) return -1;
    return type;
}

static double biquad(DspBiquad *f, double x) {
    double y = f->b0 * x + f->z1;
    f->z1 = f->b1*x - f->a1*y + f->z2;
    f->z2 = f->b2*x - f->a2*y;
    if (!isfinite(y) || !isfinite(f->z1) || !isfinite(f->z2)) {
        f->z1 = f->z2 = 0; return 0;
    }
    return y;
}

/* Chromium's BiquadFilterNode uses direct form I, which reacts differently
   from transposed direct form II when coefficients change per sample. */
static double biquad_df1(DspBiquad *f, double x) {
    double y = f->b0*x + f->b1*f->x1 + f->b2*f->x2
        - f->a1*f->y1 - f->a2*f->y2;
    f->x2 = f->x1; f->x1 = x;
    f->y2 = f->y1; f->y1 = y;
    if (!isfinite(y) || !isfinite(f->x1) || !isfinite(f->x2) ||
        !isfinite(f->y1) || !isfinite(f->y2)) {
        f->x1 = f->x2 = f->y1 = f->y2 = 0;
        return 0;
    }
    return y;
}

static double ladder_filter(DspState *s, double x) {
    double g = s->ladder_g, k = s->ladder_k;
    double g2 = g*g, g3 = g2*g, g4 = g3*g;
    double feedback = (1-g)*(g3*s->ladder[0]+g2*s->ladder[1]+g*s->ladder[2]+s->ladder[3]);
    double y = (tanh(x)-k*feedback)/(1+k*g4);
    for (int i = 0; i < 4; i++) {
        double v = (tanh(y)-s->ladder[i])*g;
        y = v+s->ladder[i];
        s->ladder[i] = y+v;
    }
    return y;
}

static void fm_visit(DspState *s,unsigned node,unsigned char visited[8]) {
    /* Visiting before recursion bounds cycles. A back edge reads the prior
       quantum; forward dependencies render before their destination. */
    if (visited[node]) return;
    visited[node]=1;
    for (unsigned source=0;source<8;source++)
        if (s->control.matrix[source][node+1]) fm_visit(s,source,visited);
    s->fm_order[s->fm_count++]=node;
}

/* Web vowel topology: five parallel bandpasses with per-formant gains
   and a makeup gain of 8 (packages/superdough/vowel.mjs). Q reaches 140,
   past the user-facing resonance clamp, so formants use unclamped RBJ
   bandpass coefficients identical to the WebAudio definition. */
static const int vowel_freqs[15][5] = {
    {660,1120,2750,3000,3350},{440,1800,2700,3000,3300},{270,1850,2900,3350,3590},
    {430,820,2700,3000,3300},{370,630,2750,3000,3400},{650,1515,2400,3000,3350},
    {560,900,2570,3000,3300},{500,1430,2300,3000,3300},{250,1750,2150,3200,3300},
    {400,1460,2400,3000,3300},{600,1250,2100,3100,3500},{500,1240,2280,3000,3500},
    {600,1480,2450,3200,3300},{700,1050,2500,3000,3300},{500,1080,2350,3000,3300}
};
static const double vowel_gains[15][5] = {
    {1,.5012,.0708,.0631,.0126},{1,.1995,.1259,.1,.1},{1,.0631,.0631,.0158,.0158},
    {1,.3162,.0501,.0794,.01995},{1,.1,.0708,.0316,.01995},{1,.5,.1008,.0631,.0126},
    {1,.5,.0708,.0631,.0126},{1,.2,.0708,.0316,.01995},{1,.1,.0708,.0316,.01995},
    {1,.2,.0708,.0316,.02995},{1,.3,.0608,.0316,.01995},{1,.1,.1708,.0216,.02995},
    {1,.15,.0708,.0316,.02995},{1,.1,.0708,.0316,.02995},{1,.1,.0708,.0316,.02995}
};
static const int vowel_qs[15][5] = {
    {80,90,120,130,140},{70,80,100,120,120},{40,90,100,120,120},
    {40,80,100,120,120},{40,60,100,120,120},{80,90,120,130,140},
    {80,90,120,130,140},{40,60,100,120,120},{40,60,100,120,120},
    {40,60,100,120,120},{40,70,100,120,130},{40,60,100,120,120},
    {40,60,100,120,120},{40,60,100,120,120},{40,60,100,120,120}
};

void dsp_init(DspState *s, const DspParams *p) {
    memset(s, 0, sizeof(*s));
    s->control = *p;
    s->noise_seed = 0x1234567u;
    s->compressor_gain = 1;
    s->trans_avg = 1;
    s->pulse[0]=-M_PI;
    int vowel = (int)p->p[DSP_VOWEL];
    if (vowel >= 0 && vowel < 15)
        for (int i = 0; i < 5; i++) {
            double w = 2*M_PI*clamp(vowel_freqs[vowel][i],20,SR*.45)/SR;
            double alpha = sin(w)/(2*vowel_qs[vowel][i]);
            double a0 = 1+alpha, c = cos(w);
            s->formant[i].b0 = alpha/a0; s->formant[i].b1 = 0; s->formant[i].b2 = -alpha/a0;
            s->formant[i].a1 = -2*c/a0; s->formant[i].a2 = (1-alpha)/a0;
            s->formant[i].z1 = s->formant[i].z2 = 0;
            s->formant_gain[i] = vowel_gains[vowel][i];
        }
    /* Cache which per-sample paths can change the signal. The controls are a
       copy fixed for the voice's lifetime, so this is computed once here. */
    s->pitch_active = p->p[DSP_PENV] != 0 || p->p[DSP_VIB] > 0;
    unsigned char visited[8]={0};
    for (unsigned i=0;i<8;i++) s->control.matrix[i][i]=p->p[DSP_FMBASE+i*8];
    for (unsigned i=0;i<8;i++) if (s->control.matrix[i][0]) fm_visit(s,i,visited);
    s->fm_active=s->fm_count>0;
    s->fm_frame=128;
    int active = 0;
    for (int type = 0; type < 3 && !active; type++)
        if (p->p[type*7] > 0) active = 1;
    if (p->p[DSP_VOWEL] >= 0 || (int)p->p[DSP_COARSE] > 1 || p->p[DSP_CRUSH] < 16 ||
        p->p[DSP_SHAPE] > 0 || p->p[DSP_DISTORT] > 0 || p->p[DSP_TREM] > 0 ||
        p->p[DSP_COMP] < 0 || p->compressor_on || p->transient_on ||
        p->p[DSP_PHASER] > 0 || p->p[DSP_CHORUS] > 0 ||
        p->p[DSP_POST] != 1) active = 1;
    s->effects_active = active;
}

static double scheduled_envelope(double age,double gate,double first_time,int exponential,
    double low,double high,double attack,double decay,double sustain_ratio,double release) {
    /* Superdough schedules gain and pitch AudioParam ramps alike,
       clipping segment endpoints linearly when the gate cuts them short.
       Exponential zero range endpoints become .001; sustain stays exact.
       BufferSource's first evaluation clamps past events to first_time. */
    /* Remove accumulated clock error only at sample-aligned ages, so a
       discontinuity lands on its frame without moving fractional onsets. */
    double frame=age*SR,nearest=round(frame);
    if (fabs(frame-nearest)<1e-6) age=nearest/SR;
    if (exponential && low==0) low=.001;
    if (exponential && high==0) high=.001;
    double sustain=low+sustain_ratio*(high-low);
    double times[5]={0},values[5]={low};
    int ramps[5]={0},count=1;
    if (attack>gate) {
        times[count]=gate;
        values[count]=gate*((high-low)/attack)+low;
        if(exponential && values[count]==0)values[count]=.001;
        ramps[count++]=1;
    } else {
        times[count]=attack; values[count]=high; ramps[count++]=1;
        if (attack+decay>gate) {
            times[count]=gate; values[count]=(gate-attack)*((sustain-high)/decay)+high;
            if(exponential && values[count]==0)values[count]=.001;
            ramps[count++]=1;
        } else {
            times[count]=attack+decay;
            /* Preserve the web's slope operation order: rounding around zero
               can turn a nominally zero endpoint into a sign-crossing ramp. */
            values[count]=(times[count]-attack)*((sustain-high)/decay)+high;
            if(exponential && values[count]==0)values[count]=.001;
            ramps[count++]=1;
            times[count]=gate; values[count]=sustain; count++;
        }
    }
    times[count]=gate+release; values[count]=low; ramps[count++]=1;
    for (int i=0;i<count;i++) times[i]=fmax(first_time,times[i]);
    for (int i=1;i<count;i++) {
        if (age<times[i]) {
            double start=values[i-1],end=values[i];
            /* WebAudio holds the start value for zero/sign-crossing ramps. */
            if (!ramps[i]) return start;
            double fraction=(age-times[i-1])/(times[i]-times[i-1]);
            if (!exponential) return start+(end-start)*fraction;
            if (start==0 || end==0 || signbit(start)!=signbit(end)) return start;
            return start*pow(end/start,fraction);
        }
    }
    return low;
}

static double scheduled_pitch_cents(const double *p,double age,double gate,double first_time,int exponential) {
    double depth=100*p[DSP_PENV];
    return scheduled_envelope(age,gate,first_time,exponential,0-depth*p[DSP_PANCHOR],
        depth-depth*p[DSP_PANCHOR],p[DSP_PATT],p[DSP_PDEC],p[DSP_PSUS],p[DSP_PREL]);
}

/* Pitch-envelope multiplier without vibrato, shared by the closed-form
   and integrated-vibrato paths. */
static double pitch_env_mult(const double *p, double age, double gate) {
    double ratio = 1;
    if (p[DSP_PENV] != 0) {
        if (p[DSP_PCURVE]>0) ratio*=pow(2,scheduled_pitch_cents(p,age,gate,0,1)/1200);
        else {
            double env = dsp_envelope(age, gate, p[DSP_PATT], p[DSP_PDEC], p[DSP_PSUS], p[DSP_PREL]);
            ratio *= pow(2, p[DSP_PENV] * (env - p[DSP_PANCHOR]) / 12);
        }
    }
    return ratio;
}

double dsp_pitch_multiplier(const DspParams *params, double age, double gate) {
    if (!params->enabled) return 1;
    const double *p = params->p;
    /* Neither branch below can change the ratio; skip the envelope and the
       transcendental work entirely. */
    if (p[DSP_PENV] == 0 && p[DSP_VIB] <= 0) return 1;
    double ratio = pitch_env_mult(p, age, gate);
    if (p[DSP_VIB] > 0) ratio *= pow(2, sin(2*M_PI*age*p[DSP_VIB])*(p[DSP_VIBMOD]+params->gen_lfo[GEN_VIBMOD]/100)/12);
    return ratio;
}

double dsp_sample_pitch_multiplier(const DspParams *params,double age,double gate,double first_time) {
    if (!params->enabled || first_time<=0) return dsp_pitch_multiplier(params,age,gate);
    const double *p=params->p;
    double cents=p[DSP_PENV]!=0?scheduled_pitch_cents(p,age,gate,first_time,p[DSP_PCURVE]>0):0;
    if (p[DSP_VIB]>0) cents+=100*sin(2*M_PI*age*p[DSP_VIB])*(p[DSP_VIBMOD]+params->gen_lfo[GEN_VIBMOD]/100);
    return pow(2,cents/1200);
}

float dsp_oscillator_nyquist_gain(float frequency) {
    /* The basic sine keeps one partial. The top pitch range culls it, and the
       range below interpolates toward that empty table. */
    float nyquist=(float)(SR*.5);
    float mag=fabsf(frequency);
    if (!(mag<nyquist)) return 0;
    if (mag<=nyquist*exp2f(-1.f/3.f)) return 1;
    return 3.f*log2f(nyquist/mag);
}

void dsp_oscillator_onset(DspState *state,double onset) {
    if (!isfinite(onset*SR)) return;
    double fm_frame=ceil(onset*SR-1e-7);
    /* Upstream modulators start immediately but are first pulled in the
       carrier's onset quantum; skipped silent quanta do not advance phase. */
    unsigned fm_skip_frames=(unsigned)fmod(fmax(0,fm_frame),128);
    state->fm_initial_frames=128-fm_skip_frames;
    if (!state->pitch_active) return;
    double frame=onset*SR;
    if (!isfinite(frame)) return;
    double nearest=round(frame);
    if (fabs(frame-nearest)<=10*DBL_EPSILON*fmax(1,fabs(frame))) frame=nearest;
    double first=ceil(frame),offset=fmod(first,128);
    if (offset<0) offset+=128;
    state->pitch_onset_fraction=(first-frame)/SR;
    state->pitch_quantum_offset=offset/SR;
    state->pitch_initial_frames=128-(unsigned)offset;
}

double dsp_frequency_traj(DspState *s, double freq, double age, double gate,
    const double *fm_traj) {
    if (!s->control.enabled) return freq;
    const double *p = s->control.p;
    /* No pitch envelope/vibrato and no FM operator: the modulation sum is
       exactly zero, so the result is freq unchanged (freq is bounded well
       inside the clamp range by the event layer). */
    if (!s->pitch_active && !s->fm_active) return freq;
    double base=freq;
    if (s->pitch_active) {
        double pitch_age=age+s->pitch_onset_fraction;
        if (s->pitch_initial_frames) {
            if (age*SR+1e-7<s->pitch_initial_frames) pitch_age-=s->pitch_quantum_offset;
            else s->pitch_initial_frames=0;
        }
        if (pitch_age>=0) {
            if (s->control.vib_lfo && p[DSP_VIB] > 0) {
                /* A generic vib-rate LFO bends the vibrato oscillator's
                   frequency AudioParam, which the deployed node integrates
                   per sample. The static rate term keeps the closed form
                   (with onset alignment); only the LFO deviation integrates
                   from the voice start. */
                double depth=p[DSP_VIBMOD]+s->control.gen_lfo[GEN_VIBMOD]/100;
                freq*=pitch_env_mult(p,pitch_age,gate);
                freq*=pow(2,sin(2*M_PI*pitch_age*p[DSP_VIB]+s->vib_phase)*depth/12);
                s->vib_phase += 2*M_PI*s->control.gen_lfo[GEN_VIB]/SR;
                if (s->vib_phase > 2*M_PI || s->vib_phase < -2*M_PI)
                    s->vib_phase = fmod(s->vib_phase,2*M_PI);
            } else freq*=dsp_pitch_multiplier(&s->control,pitch_age,gate);
        }
    }
    if (!s->fm_active) return freq;
    /* WebAudio pulls complete modulator quanta. Fixed voice-owned buffers
       preserve graph dependency order and feedback without callback allocation.
       OscillatorNode frequency is clamped to ±Nyquist; the constant depth gain
       keeps the unclamped harmonicity product. Noise modulators loop the same
       two-second buffers as source noise and have no frequency input. */
    if (s->fm_frame>=128) {
        const float nyquist=(float)(SR*.5);
        /* An enveloped operator's GainNode stays at its default of 1 until
           setValueAtTime(begin). In the onset quantum that begins before the
           note, upstream operators are pulled for that head and advance the
           phases of the operators they feed. The carrier does not read its
           own modulator until the note, so a direct envelope is unchanged. */
        if (s->fm_initial_frames && s->fm_initial_frames<128) {
            unsigned skip=128-s->fm_initial_frames;
            float head[8][128];
            memset(head,0,sizeof head);
            for (unsigned at=0;at<s->fm_count;at++) {
                unsigned i=s->fm_order[at];
                int shape=(int)p[DSP_FMBASE+i*8+7];
                /* BufferSource noise has no oscillator frequency input. Its
                   buffer cursor is consumed by the regular quantum below,
                   not by the oscillator-only silent-head phase correction. */
                if (shape>=4 && shape<=6) continue;
                int feeds=0, incoming=0;
                for (unsigned j=1;j<9;j++) if (s->control.matrix[i][j]) feeds=1;
                for (unsigned source=0;source<8;source++) if (s->control.matrix[source][i+1] &&
                    (s->control.fm_envelopes&(1u<<source)) &&
                    s->control.p[DSP_FMBASE+source*8+7]<4) {
                    for (unsigned j=1;j<9;j++) if (s->control.matrix[source][j]) incoming=1;
                }
                if (!(feeds && (s->control.fm_envelopes&(1u<<i))) && !incoming) continue;
                const double *op=p+DSP_FMBASE+i*8;
                for (unsigned frame=0;frame<skip;frame++) {
                    /* A generic ratio LFO bends the operator frequency in Hz. */
                    float frequency=(float)(base*op[1])+(float)s->control.gen_lfo[GEN_FMH1+i];
                    if (shape<4 || shape>6) for (unsigned source=0;source<8;source++) {
                        /* A generic index LFO rides the registered edge only. */
                        double gen=s->control.fm_gen_edge[source]==(signed char)(i+2) ?
                            s->control.gen_lfo[GEN_FMI1+source] : 0;
                        double index=s->control.matrix[source][i+1]+gen;
                        if (index) frequency+=(float)((float)(head[source][frame]*(float)index)*
                            (float)(base*p[DSP_FMBASE+source*8+1]));
                    }
                    if (frequency>nyquist) frequency=nyquist;
                    else if (frequency<-nyquist) frequency=-nyquist;
                    double phase=s->fm_phase[i],wave=sin(phase);
                    switch (shape) {
                    case 1:
                        wave=dsp_oscillator(2,phase,frequency,.5);
                        break;
                    case 2:
                        wave=dsp_oscillator(1,phase,frequency,.5);
                        break;
                    case 3:
                        wave=dsp_oscillator(0,phase,frequency,.5);
                        break;
                    default:
                        if (shape<4 || shape>6) {
                            float gain=dsp_oscillator_nyquist_gain(frequency);
                            if (gain!=1.f) wave*=gain;
                        }
                        break;
                    }
                    head[i][frame]=(float)wave;
                    s->fm_phase[i]=fmod(phase+2*M_PI*frequency/SR,2*M_PI);
                }
            }
        }
        for (unsigned at=0;at<s->fm_count;at++) {
            unsigned i=s->fm_order[at];
            const double *op=p+DSP_FMBASE+i*8;
            int shape=(int)op[7];
            for (unsigned frame=0;frame<128;frame++) {
                /* A generic ratio LFO bends the operator frequency in Hz.
                   The trajectory carries the per-frame worklet output; the
                   fallback holds the voice-stepped sample value. */
                float lfo_h = fm_traj ? (float)fm_traj[(8+i)*128+frame] :
                    (float)s->control.gen_lfo[GEN_FMH1+i];
                float frequency=(float)(base*op[1])+lfo_h;
                /* A noise BufferSource has no frequency parameter, so matrix
                   sends into it are dropped. */
                if (shape<4 || shape>6) for (unsigned source=0;source<8;source++) {
                    /* A generic index LFO rides the registered edge only. */
                    double gen=s->control.fm_gen_edge[source]==(signed char)(i+2) ?
                        (fm_traj ? fm_traj[source*128+frame] :
                            s->control.gen_lfo[GEN_FMI1+source]) : 0;
                    double index=s->control.matrix[source][i+1]+gen;
                    if (index) frequency+=(float)((float)(s->fm_buffer[source][frame]*(float)index)*
                        (float)(base*p[DSP_FMBASE+source*8+1]));
                }
                if (frequency>nyquist) frequency=nyquist;
                else if (frequency<-nyquist) frequency=-nyquist;
                double phase=s->fm_phase[i],wave=sin(phase);
                switch (shape) {
                case 1:
                    wave=dsp_oscillator(2,phase,frequency,.5);
                    break;
                case 2:
                    wave=dsp_oscillator(1,phase,frequency,.5);
                    break;
                case 3:
                    wave=dsp_oscillator(0,phase,frequency,.5);
                    break;
                case 4:
                case 5:
                case 6: {
                    const float *buffer=s->control.fm_noise[shape-4];
                    double pos=s->fm_noise_pos[i];
                    unsigned a=(unsigned)pos;
                    if (a>=88200) a=0;
                    unsigned b=a+1<88200?a+1:0;
                    double fraction=pos-a;
                    wave=buffer?buffer[a]+(buffer[b]-buffer[a])*fraction:0;
                    pos+=1;
                    if (pos>=88200) pos-=88200;
                    s->fm_noise_pos[i]=pos;
                    break;
                }
                default: {
                    float gain=dsp_oscillator_nyquist_gain(frequency);
                    if (gain!=1.f) wave*=gain;
                    break;
                }
                }
                double env_age=age+frame/SR;
                /* BufferSource output retains the complete onset quantum.
                   Its GainNode uses the default gain before the envelope's
                   first event, even though the carrier starts mid-quantum. */
                if (shape>=4 && shape<=6 && s->fm_initial_frames)
                    env_age-=(128-s->fm_initial_frames)/SR;
                double env=(s->control.fm_envelopes&(1u<<i))?
                    (env_age<0?1:scheduled_envelope(env_age,gate,0,op[6]>0,0,1,op[2],op[3],op[4],op[5])):1;
                s->fm_buffer[i][frame]=(float)((float)wave*(float)env);
                s->fm_phase[i]=fmod(phase+2*M_PI*frequency/SR,2*M_PI);
            }
        }
        s->fm_frame=0;
    }
    float mod=0;
    for (unsigned i=0;i<8;i++) if (s->control.matrix[i][0]) {
        /* A generic index LFO rides the registered carrier edge only. */
        double gen=s->control.fm_gen_edge[i]==1 ? s->control.gen_lfo[GEN_FMI1+i] : 0;
        mod+=(float)((float)(s->fm_buffer[i][s->fm_frame]*(float)(s->control.matrix[i][0]+gen))*
            (float)(base*p[DSP_FMBASE+i*8+1]));
    }
    s->fm_frame++;
    if (s->fm_initial_frames && !--s->fm_initial_frames) s->fm_frame=128;
    /* Values inside ±Nyquist keep the established expression. The caller
       clamps this frequency parameter and applies the sine-table fade. */
    return base?freq*(1+(double)mod/base):freq;
}

double dsp_frequency(DspState *s, double freq, double age, double gate) {
    return dsp_frequency_traj(s, freq, age, gate, NULL);
}

static double mod_wave(int shape, double skew, double phase) {
    phase -= floor(phase);
    double x = phase < skew ? .5*phase/skew : .5 + .5*(phase-skew)/(1-skew);
    switch (shape) {
    case 1: return 1 - fabs(2*x - 1);
    case 2: return x < .5 ? 1 : 0;
    case 3: return 1 - x;
    case 4: return x;
    default: return .5 + .5*sin(2*M_PI*x);
    }
}

/* Transient shaper matching the deployed transient-processor worklet
   verbatim: dual envelope followers (attack .003 s, sustain .08 s),
   peakiness ratio with a 0.5+5*sensitivity scaling, dB gains of 18/36,
   a slow makeup normalizer and a final soft clip. The node sits before
   the voice gain in the deployed chain, so the folded static gain is
   divided out and back; only the soft clip is level-sensitive. The
   envelopes live in float32 like the worklet's Float32Array. */
static double transient_process(DspState *s, double x) {
    double attack = s->control.p[DSP_TRANSIENT];
    if (attack < -1) attack = -1;
    if (attack > 1) attack = 1;
    double sustain = s->control.p[DSP_TRANSSUSTAIN];
    if (sustain < -1) sustain = -1;
    if (sustain > 1) sustain = 1;
    double attackCoeff = 1-exp(-1/(SR*.003));
    double sustainCoeff = 1-exp(-1/(SR*.08));
    double gainCoeff = 1-exp(-1/(SR*.2));
    /* Sensitivity and mix have no user control; the deployed defaults
       give scaling 0.5+5*.1 = 1 and a fully wet mix. */
    double G = s->control.transient_gain;
    double u = G != 0 ? x/G : x;
    double ax = fabs(u);
    float att = (float)((double)s->trans_att+(ax-(double)s->trans_att)*attackCoeff);
    float sus = (float)((double)s->trans_sus+(ax-(double)s->trans_sus)*sustainCoeff);
    s->trans_att = att;
    s->trans_sus = sus;
    double peakiness = ((double)att-(double)sus)/((double)sus+1e-6);
    if (peakiness < -1.5) peakiness = -1.5;
    if (peakiness > 1.5) peakiness = 1.5;
    double attScale = peakiness > 0 ? peakiness : 0;
    double susScale = peakiness < 0 ? -peakiness : 0;
    double attackGain = pow(10,(attack*attScale*18)/20);
    double sustainGain = pow(10,(sustain*susScale*36)/20);
    double gain = attackGain*sustainGain;
    if (gain < 0) gain = 0;
    if (gain > 8) gain = 8;
    s->trans_avg += (gain-s->trans_avg)*gainCoeff;
    double makeup = s->trans_avg > 1e-3 ? 1/s->trans_avg : 1;
    double wet = u*gain*makeup;
    double y = wet/(1+fabs(wet));
    return G != 0 ? y*G : y;
}

static double dsp_process_impl(DspState *s, double x, double age, double gate) {
    if (!s->control.enabled) return x;
    const double *p = s->control.p;
    int update = (s->tick++ & 31) == 0;
    if (s->control.lfo_sample_rate) update=1;
    /* Nothing configured: the full path reduces to the post gain (1) and the
       output guard. Keep tick advancing so state evolution is unchanged. */
    if (!s->effects_active) return isfinite(x) ? clamp(x, -64, 64) : 0;
    /* The deployed transient node leaves its first render quantum
       unwritten (currentTime <= begin) and never steps its envelopes
       there. The countdown is set at trigger from the voice start. */
    if (s->control.transient_on) {
        if (s->trans_hold > 0) { s->trans_hold--; return 0.0; }
        x = transient_process(s, x);
    }
    for (int type = 0; type < 3; type++) {
        int base = type * 7;
        if (p[base] <= 0) continue;
        if (update) {
            double hz = dsp_filter_frequency(&s->control,type,age,gate);
            int lfo = DSP_LPRATE+type*7;
            if (p[lfo] > 0) {
                double rate=p[lfo];
                double phase=age*rate;
                int active=1;
                if (s->control.lfo_start_time >= 0) {
                    /* The deployed LFO worklet leaves its output at zero for
                       the render quantum containing the node start, then
                       begins at the absolute start phase. The previous output
                       history is damped at that connection so the
                       direct-form-I state step matches the deployed filter. */
                    double quantum=128.0/SR;
                    double rel=fmod(s->control.lfo_start_time,quantum);
                    double hold=rel>0?quantum-rel:quantum;
                    if (age < hold) active=0;
                    else {
                        phase=(s->control.lfo_start_time+age-hold)*rate;
                        if (!s->lfo_switched) {
                            DspBiquad *f=&s->filter[type*2];
                            f->y1*=.77;
                            f->y2*=.77;
                            s->lfo_switched=1;
                        }
                    }
                }
                if (active) {
                    double wave = mod_wave((int)p[lfo+3],p[lfo+5],phase)+p[lfo+4];
                    double sc = p[lfo+2] > 0 ? p[lfo+2] : p[base]*p[lfo+1];
                    double g = wave*sc;
                    double curve = p[lfo+6] > 0 ? p[lfo+6] : 1;
                    if (curve != 1.0) g = pow(g,curve);
                    if (!isfinite(g)) {
                        /* Non-finite LFO output reads the BiquadFilter
                           frequency default on the deployed node. */
                        hz = 1000.0;
                    } else {
                        /* Generic cutoff LFOs connect straight to the filter
                           frequency AudioParam: clamp like getRangeForParam
                           ([20-base, 24000-base] at or above 30 Hz). */
                        double mn, mx;
                        if (p[base] >= 30) { mn = 20-p[base]; mx = 24000-p[base]; }
                        else { mn = p[lfo+4]*sc; mx = mn+sc; }
                        if (g < mn) g = mn;
                        if (g > mx) g = mx;
                        hz += g;
                    }
                }
            }
            if (type == 0 && p[DSP_FTYPE] == 1) {
                double g = tan(M_PI*clamp(hz,20,SR*.45)/SR);
                s->ladder_g = g/(1+g);
                s->ladder_k = clamp((p[base+1]+s->control.q_lfo[type])/12,0,3.95);
            }
            {
                double q = p[base+1] + s->control.q_lfo[type];
                coefficients(&s->filter[type*2], type, hz, q);
                coefficients(&s->filter[type*2+1], type, hz, q);
            }
        }
        if (type == 0 && p[DSP_FTYPE] == 1) x = ladder_filter(s,x);
        else if (s->control.lfo_sample_rate)
            x = biquad_df1(&s->filter[type*2], x);
        else x = biquad(&s->filter[type*2], x);
        if (p[DSP_FTYPE] >= 2) {
            if (s->control.lfo_sample_rate)
                x = biquad_df1(&s->filter[type*2+1], x);
            else x = biquad(&s->filter[type*2+1], x);
        }
    }
    if (p[DSP_VOWEL] >= 0) {
        double vg = s->control.gen_lfo[GEN_VOWEL];
        if (vg != 0) {
            /* A generic vowel LFO shifts every formant by the same Hz
               offset, matching the deployed connection to all five
               filter frequencies. */
            int vowel = (int)p[DSP_VOWEL];
            if (vowel >= 0 && vowel < 15)
                for (int i = 0; i < 5; i++) {
                    double w = 2*M_PI*clamp(vowel_freqs[vowel][i]+vg,20,SR*.45)/SR;
                    double alpha = sin(w)/(2*vowel_qs[vowel][i]);
                    double a0 = 1+alpha, c = cos(w);
                    s->formant[i].b0 = alpha/a0; s->formant[i].b1 = 0; s->formant[i].b2 = -alpha/a0;
                    s->formant[i].a1 = -2*c/a0; s->formant[i].a2 = (1-alpha)/a0;
                }
        }
        double sum = 0;
        for (int i = 0; i < 5; i++) sum += biquad(&s->formant[i], x)*s->formant_gain[i];
        x = sum*8;
    }
    int coarse = (int)(p[DSP_COARSE]+s->control.gen_lfo[GEN_COARSE]);
    if (coarse > 1) {
        if ((s->tick - 1) % (unsigned)coarse == 0) s->held = x;
        x = s->held;
    }
    if (p[DSP_CRUSH]+s->control.gen_lfo[GEN_CRUSH] < 16) {
        double steps = pow(2, p[DSP_CRUSH]+s->control.gen_lfo[GEN_CRUSH] - 1);
        x = round(x * steps) / steps;
    }
    if (p[DSP_SHAPE]+s->control.gen_lfo[GEN_SHAPE] > 0) {
        double sh = p[DSP_SHAPE]+s->control.gen_lfo[GEN_SHAPE];
        /* The deployed worklet clamps the summed shape below 1 and the
           summed postgain to [0.001, 1] before the curve. */
        if (sh > 1.0-4e-10) sh = 1.0-4e-10;
        double k = 2*sh/(1-sh);
        double svol = p[DSP_SHAPEVOL]+s->control.gen_lfo[GEN_SHAPEVOL];
        if (svol < 0.001) svol = 0.001; else if (svol > 1) svol = 1;
        x = (1+k)*x/(1+k*fabs(x))*svol;
    }
    if (p[DSP_DISTORT]+s->control.gen_lfo[GEN_DISTORT] > 0) {
        double dist = p[DSP_DISTORT]+s->control.gen_lfo[GEN_DISTORT];
        int dtype = (int)(p[DSP_DISTTYPE]+s->control.gen_lfo[GEN_DISTTYPE]);
        if (dtype == 0) {
            double shape=expm1(dist);
            double postgain=clamp(p[DSP_DISTVOL]+s->control.gen_lfo[GEN_DISTVOL],.001,1);
            x=((1+shape)*x/(1+shape*fabs(x)))*postgain;
        } else {
            double input = x*(1+dist*8);
            switch (dtype) {
            case 1: x = fabs(fmod(input+3,4)-2)-1; break;
            case 2: x = cos(clamp(dist,1,32)*acos(clamp(x,-1,1))); break;
            case 3: x = input/(1+fabs(input)); break;
            case 4: x = input < 0 ? tanh(input*.5)*.5 : tanh(input); break;
            case 5: x = tanh(input+.5)-tanh(.5); break;
            case 6: x = sin(input); break;
            default: x = tanh(input); break;
            }
            x *= clamp(p[DSP_DISTVOL]+s->control.gen_lfo[GEN_DISTVOL],.001,1);
        }
    }
    if (p[DSP_TREM]+s->control.gen_lfo[GEN_TREM_RATE] > 0) {
        double rate = p[DSP_TREM]+s->control.gen_lfo[GEN_TREM_RATE];
        double depth = p[DSP_TDEPTH];
        double skew = p[DSP_TSKEW]+s->control.gen_lfo[GEN_TREM_SKEW];
        double phase = p[DSP_TPHASE];
        int shape = (int)(p[DSP_TSHAPE]+s->control.gen_lfo[GEN_TREM_SHAPE]);
        /* The deployed tremolo LFO hard-codes curve 1.5, dcoffset 0 and a
           [0, 1] output clamp on an amGain base of max(1-depth, 0). Like
           every lfo-processor node it leaves its first render quantum
           unwritten and phases from the quantum end. A modulated rate is
           integrated like the worklet's per-sample phase advance instead
           of multiplying the absolute age. */
        double quantum = 128.0/SR;
        double hold = quantum;
        if (s->control.lfo_start_time >= 0) {
            double rel = fmod(s->control.lfo_start_time, quantum);
            hold = rel > 0 ? quantum-rel : quantum;
        }
        double lfo = 0.0;
        if (age >= hold) {
            s->trem_fm_phase += (rate - p[DSP_TREM]) / SR;
            double wave = mod_wave(shape, skew, (age-hold)*p[DSP_TREM] + s->trem_fm_phase + phase);
            lfo = pow(fmax(wave*depth, 0.0), 1.5);
            if (!(lfo <= 1.0)) lfo = lfo > 1.0 ? 1.0 : 0.0;
        }
        double mbase = depth < 1.0 ? 1.0-depth : 0.0;
        x *= mbase + lfo + s->control.gen_lfo[GEN_TREM_GAIN];
    }
    if (s->control.compressor_on) {
        double threshold = p[DSP_COMP]+s->control.gen_lfo[GEN_COMP];
        if (threshold < 0) {
            double ratio = p[DSP_COMPRATIO]+s->control.gen_lfo[GEN_COMPRATIO];
            double knee = p[DSP_COMPKNEE]+s->control.gen_lfo[GEN_COMPKNEE];
            double level = 20*log10(fmax(1e-12,fabs(x)));
            double over = level-threshold, reduction = 0;
            if (knee > 0 && over > -knee*.5 && over < knee*.5)
                reduction = (1-1/ratio)*(over+knee*.5)*(over+knee*.5)/(2*knee);
            else if (over > 0) reduction = over*(1-1/ratio);
            double desired = pow(10,-reduction/20);
            double time = desired < s->compressor_gain ? p[DSP_COMPATT]+s->control.gen_lfo[GEN_COMPATT] : p[DSP_COMPREL]+s->control.gen_lfo[GEN_COMPREL];
            double speed = time > 0 ? exp(-1/(time*SR)) : 0;
            s->compressor_gain = desired + speed*(s->compressor_gain-desired);
            x *= s->compressor_gain;
        }
        /* Structural DynamicsCompressorNode latency: the delayed signal feeds
           the downstream nodes (phaser/post) at absolute age, like the
           deployed chain where the compressor node delays everything before
           it while downstream LFOs stay timed to the hap start. */
        double delayed = s->comp_delay[s->comp_pos];
        s->comp_delay[s->comp_pos] = x;
        s->comp_pos = (s->comp_pos+1) % DSP_COMP_DELAY;
        x = delayed;
    }
    /* The deployed phaser node only exists while the static depth is
       positive; generic depth LFOs only route in that case too. */
    if ((p[DSP_PHASER] != 0 || s->control.gen_lfo[GEN_PHRATE] != 0 ||
        s->control.gen_lfo[GEN_PHSWEEP] != 0 ||
        s->control.gen_lfo[GEN_PHCENTER] != 0 ||
        s->control.gen_lfo[GEN_PHDEPTH] != 0) && p[DSP_PHDEPTH] > 0) {
        double rate = p[DSP_PHASER]+s->control.gen_lfo[GEN_PHRATE];
        double sweep = p[DSP_PHSWEEP];
        double center = p[DSP_PHCENTER];
        double depth = p[DSP_PHDEPTH];
        /* The deployed phaser is a single notch (center + 282 Hz) whose
           detune in cents is driven by a triangle LFO (skew .5, dcoffset
           -.5, depth sweep*2, curve 1), with a first-quantum hold and an
           integrated phase like every lfo-processor node. */
        double quantum = 128.0/SR;
        double hold = quantum;
        if (s->control.lfo_start_time >= 0) {
            double rel = fmod(s->control.lfo_start_time, quantum);
            hold = rel > 0 ? quantum-rel : quantum;
        }
        double lfo = 0.0;
        if (age >= hold) {
            s->phaser_fm_phase += rate / SR;
            double u = s->phaser_lfo_phase + s->phaser_fm_phase;
            u -= floor(u);
            double wave = (u < 0.5) ? u*2.0 : 2.0-2.0*u;
            lfo = (wave-0.5)*(sweep*2.0+s->control.gen_lfo[GEN_PHSWEEP]);
            if (lfo < -sweep) lfo = -sweep;
            else if (lfo > sweep) lfo = sweep;
        }
        double q = 2.0 - fmin(fmax(depth*2.0, 0.0), 1.9) + s->control.gen_lfo[GEN_PHDEPTH];
        double hz = (center+282.0+s->control.gen_lfo[GEN_PHCENTER])*pow(2.0, lfo/1200.0);
        hz = clamp(hz, 20.0, SR*.4);
        double w0 = 2*M_PI*hz/SR, c = cos(w0);
        double alpha = sin(w0)/(2.0*(q > 0.05 ? q : 0.05));
        double b0 = 1.0, b1 = -2.0*c, b2 = 1.0;
        double a0 = 1.0+alpha, a1n = -2.0*c, a2 = 1.0-alpha;
        double y = (b0*x + b1*s->phaser_z[0] + b2*s->phaser_z[1] - a1n*s->phaser_z[2] - a2*s->phaser_z[3]) / a0;
        if (!isfinite(y)) y = 0;
        s->phaser_z[1] = s->phaser_z[0]; s->phaser_z[0] = x;
        s->phaser_z[3] = s->phaser_z[2]; s->phaser_z[2] = y;
        x = y;
    }
    x *= p[DSP_POST]+s->control.gen_lfo[GEN_POSTGAIN];
    return isfinite(x) ? clamp(x, -64, 64) : 0;
}

#ifdef TOMPOUCE_RENDER_PROFILE
static unsigned long long dsp_cycles(void) {
    unsigned int low, high;
    __asm__ __volatile__("rdtsc" : "=a"(low), "=d"(high));
    return ((unsigned long long)high << 32) | low;
}
void dsp_profile_stats(unsigned long long *calls, unsigned long long *total_cycles,
                       unsigned long long *max_cycles) {
    if (calls) *calls=atomic_load(&dsp_calls);
    if (total_cycles) *total_cycles=atomic_load(&dsp_total_ns);
    if (max_cycles) *max_cycles=atomic_load(&dsp_max_ns);
}
double dsp_process(DspState *s, double x, double age, double gate) {
    unsigned long long started=dsp_cycles();
    double result=dsp_process_impl(s,x,age,gate);
    unsigned long long elapsed=dsp_cycles()-started;
    atomic_fetch_add(&dsp_calls,1);
    atomic_fetch_add(&dsp_total_ns,elapsed);
    unsigned long long previous=atomic_load(&dsp_max_ns);
    while (elapsed>previous &&
           !atomic_compare_exchange_weak(&dsp_max_ns,&previous,elapsed)) {}
    return result;
}
#else
double dsp_process(DspState *s, double x, double age, double gate) {
    return dsp_process_impl(s,x,age,gate);
}
#endif

int dsp_process_stereo_filter(DspState *left, DspState *right,
    double xl, double xr, double age, double gate, double *yl, double *yr) {
    int type=dsp_single_filter(left);
    if (type<0 || dsp_single_filter(right)!=type) return 0;
    const int at=type*2;
    if ((left->tick++ &31)==0) {
        double hz=dsp_filter_frequency(&left->control,type,age,gate);
        coefficients(&left->filter[at],type,hz,left->control.p[type*7+1]);
    }
    if ((right->tick++ &31)==0) {
        double hz=dsp_filter_frequency(&right->control,type,age,gate);
        coefficients(&right->filter[at],type,hz,right->control.p[type*7+1]);
    }
    DspBiquad2 state;
    DspBiquad pair[2]={left->filter[at],right->filter[at]};
    dsp_biquad2_init(&state,pair);
    double in[2]={xl,xr}, out[2];
    dsp_biquad2_process(&state,in,out,1);
    left->filter[at].z1=state.z1[0]; left->filter[at].z2=state.z2[0];
    right->filter[at].z1=state.z1[1]; right->filter[at].z2=state.z2[1];
    *yl=isfinite(out[0])?clamp(out[0],-64,64):0;
    *yr=isfinite(out[1])?clamp(out[1],-64,64):0;
    return 1;
}

int dsp_stereo_filter_step(DspState *left, DspState *right, DspBiquad2 *state,
    int type, double xl, double xr, double age, double gate,
    double *yl, double *yr) {
    if (!left || !right || !state || type<0 || type>2) return 0;
    const int at=type*2;
    if ((left->tick++ &31)==0) {
        DspBiquad f;
        double hz=dsp_filter_frequency(&left->control,type,age,gate);
        coefficients(&f,type,hz,left->control.p[type*7+1]);
        state->b0[0]=f.b0; state->b1[0]=f.b1; state->b2[0]=f.b2;
        state->a1[0]=f.a1; state->a2[0]=f.a2;
    }
    if ((right->tick++ &31)==0) {
        DspBiquad f;
        double hz=dsp_filter_frequency(&right->control,type,age,gate);
        coefficients(&f,type,hz,right->control.p[type*7+1]);
        state->b0[1]=f.b0; state->b1[1]=f.b1; state->b2[1]=f.b2;
        state->a1[1]=f.a1; state->a2[1]=f.a2;
    }
    double in[2]={xl,xr}, out[2];
    dsp_biquad2_process(state,in,out,1);
    left->filter[at].z1=state->z1[0]; left->filter[at].z2=state->z2[0];
    right->filter[at].z1=state->z1[1]; right->filter[at].z2=state->z2[1];
    *yl=isfinite(out[0])?clamp(out[0],-64,64):0;
    *yr=isfinite(out[1])?clamp(out[1],-64,64):0;
    return 1;
}

double dsp_noise(DspState *s, int color) {
    unsigned seed = s->noise_seed;
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    s->noise_seed = seed;
    double white = (double)seed/2147483648.0-1;
    if (color == 2) {
        s->noise[7] = (s->noise[7]+white*.02)/1.02;
        return s->noise[7]*3.5;
    }
    if (color != 1) return white;
    double *b = s->noise;
    b[0] = .99886*b[0]+white*.0555179;
    b[1] = .99332*b[1]+white*.0750759;
    b[2] = .969*b[2]+white*.153852;
    b[3] = .8665*b[3]+white*.3104856;
    b[4] = .55*b[4]+white*.5329522;
    b[5] = -.7616*b[5]-white*.016898;
    double pink = b[0]+b[1]+b[2]+b[3]+b[4]+b[5]+b[6]+white*.5362;
    b[6] = white*.115926;
    return pink*.11;
}

static double poly_blep(double phase, double step) {
    if (step <= 0) return 0;
    if (phase < step) { double t = phase/step; return t+t-t*t-1; }
    if (phase > 1-step) { double t = (phase-1)/step; return t*t+t+t+1; }
    return 0;
}

double dsp_periodic_gain(int waveform) {
    /* Chromium normalizes its 4096-point periodic table's full-partial peak.
       These are the reciprocal Fourier peaks, including Gibbs overshoot;
       keep raw polyBLEP/Kabelsalat/supersaw amplitudes separate. */
    return waveform==0?.8485424501794022:waveform==1?.8481909356632208:1;
}

double dsp_oscillator(int waveform, double phase, double frequency, double width) {
    double t = phase/(2*M_PI); t -= floor(t);
    double step = fmin(.49,fabs(frequency)/SR);
    if (waveform == 0) {
        /* Saw discontinuity at half-cycle, with the WebAudio sine phase. */
        double shifted = t+.5; shifted -= floor(shifted);
        return 2*shifted-1-poly_blep(shifted,step);
    }
    if (waveform == 1) {
        width = clamp(width,.01,.99);
        double edge = t-width; edge -= floor(edge);
        return (t < width ? 1 : -1)+poly_blep(t,step)-poly_blep(edge,step);
    }
    return 2/M_PI*asin(sin(phase));
}

void dsp_supersaw_init(DspState *s, unsigned seed) {
    double voices=clamp(s->control.p[DSP_UNISON],1,DSP_SUPERSAW_VOICES);
    s->supersaw_voices=(unsigned)ceil((float)voices);
    double spread=voices>1?clamp(s->control.p[DSP_SPREAD],0,1):0;
    s->supersaw_pan_l=sqrt((1-spread)*.5);
    s->supersaw_pan_r=sqrt((1+spread)*.5);
    s->supersaw_gain=1/sqrt(voices);
    /* AudioWorklet parameters are Float32, including fractional voice counts. */
    double count=(float)voices, width=(float)s->control.supersaw_detune;
    for (unsigned i=0;i<s->supersaw_voices;i++) {
        seed=seed*1664525u+1013904223u;
        s->saw_phase[i]=(double)seed/4294967296.0;
        double offset=count>1?i*width/(count-1)-width*.5:0;
        s->saw_ratio[i]=pow(2,offset/12);
        if (!isfinite(s->saw_ratio[i])) s->saw_ratio[i]=0;
    }
}

double dsp_supersaw(DspState *s, double frequency) {
    double left = 0, right = 0;
    double pan_l=s->supersaw_pan_l,pan_r=s->supersaw_pan_r;
    /* A generic spread LFO modulates the deployed panspread per sample;
       recompute the voice gains from the widened/narrowed spread. */
    double ds = s->control.gen_lfo[GEN_SPREAD];
    if (ds != 0 && isfinite(ds)) {
        double voices=clamp(s->control.p[DSP_UNISON],1,DSP_SUPERSAW_VOICES);
        double spread=voices>1?clamp(s->control.p[DSP_SPREAD]+(float)ds,0,1):0;
        pan_l=sqrt((1-spread)*.5); pan_r=sqrt((1+spread)*.5);
    }
    /* A generic detune LFO modulates the deployed freqspread per sample;
       recompute the voice ratios from the widened/narrowed total. */
    double dg = s->control.gen_lfo[GEN_DETUNE];
    if (dg != 0) {
        double voices=clamp(s->control.p[DSP_UNISON],1,DSP_SUPERSAW_VOICES);
        double count=(float)voices, width=(float)(s->control.supersaw_detune+dg);
        for (unsigned i=0;i<s->supersaw_voices;i++) {
            double offset=count>1?i*width/(count-1)-width*.5:0;
            double r=pow(2,offset/12);
            s->saw_ratio[i]=isfinite(r)?r:0;
        }
    }
    if (s->supersaw_voices >= 4) {
        double values[DSP_SUPERSAW_VOICES];
        dsp_supersaw_values(s,frequency,values);
        /* Keep the original index-order sum; the kernel only computes the
           independent oscillator values and advances their phases. */
        for (unsigned i=0;i<s->supersaw_voices;i++) {
            double x=values[i];
            left += x*pan_l; right += x*pan_r;
            double swap=pan_l; pan_l=pan_r; pan_r=swap;
        }
    } else {
        for (unsigned i = 0; i < s->supersaw_voices; i++) {
            double rate=frequency*s->saw_ratio[i]/SR, x=0;
            /* Near-one fractional unison and extreme spreads can overflow the
               exponential. Keep invalid oscillators silent and state finite. */
            if (s->saw_ratio[i]>0 && isfinite(rate)) {
                rate-=floor(rate);
                double phase=s->saw_phase[i];
                x=2*phase-1-poly_blep(phase,fmin(rate,1-rate));
                phase+=rate;
                if (phase>=1) phase-=1;
                s->saw_phase[i]=phase;
            }
            left += x*pan_l; right += x*pan_r;
            double swap=pan_l; pan_l=pan_r; pan_r=swap;
        }
    }
    s->supersaw_l = left*s->supersaw_gain;
    s->supersaw_r = right*s->supersaw_gain;
    return (s->supersaw_l+s->supersaw_r)*.5;
}

int dsp_supports_control(const char *name) {
    if (!name) return 0;
    for (int i = 0; i < DSP_FMBASE; i++) if (!strcmp(name,parameters[i].name)) return 1;
    const char *fields[] = {"fmi","fmh","fmattack","fmdecay","fmsustain","fmrelease","fmenv","fmwave",NULL};
    for (int i = 0; fields[i]; i++) {
        size_t n = strlen(fields[i]);
        if (strncmp(name,fields[i],n)) continue;
        const char *p = name+n;
        if (!*p || (p[0] >= '1' && p[0] <= '8' && !p[1])) return 1;
        if (i == 0 && p[0] >= '1' && p[0] <= '8' && p[1] >= '1' && p[1] <= '8' && !p[2]) return 1;
    }
    return !strcmp(name,"size") || !strcmp(name,"partials") || !strcmp(name,"phases") || !strcmp(name,"noise");
}

double dsp_pulse(DspState *s,double frequency) {
    double *m=s->pulse;
    double width=s->control.p[DSP_PW]+s->control.gen_lfo[GEN_PW];
    if (s->control.p[DSP_PWSWEEP]+s->control.gen_lfo[GEN_PWSWEEP]!=0) {
        double phase=s->pulse_lfo_phase;
        double tri=phase<.5?phase*2:2-phase*2;
        width=(float)((float)width+(float)((tri-.5)*(s->control.p[DSP_PWSWEEP]+s->control.gen_lfo[GEN_PWSWEEP])));
        phase+=(s->control.p[DSP_PWRATE]+s->control.gen_lfo[GEN_PWRATE])/SR;
        s->pulse_lfo_phase=phase-floor(phase);
    }
    unsigned frame=s->pulse_frame++%128;
    if (!frame) m[5]=1;
    m[5]*=.9998;
    m[3]+=.1*(frequency*2*M_PI/SR-m[3]);
    m[4]+=.1*(m[5]-m[4]);
    m[0]+=m[3];
    if (m[0]>=M_PI) m[0]-=2*M_PI;
    double feedback=fmax(0,2.3*(1-.0001*frequency));
    double first=cos(m[0]+feedback*m[1]);
    double second=cos(m[0]+feedback*m[2]+(1-clamp(width,-.99,.99))*M_PI);
    m[1]=.5*(first+m[1]); m[2]=.5*(second+m[2]);
    return .15*(first-second)*m[4];
}
