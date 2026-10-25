#include "dsp.h"
#include "common.h"
#include <float.h>

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
    {"compressor",0,-100,0}, {"compressorRatio",12,1,20}, {"compressorKnee",30,0,40},
    {"compressorAttack",.003,0,1}, {"compressorRelease",.25,0,1}, {"pcurve",0,0,1},
    {"chorus",0,0,1},{"shapevol",1,0,16},
    {"lprate",0,0,20000},{"lpdepth",1,0,16},{"lpdepthfrequency",0,0,20000},
    {"lpshape",0,0,4},{"lpdc",-.5,-1,1},{"lpskew",.5,.001,.999},
    {"hprate",0,0,20000},{"hpdepth",1,0,16},{"hpdepthfrequency",0,0,20000},
    {"hpshape",0,0,4},{"hpdc",-.5,-1,1},{"hpskew",.5,.001,.999},
    {"bprate",0,0,20000},{"bpdepth",1,0,16},{"bpdepthfrequency",0,0,20000},
    {"bpshape",0,0,4},{"bpdc",-.5,-1,1},{"bpskew",.5,.001,.999},
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
    for (int i = 0; i < DSP_FMBASE; i++) p->p[i] = parameters[i].initial;
    for (int i = 0; i < 8; i++) {
        double *op = p->p + DSP_FMBASE + i * 8;
        op[1] = 1; op[2] = .001; op[3] = .1; op[4] = 1; op[5] = .1;
    }
}

static int symbol(const cJSON *obj, const char *key, const char *const *names, int n) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(v)) return -1;
    for (int i = 0; i < n; i++) if (!strcmp(v->valuestring, names[i])) return i;
    return -1;
}

void dsp_params_read(DspParams *p, const cJSON *value) {
    dsp_params_init(p); p->enabled = 1;
    /* Superdough's supersaw uses detune ?? n ?? .18 as total frequency
       spread in semitones; other oscillators use detune as pitch cents. */
    p->supersaw_detune = number(value,"detune",number(value,"n",.18,-9600,9600),-9600,9600);
    p->noise_mix=number(value,"noise",0,0,1);
    for (int i = 0; i < DSP_FMBASE; i++)
        p->p[i] = number(value, parameters[i].name, p->p[i], parameters[i].min, parameters[i].max);
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
    const char *shape_names[] = {"lpshape","hpshape","bpshape"};
    for (int i = 0; i < 3; i++) {
        v = symbol(value,shape_names[i],waves,5);
        if (v >= 0) p->p[DSP_LPSHAPE+i*6] = v;
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
        for (int j = 0; j < 8; j++) {
            char name[32];
            snprintf(name, sizeof(name), "%s%d", fields[j], i + 1);
            double *dest = p->p + DSP_FMBASE + i * 8 + j;
            if (i == 0) *dest = number(value, fields[j], *dest, 0, maximum[j]);
            *dest = number(value, name, *dest, 0, maximum[j]);
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
        for (int j = 0; j < 8; j++) {
            char name[32];
            snprintf(name, sizeof(name), "fmi%d%d", i + 1, j + 1);
            p->matrix[i][j] = number(value, name, 0, -64, 64);
        }
    }
}

static double held_envelope(double t, double a, double d, double s) {
    if (t < a) return a > 0 ? t / a : 1;
    if (t < a + d) return 1 - (1 - s) * (t - a) / d;
    return s;
}

double dsp_envelope(double age, double gate, double a, double d, double s, double r) {
    if (age < 0) return 0;
    if (age <= gate) return held_envelope(age, a, d, s);
    if (r <= 0 || age >= gate + r) return 0;
    return held_envelope(gate, a, d, s) * (1 - (age - gate) / r);
}

static double exponential_ramp(double from, double to, double fraction) {
    from = fmax(.001,from); to = fmax(.001,to);
    return from*pow(to/from,clamp(fraction,0,1));
}

/* Match the frequency automation endpoints of Superdough getParamADSR,
   including arithmetic sustain levels and notes shorter than a segment. */
double dsp_filter_frequency(const DspParams *params, int type, double age, double gate) {
    if (!params || type < 0 || type > 2) return 0;
    const double *p = params->p;
    int base = type*7;
    double depth = fabs(p[base+2]);
    double low = fmin(20000,p[base]*pow(2,-depth*p[DSP_ANCHOR]));
    double high = fmin(20000,p[base]*pow(2,depth*(1-p[DSP_ANCHOR])));
    double start = p[base+2] < 0 ? high : low;
    double peak = p[base+2] < 0 ? low : high;
    double sustain = start+(peak-start)*p[base+5];
    double a = p[base+3], d = p[base+4], r = p[base+6];
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

static double biquad(DspBiquad *f, double x) {
    double y = f->b0 * x + f->z1;
    f->z1 = f->b1*x - f->a1*y + f->z2;
    f->z2 = f->b2*x - f->a2*y;
    if (!isfinite(y) || !isfinite(f->z1) || !isfinite(f->z2)) {
        f->z1 = f->z2 = 0; return 0;
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

void dsp_init(DspState *s, const DspParams *p) {
    memset(s, 0, sizeof(*s));
    s->control = *p;
    s->noise_seed = 0x1234567u;
    s->compressor_gain = 1;
    s->pulse[0]=-M_PI;
    /* Web vowel topology: five parallel bandpasses with per-formant gains
       and a makeup gain of 8 (packages/superdough/vowel.mjs). Q reaches 140,
       past the user-facing resonance clamp, so formants use unclamped RBJ
       bandpass coefficients identical to the WebAudio definition. */
    static const int freqs[15][5] = {
        {660,1120,2750,3000,3350},{440,1800,2700,3000,3300},{270,1850,2900,3350,3590},
        {430,820,2700,3000,3300},{370,630,2750,3000,3400},{650,1515,2400,3000,3350},
        {560,900,2570,3000,3300},{500,1430,2300,3000,3300},{250,1750,2150,3200,3300},
        {400,1460,2400,3000,3300},{600,1250,2100,3100,3500},{500,1240,2280,3000,3500},
        {600,1480,2450,3200,3300},{700,1050,2500,3000,3300},{500,1080,2350,3000,3300}
    };
    static const double gains[15][5] = {
        {1,.5012,.0708,.0631,.0126},{1,.1995,.1259,.1,.1},{1,.0631,.0631,.0158,.0158},
        {1,.3162,.0501,.0794,.01995},{1,.1,.0708,.0316,.01995},{1,.5,.1008,.0631,.0126},
        {1,.5,.0708,.0631,.0126},{1,.2,.0708,.0316,.01995},{1,.1,.0708,.0316,.01995},
        {1,.2,.0708,.0316,.02995},{1,.3,.0608,.0316,.01995},{1,.1,.1708,.0216,.02995},
        {1,.15,.0708,.0316,.02995},{1,.1,.0708,.0316,.02995},{1,.1,.0708,.0316,.02995}
    };
    static const int qs[15][5] = {
        {80,90,120,130,140},{70,80,100,120,120},{40,90,100,120,120},
        {40,80,100,120,120},{40,60,100,120,120},{80,90,120,130,140},
        {80,90,120,130,140},{40,60,100,120,120},{40,60,100,120,120},
        {40,60,100,120,120},{40,70,100,120,130},{40,60,100,120,120},
        {40,60,100,120,120},{40,60,100,120,120},{40,60,100,120,120}
    };
    int vowel = (int)p->p[DSP_VOWEL];
    if (vowel >= 0 && vowel < 15)
        for (int i = 0; i < 5; i++) {
            double w = 2*M_PI*clamp(freqs[vowel][i],20,SR*.45)/SR;
            double alpha = sin(w)/(2*qs[vowel][i]);
            double a0 = 1+alpha, c = cos(w);
            s->formant[i].b0 = alpha/a0; s->formant[i].b1 = 0; s->formant[i].b2 = -alpha/a0;
            s->formant[i].a1 = -2*c/a0; s->formant[i].a2 = (1-alpha)/a0;
            s->formant[i].z1 = s->formant[i].z2 = 0;
            s->formant_gain[i] = gains[vowel][i];
        }
    /* Cache which per-sample paths can change the signal. The controls are a
       copy fixed for the voice's lifetime, so this is computed once here. */
    s->pitch_active = p->p[DSP_PENV] != 0 || p->p[DSP_VIB] > 0;
    s->fm_active = 0;
    for (int i = 0; i < 8; i++)
        if (p->p[DSP_FMBASE + i*8] != 0) { s->fm_active = 1; break; }
    int active = 0;
    for (int type = 0; type < 3 && !active; type++)
        if (p->p[type*7] > 0) active = 1;
    if (p->p[DSP_VOWEL] >= 0 || (int)p->p[DSP_COARSE] > 1 || p->p[DSP_CRUSH] < 16 ||
        p->p[DSP_SHAPE] > 0 || p->p[DSP_DISTORT] > 0 || p->p[DSP_TREM] > 0 ||
        p->p[DSP_COMP] < 0 || p->p[DSP_PHASER] > 0 || p->p[DSP_CHORUS] > 0 ||
        p->p[DSP_POST] != 1) active = 1;
    s->effects_active = active;
}

double dsp_pitch_multiplier(const DspParams *params, double age, double gate) {
    if (!params->enabled) return 1;
    const double *p = params->p;
    /* Neither branch below can change the ratio; skip the envelope and the
       transcendental work entirely. */
    if (p[DSP_PENV] == 0 && p[DSP_VIB] <= 0) return 1;
    double ratio = 1;
    if (p[DSP_PENV] != 0) {
        double env = dsp_envelope(age, gate, p[DSP_PATT], p[DSP_PDEC], p[DSP_PSUS], p[DSP_PREL]);
        if (p[DSP_PCURVE] > 0) env *= env;
        ratio *= pow(2, p[DSP_PENV] * (env - p[DSP_PANCHOR]) / 12);
    }
    if (p[DSP_VIB] > 0) ratio *= pow(2, sin(2*M_PI*age*p[DSP_VIB])*p[DSP_VIBMOD]/12);
    return ratio;
}

void dsp_oscillator_onset(DspState *state,double onset) {
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

double dsp_frequency(DspState *s, double freq, double age, double gate) {
    if (!s->control.enabled) return freq;
    const double *p = s->control.p;
    /* No pitch envelope/vibrato and no FM operator: the modulation sum is
       exactly zero, so the result is freq unchanged (freq is bounded well
       inside the clamp range by the event layer). */
    if (!s->pitch_active && !s->fm_active) return freq;
    if (s->pitch_active) {
        double pitch_age=age+s->pitch_onset_fraction;
        if (s->pitch_initial_frames) {
            if (age*SR+1e-7<s->pitch_initial_frames) pitch_age-=s->pitch_quantum_offset;
            else s->pitch_initial_frames=0;
        }
        if (pitch_age>=0) freq*=dsp_pitch_multiplier(&s->control,pitch_age,gate);
    }
    if (!s->fm_active) return freq;
    double modulation[8];
    for (int i = 0; i < 8; i++) {
        const double *op = p + DSP_FMBASE + i * 8;
        if (op[0] == 0) { modulation[i] = 0; continue; }
        double phase = s->fm_phase[i];
        for (int j = 0; j < 8; j++) phase += s->fm_last[j] * s->control.matrix[j][i];
        double env = dsp_envelope(age, gate, op[2], op[3], op[4], op[5]);
        if (op[6] > 0) env *= env;
        double wave = sin(phase), u = phase/(2*M_PI); u -= floor(u);
        switch ((int)op[7]) {
        case 1: wave = 4*fabs(u-.5)-1; break;
        case 2: wave = u < .5 ? 1 : -1; break;
        case 3: wave = 2*u-1; break;
        case 4: case 5: case 6: wave = dsp_noise(s,(int)op[7]-4); break;
        default: break;
        }
        modulation[i] = wave * op[0] * env;
        s->fm_phase[i] += 2*M_PI*freq*op[1]/SR;
        s->fm_phase[i] = fmod(s->fm_phase[i], 2*M_PI);
    }
    double mod = 0;
    for (int i = 0; i < 8; i++) { mod += modulation[i]; s->fm_last[i] = modulation[i]; }
    return clamp(freq * (1 + mod), -SR * 16, SR * 16);
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

double dsp_process(DspState *s, double x, double age, double gate) {
    if (!s->control.enabled) return x;
    const double *p = s->control.p;
    int update = (s->tick++ & 31) == 0;
    /* Nothing configured: the full path reduces to the post gain (1) and the
       output guard. Keep tick advancing so state evolution is unchanged. */
    if (!s->effects_active) return isfinite(x) ? clamp(x, -64, 64) : 0;
    for (int type = 0; type < 3; type++) {
        int base = type * 7;
        if (p[base] <= 0) continue;
        if (update) {
            double hz = dsp_filter_frequency(&s->control,type,age,gate);
            int lfo = DSP_LPRATE+type*6;
            if (p[lfo] > 0) {
                double wave = mod_wave((int)p[lfo+3],p[lfo+5],age*p[lfo])+p[lfo+4];
                hz += wave*(p[lfo+2] > 0 ? p[lfo+2] : p[base]*p[lfo+1]);
            }
            if (type == 0 && p[DSP_FTYPE] == 1) {
                double g = tan(M_PI*clamp(hz,20,SR*.45)/SR);
                s->ladder_g = g/(1+g);
                s->ladder_k = clamp(p[base+1]/12,0,3.95);
            }
            coefficients(&s->filter[type*2], type, hz, p[base+1]);
            coefficients(&s->filter[type*2+1], type, hz, p[base+1]);
        }
        if (type == 0 && p[DSP_FTYPE] == 1) x = ladder_filter(s,x);
        else x = biquad(&s->filter[type*2], x);
        if (p[DSP_FTYPE] >= 2) {
            x = biquad(&s->filter[type*2+1], x);
        }
    }
    if (p[DSP_VOWEL] >= 0) {
        double sum = 0;
        for (int i = 0; i < 5; i++) sum += biquad(&s->formant[i], x)*s->formant_gain[i];
        x = sum*8;
    }
    int coarse = (int)p[DSP_COARSE];
    if (coarse > 1) {
        if ((s->tick - 1) % (unsigned)coarse == 0) s->held = x;
        x = s->held;
    }
    if (p[DSP_CRUSH] < 16) {
        double steps = pow(2, p[DSP_CRUSH] - 1);
        x = round(x * steps) / steps;
    }
    if (p[DSP_SHAPE] > 0) {
        double k = 2*p[DSP_SHAPE]/(1-p[DSP_SHAPE]);
        x = (1+k)*x/(1+k*fabs(x))*p[DSP_SHAPEVOL];
    }
    if (p[DSP_DISTORT] > 0) {
        double input = x*(1+p[DSP_DISTORT]*8);
        switch ((int)p[DSP_DISTTYPE]) {
        case 1: x = fabs(fmod(input+3,4)-2)-1; break;
        case 2: x = cos(clamp(p[DSP_DISTORT],1,32)*acos(clamp(x,-1,1))); break;
        case 3: x = input/(1+fabs(input)); break;
        case 4: x = input < 0 ? tanh(input*.5)*.5 : tanh(input); break;
        case 5: x = tanh(input+.5)-tanh(.5); break;
        case 6: x = sin(input); break;
        default: x = tanh(input); break;
        }
        x *= p[DSP_DISTVOL];
    }
    if (p[DSP_TREM] > 0)
        x *= 1-p[DSP_TDEPTH] + p[DSP_TDEPTH]*mod_wave((int)p[DSP_TSHAPE],p[DSP_TSKEW],age*p[DSP_TREM]+p[DSP_TPHASE]);
    if (p[DSP_COMP] < 0) {
        double level = 20*log10(fmax(1e-12,fabs(x)));
        double over = level-p[DSP_COMP], knee = p[DSP_COMPKNEE], reduction = 0;
        if (knee > 0 && over > -knee*.5 && over < knee*.5)
            reduction = (1-1/p[DSP_COMPRATIO])*(over+knee*.5)*(over+knee*.5)/(2*knee);
        else if (over > 0) reduction = over*(1-1/p[DSP_COMPRATIO]);
        double desired = pow(10,-reduction/20);
        double time = desired < s->compressor_gain ? p[DSP_COMPATT] : p[DSP_COMPREL];
        double speed = time > 0 ? exp(-1/(time*SR)) : 0;
        s->compressor_gain = desired + speed*(s->compressor_gain-desired);
        x *= s->compressor_gain;
    }
    if (p[DSP_PHASER] > 0) {
        double hz = clamp(p[DSP_PHCENTER] + p[DSP_PHSWEEP]*sin(2*M_PI*age*p[DSP_PHASER]), 20, SR*.4);
        double t = tan(M_PI*hz/SR), a = (1-t)/(1+t), wet = x;
        for (int i = 0; i < 6; i++) {
            double y = -a*wet + s->phaser_z[i];
            s->phaser_z[i] = wet + a*y; wet = y;
        }
        x = x*(1-p[DSP_PHDEPTH]*.5) + wet*p[DSP_PHDEPTH]*.5;
    }
    if (p[DSP_CHORUS] > 0) {
        double delay = SR*(.02+.005*sin(2*M_PI*age*.8));
        /* chorus_pos and delay are both below DSP_CHORUS_LEN, so one wrap
           correction is enough and no modulo is needed in the callback. */
        double read = (double)s->chorus_pos-delay;
        if (read < 0) read += DSP_CHORUS_LEN;
        unsigned a = (unsigned)read;
        if (a >= DSP_CHORUS_LEN) a -= DSP_CHORUS_LEN;
        unsigned b = a+1 < DSP_CHORUS_LEN ? a+1 : 0;
        double f = read-floor(read);
        double wet = s->chorus[a]*(1-f)+s->chorus[b]*f;
        s->chorus[s->chorus_pos] = (float)x;
        if (++s->chorus_pos >= DSP_CHORUS_LEN) s->chorus_pos = 0;
        x = x*(1-p[DSP_CHORUS]*.5)+wet*p[DSP_CHORUS]*.5;
    }
    x *= p[DSP_POST];
    return isfinite(x) ? clamp(x, -64, 64) : 0;
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
    double width=s->control.p[DSP_PW];
    if (s->control.p[DSP_PWSWEEP]!=0) {
        double phase=s->pulse_lfo_phase;
        double tri=phase<.5?phase*2:2-phase*2;
        width=(float)((float)width+(float)((tri-.5)*s->control.p[DSP_PWSWEEP]));
        phase+=s->control.p[DSP_PWRATE]/SR;
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
