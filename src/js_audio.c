#include "js_audio.h"
#include "gm_synth.h"
#include "audio.h"
#include "audio_output.h"
#include "lv2_worker.h"
#include "live_lv2.h"
#include "scheduler.h"
#include "sampler.h"
#include "websamp.h"
#include "wavetable.h"
#include "stretch.h"
#include "prep_pool.h"
#include "prep_profile.h"
#include <ctype.h>
#include "js_number.h"

static double control(const cJSON *value, const char *name, double initial, double min, double max) {
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(value, name);
    if (!cJSON_IsNumber(p) || !isfinite(p->valuedouble)) return initial;
    return fmax(min, fmin(max, p->valuedouble));
}

static const char *text_control(const cJSON *value, const char *name) {
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(value, name);
    return cJSON_IsString(p) ? p->valuestring : "";
}

static int number_control(const cJSON *value, const char *name) {
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(value, name);
    return cJSON_IsNumber(p) && isfinite(p->valuedouble);
}

static int any_number_control(const cJSON *value, const char *const *names, int count) {
    for (int i = 0; i < count; i++)
        if (number_control(value, names[i])) return 1;
    return 0;
}

/* Superdough LFO shapes: tri/triangle, sine, ramp, saw, square. */
static int lfo_shape_control(const cJSON *value, const char *name) {
    static const char *const names[] = {"tri","triangle","sine","ramp","saw","square"};
    static const int shapes[] = {0,0,1,2,3,4};
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(value, name);
    if (cJSON_IsNumber(p) && isfinite(p->valuedouble)) {
        int shape = (int)lround(p->valuedouble);
        return shape >= 0 && shape <= 4 ? shape : 0;
    }
    if (!cJSON_IsString(p) || !p->valuestring) return 0;
    for (int i = 0; i < (int)(sizeof(names)/sizeof(names[0])); i++)
        if (!strcasecmp(p->valuestring, names[i])) return shapes[i];
    return 0;
}

/* Superdough warpMode: numbers 0..21 or their uppercase names. */
static int warp_mode_control(const cJSON *value, const char *name) {
    static const char *const names[] = {
        "NONE","ASYM","MIRROR","BENDP","BENDM","BENDMP","SYNC","QUANT",
        "FOLD","PWM","ORBIT","SPIN","CHAOS","PRIMES","BINARY","BROWNIAN",
        "RECIPROCAL","WORMHOLE","LOGISTIC","SIGMOID","FRACTAL","FLIP"
    };
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(value, name);
    if (cJSON_IsNumber(p) && isfinite(p->valuedouble)) {
        int mode = (int)lround(p->valuedouble);
        return mode >= 0 && mode < (int)(sizeof(names)/sizeof(names[0])) ? mode : 0;
    }
    if (!cJSON_IsString(p) || !p->valuestring) return 0;
    for (int i = 0; i < (int)(sizeof(names)/sizeof(names[0])); i++)
        if (!strcasecmp(p->valuestring, names[i])) return i;
    return 0;
}

static const cJSON *duck_item(const cJSON *value, const char *name, int index) {
    const cJSON *item=cJSON_GetObjectItemCaseSensitive(value,name);
    if (cJSON_IsArray(item)) {
        const cJSON *entry=cJSON_GetArrayItem(item,index);
        item=entry && !cJSON_IsNull(entry)?entry:cJSON_GetArrayItem(item,0);
    }
    return item;
}

static void duck_onset(const cJSON *value, int index, DuckControl *control) {
    const cJSON *item=duck_item(value,"duckonset",index);
    control->onset=0; control->string_onset=0; control->onset_text[0]=0;
    int array=0;
    for (int depth=0;cJSON_IsArray(item);depth++) {
        array=1;
        int count=cJSON_GetArraySize(item);
        if (depth>=128 || count>1) {control->onset=NAN;return;}
        if (!count) {item=NULL;break;}
        item=cJSON_GetArrayItem(item,0);
    }
    if (array || cJSON_IsString(item)) {
        const char *text=""; char number[32];
        if (cJSON_IsString(item)) text=item->valuestring;
        else if (cJSON_IsBool(item)) text=cJSON_IsTrue(item)?"true":"false";
        else if (cJSON_IsNumber(item)) {
            JSDTOATempMem temporary;
            js_dtoa(number,item->valuedouble,10,0,JS_DTOA_FORMAT_FREE,&temporary);
            text=number;
        } else if (item && !cJSON_IsNull(item)) {control->onset=NAN;return;}
        if (strlen(text)>=sizeof(control->onset_text)) {
            control->long_onset=duck_onset_prepare(text);
            if (!control->long_onset) {
                log_console("duck: long onset summary cache capacity exceeded");
                control->onset=NAN; return;
            }
            control->string_onset=DUCK_ONSET_SUMMARY;
        } else {
            snprintf(control->onset_text,sizeof(control->onset_text),"%s",text);
            control->string_onset=DUCK_ONSET_STRING;
        }
    } else if (cJSON_IsBool(item)) control->onset=cJSON_IsTrue(item)?1:0;
    else if (cJSON_IsNumber(item)) control->onset=item->valuedouble;
    else if (item && !cJSON_IsNull(item)) control->onset=NAN;
}

static double duck_numeric(const cJSON *value, const char *name, int index, double fallback) {
    const cJSON *item=duck_item(value,name,index);
    if (!item) return fallback;
    int array=0;
    for (int depth=0;cJSON_IsArray(item);depth++) {
        if (depth>=128) return NAN;
        int count=cJSON_GetArraySize(item);
        if (!count) return 0;
        if (count!=1) return NAN;
        item=cJSON_GetArrayItem(item,0); array=1;
    }
    if (cJSON_IsNull(item)) return 0;
    if (cJSON_IsBool(item)) return array?NAN:(cJSON_IsTrue(item)?1:0);
    if (cJSON_IsNumber(item)) return item->valuedouble;
    if (cJSON_IsString(item)) return js_number_parse(item->valuestring);
    return NAN;
}

static double duck_attack(const cJSON *value, int index) {
    const char *names[]={"duckattack","duckatt","datt"};
    for (unsigned i=0;i<sizeof(names)/sizeof(names[0]);i++) {
        const cJSON *item=cJSON_GetObjectItemCaseSensitive(value,names[i]);
        if (!item) continue;
        double number=duck_numeric(value,names[i],index,cJSON_IsArray(item)?NAN:.1);
        return isnan(number)?NAN:fmax(.002,number);
    }
    return .1;
}

static int oscillator(const char *s) {
    if (native_synth_sound(s)) return INST_NATIVE;
    if (!strcmp(s,"sine")) return INST_SINE;
    if (!strcmp(s,"saw") || !strcmp(s,"sawtooth")) return INST_SAW;
    if (!strcmp(s,"square")) return INST_SQUARE;
    if (!strcmp(s,"pulse")) return INST_PULSE;
    if (!strcmp(s,"tri") || !strcmp(s,"triangle")) return INST_TRI;
    if (!strcmp(s,"user")) return INST_TRI; /* web fallback without partials */
    if (!strcmp(s,"noise") || !strcmp(s,"white")) return INST_NOISE;
    if (!strcmp(s,"pink")) return INST_PINK;
    if (!strcmp(s,"brown")) return INST_BROWN;
    if (!strcmp(s,"supersaw")) return INST_SUPERSAW;
    if (!strncmp(s,"wt:",3)) return INST_WT;
    return -1;
}

static int duck_target(const cJSON *target) {
    /* Array property keys use Array.toString; only a chain of singleton
       arrays can produce a canonical numeric orbit key. */
    for (int depth=0;cJSON_IsArray(target);depth++) {
        if (depth>=128 || cJSON_GetArraySize(target)!=1) return -1;
        target=cJSON_GetArrayItem(target,0);
    }
    if (cJSON_IsNumber(target) && isfinite(target->valuedouble) &&
        target->valuedouble>=0 && target->valuedouble<N_ORBITS &&
        target->valuedouble==floor(target->valuedouble)) return target->valueint;
    if (cJSON_IsString(target) && target->valuestring) for (int orbit=0;orbit<N_ORBITS;orbit++) {
        char key[16]; snprintf(key,sizeof(key),"%d",orbit);
        if (!strcmp(target->valuestring,key)) return orbit;
    }
    return -1;
}

static void duck_assign(const cJSON *v, const cJSON *target, int index, AudioEvent *ev) {
    int orbit=duck_target(target);
    if (orbit<0) return;
    ev->duck_mask|=1u<<orbit; ev->duck_values=1;
    duck_onset(v,index,&ev->ducks[orbit]);
    ev->ducks[orbit].attack=duck_attack(v,index);
    ev->ducks[orbit].depth=duck_numeric(v,"duckdepth",index,1);
}

static void duck_controls(const cJSON *v, AudioEvent *ev);
int js_audio_duck_entries(const cJSON *v, int *orbits, DuckControl *controls, int capacity) {
    if (!v || !orbits || !controls || capacity <= 0) return 0;
    int count = 0;
    const cJSON *duck = cJSON_GetObjectItemCaseSensitive(v, "duckorbit");
    if (!duck) duck = cJSON_GetObjectItemCaseSensitive(v, "duck");
    if (cJSON_IsArray(duck)) {
        int index = 0;
        const cJSON *target;
        cJSON_ArrayForEach(target, duck) {
            int at = index++;
            int orbit = duck_target(target);
            if (orbit < 0 || count >= capacity) continue;
            AudioEvent scratch;
            memset(&scratch, 0, sizeof(scratch));
            duck_assign(v, target, at, &scratch);
            orbits[count] = orbit;
            controls[count] = scratch.ducks[orbit];
            count++;
        }
        return count;
    }
    int orbit = duck_target(duck);
    if (orbit < 0 || count >= capacity) return 0;
    AudioEvent event;
    memset(&event, 0, sizeof(event));
    duck_controls(v, &event);
    if (!(event.duck_mask & (1u << orbit))) return 0;
    orbits[0] = orbit;
    controls[0] = event.ducks[orbit];
    return 1;
}

static void duck_controls(const cJSON *v, AudioEvent *ev) {
    ev->duck_attack=fmax(.002,control(v,"duckattack",control(v,"duckatt",control(v,"datt",.1,0,10),0,10),0,10));
    ev->duck_depth=control(v,"duckdepth",1,0,1);
    ev->duck_onset=control(v,"duckonset",0,-10,10);
    const cJSON *duck=cJSON_GetObjectItemCaseSensitive(v,"duckorbit");
    if (!duck) duck=cJSON_GetObjectItemCaseSensitive(v,"duck");
    if (cJSON_IsArray(duck)) {
        int index=0;
        const cJSON *target;
        cJSON_ArrayForEach(target,duck) {
            duck_assign(v,target,index,ev);
            index++;
        }
    } else {
        int o=duck_target(duck);
        if (o>=0) ev->duck_mask=1u<<o;
    }
    if (ev->duck_mask && !ev->duck_values) {
        ev->duck_values=1;
        for (int o=0;o<N_ORBITS;o++) if (ev->duck_mask&(1u<<o)) {
            duck_onset(v,0,&ev->ducks[o]);
            ev->ducks[o].attack=duck_attack(v,0);
            ev->ducks[o].depth=duck_numeric(v,"duckdepth",0,1);
        }
    }
}

static double midi_value(const cJSON *v) {
    if (cJSON_IsNumber(v) && isfinite(v->valuedouble)) return fmax(-128, fmin(256, v->valuedouble));
    if (!cJSON_IsString(v)) return 36;
    const char *p = v->valuestring;
    char *end;
    double num = strtod(p, &end);
    if (end != p && !*end && isfinite(num)) return fmax(-128, fmin(256, num));
    const char *letters = "c d ef g a b";
    const char *base = strchr(letters, tolower((unsigned char)*p));
    if (!*p || !base || *p == ' ') return 60;
    int semitone = (int)(base - letters);
    p++;
    while (*p == '#' || *p == 's' || *p == 'b' || *p == 'f') {
        semitone += (*p == '#' || *p == 's') ? 1 : -1;
        p++;
    }
    long octave = 3;
    if (*p) {
        octave = strtol(p, &end, 10);
        if (*end || octave < -10 || octave > 20) return 60;
    }
    return (octave + 1) * 12 + semitone;
}

/* Prepare a bounded offline render through a confined LV2 plugin. The plugin
   runs in a worker, never in the audio callback; the result becomes an owned
   playback sample. Only the test/demo lv2: namespace is accepted here. */
static int lv2_prepare_sample(AudioEvent *ev, const char *sound, double duration, double gain) {
    if (strncmp(sound,"lv2:",4) || strlen(sound)<=4 || strlen(sound)>260) return 0;
    if (!isfinite(duration) || duration<=0) return 0;
    char error[256]="";
    Lv2Worker *worker=lv2_worker_start(sound+4,SR,error,sizeof(error));
    if (!worker) return 0;
    int frames=(int)fmin(4.0*SR,fmax(SR/10.0,ceil((duration+.1)*SR)));
    float *pcm=calloc((size_t)frames*2,sizeof(float));
    if (!pcm) { lv2_worker_stop(worker); return 0; }
    double frequency=fmax(20,fmin(12000,ev->freq>0?ev->freq:440));
    double phase=0, step=2*M_PI*frequency/SR;
    for (int i=0;i<frames;i++) {
        double fade=fmin(1,i/(.005*SR))*exp(-i/(.5*SR));
        pcm[i*2]=(float)(.25*sin(phase)*fade);
        pcm[i*2+1]=pcm[i*2];
        phase+=step;
    }
    lv2_worker_set_control(worker,0,(float)fmax(0,fmin(2,gain)),error,sizeof(error));
    /* Dispatch the note and its mapped controls into the plugin's atom MIDI
       input. Effects ignore it; instruments render the requested note. The
       engine applies the event gain separately, so velocity stays fixed and
       CC 74 carries the resolved low-pass cutoff. */
    int channel=ev->midi_chan>=0?(ev->midi_chan&15):0;
    int note=ev->midi_note;
    if (note<0) note=(int)fmax(0,fmin(127,round(69+12*log2(frequency/440))));
    int cc=-1;
    if (ev->dsp.enabled) {
        double cutoff=ev->dsp.p[DSP_LPF];
        if (cutoff>0) {
            double t=log2(fmax(20,fmin(20000,cutoff))/20)/log2(1000);
            cc=(int)fmax(0,fmin(127,round(t*127)));
        }
    }
    int note_off=(int)fmin((double)frames-1,fmax(1,ceil(duration*SR)));
    const unsigned char note_on[3]={(unsigned char)(0x90|channel),
        (unsigned char)note,100};
    const unsigned char note_off_msg[3]={(unsigned char)(0x80|channel),
        (unsigned char)note,0};
    const unsigned char cc_msg[3]={(unsigned char)(0xB0|channel),74,
        (unsigned char)(cc<0?0:cc)};
    int ok=1;
    for (int at=0;at<frames && ok;at+=LV2_WORKER_FRAMES) {
        int count=frames-at<LV2_WORKER_FRAMES?frames-at:LV2_WORKER_FRAMES;
        if (at==0 && cc>=0)
            ok=lv2_worker_send_midi_at(worker,0,cc_msg,3,error,sizeof(error));
        if (ok && at==0)
            ok=lv2_worker_send_midi_at(worker,0,note_on,3,error,sizeof(error));
        if (ok && note_off>=at && note_off<at+count)
            ok=lv2_worker_send_midi_at(worker,(uint32_t)(note_off-at),
                note_off_msg,3,error,sizeof(error));
        if (ok)
            ok=lv2_worker_process(worker,pcm+at*2,pcm+at*2,(unsigned)count,
                error,sizeof(error));
    }
    lv2_worker_stop(worker);
    if (!ok) { free(pcm); return 0; }
    ev->smp=smp_store_owned(pcm,frames);
    free(pcm);
    return ev->smp>=0;
}

static int fx_arg_number(const cJSON *step, int index, double *out) {
    const cJSON *args=cJSON_IsObject(step)?cJSON_GetObjectItemCaseSensitive(step,"args"):NULL;
    const cJSON *arg=cJSON_IsArray(args)?cJSON_GetArrayItem(args,index):NULL;
    if (cJSON_IsNumber(arg) && isfinite(arg->valuedouble)) {
        *out=arg->valuedouble; return 1;
    }
    if (cJSON_IsString(arg) && arg->valuestring && *arg->valuestring &&
        strlen(arg->valuestring)<64) {
        char *end=NULL;
        double value=strtod(arg->valuestring,&end);
        if (end && end!=arg->valuestring && !*end && isfinite(value)) {
            *out=value; return 1;
        }
    }
    return 0;
}

static int fx_arg_string(const cJSON *step, int index, char *out, size_t outlen) {
    const cJSON *args=cJSON_IsObject(step)?cJSON_GetObjectItemCaseSensitive(step,"args"):NULL;
    const cJSON *arg=cJSON_IsArray(args)?cJSON_GetArrayItem(args,index):NULL;
    if (!cJSON_IsString(arg) || !arg->valuestring || !*arg->valuestring ||
        strlen(arg->valuestring)>=outlen) return 0;
    snprintf(out,outlen,"%s",arg->valuestring);
    return 1;
}

enum {
    FX_OP_ROOM, FX_OP_SIZE, FX_OP_DRY, FX_OP_DELAY, FX_OP_DELAYTIME,
    FX_OP_DELAYFEEDBACK,
    FX_OP_LPF, FX_OP_LPQ, FX_OP_LPA, FX_OP_LPD, FX_OP_LPS, FX_OP_LPR,
    FX_OP_LPE,
    FX_OP_HPF, FX_OP_HPQ, FX_OP_HPA, FX_OP_HPD, FX_OP_HPS, FX_OP_HPR,
    FX_OP_HPE,
    FX_OP_BPF, FX_OP_BPQ, FX_OP_BPA, FX_OP_BPD, FX_OP_BPS, FX_OP_BPR,
    FX_OP_BPE,
    FX_OP_PAN, FX_OP_COARSE, FX_OP_FTYPE,
    FX_OP_SHAPE, FX_OP_CRUSH, FX_OP_DISTORT, FX_OP_DISTORTVOL,
    FX_OP_DISTORTTYPE, FX_OP_POST, FX_OP_CHORUS, FX_OP_SHAPEVOL,
    FX_OP_PHASER, FX_OP_PHASERDEPTH, FX_OP_PHASERCENTER, FX_OP_PHASERSWEEP,
    FX_OP_TREMOLO, FX_OP_TREMOLODEPTH, FX_OP_TREMOLOSKEW, FX_OP_TREMOLOPHASE,
    FX_OP_TREMOLOSHAPE, FX_OP_COMPRESSOR, FX_OP_COMPRESSORRATIO,
    FX_OP_COMPRESSORKNEE, FX_OP_COMPRESSORATTACK, FX_OP_COMPRESSORRELEASE,
    FX_OP_PCURVE,
    FX_OP_FM_INDEX, FX_OP_FM_RATIO, FX_OP_FM_ATTACK, FX_OP_FM_DECAY,
    FX_OP_FM_SUSTAIN, FX_OP_FM_RELEASE, FX_OP_FM_ENV, FX_OP_FM_WAVE,
    FX_OP_FM_MATRIX, FX_OP_IR, FX_OP_IRSPEED, FX_OP_IRBEGIN,
    FX_OP_ROOMFADE, FX_OP_ROOMLP, FX_OP_ROOMDIM,
    FX_OP_ANALYZE, FX_OP_FFT,
    FX_OP_COUNT
};

typedef struct { const char *name; int op; } FxName;
static const FxName fx_names[] = {
    {"room",FX_OP_ROOM},{"size",FX_OP_SIZE},{"roomsize",FX_OP_SIZE},
    {"dry",FX_OP_DRY},{"delay",FX_OP_DELAY},{"delaytime",FX_OP_DELAYTIME},
    {"delayfeedback",FX_OP_DELAYFEEDBACK},
    {"lpf",FX_OP_LPF},{"cutoff",FX_OP_LPF},{"lpq",FX_OP_LPQ},
    {"resonance",FX_OP_LPQ},{"lpa",FX_OP_LPA},{"lpattack",FX_OP_LPA},
    {"lpd",FX_OP_LPD},{"lpdecay",FX_OP_LPD},{"lps",FX_OP_LPS},
    {"lpsustain",FX_OP_LPS},{"lpr",FX_OP_LPR},{"lprelease",FX_OP_LPR},
    {"lpe",FX_OP_LPE},{"lpenv",FX_OP_LPE},
    {"hpf",FX_OP_HPF},{"hcutoff",FX_OP_HPF},{"hpq",FX_OP_HPQ},
    {"hresonance",FX_OP_HPQ},{"hpa",FX_OP_HPA},{"hpattack",FX_OP_HPA},
    {"hpd",FX_OP_HPD},{"hpdecay",FX_OP_HPD},{"hps",FX_OP_HPS},
    {"hpsustain",FX_OP_HPS},{"hpr",FX_OP_HPR},{"hprelease",FX_OP_HPR},
    {"hpe",FX_OP_HPE},{"hpenv",FX_OP_HPE},
    {"bpf",FX_OP_BPF},{"bandf",FX_OP_BPF},{"bpq",FX_OP_BPQ},
    {"bandq",FX_OP_BPQ},{"bpa",FX_OP_BPA},{"bpattack",FX_OP_BPA},
    {"bpd",FX_OP_BPD},{"bpdecay",FX_OP_BPD},{"bps",FX_OP_BPS},
    {"bpsustain",FX_OP_BPS},{"bpr",FX_OP_BPR},{"bprelease",FX_OP_BPR},
    {"bpe",FX_OP_BPE},{"bpenv",FX_OP_BPE},
    {"pan",FX_OP_PAN},{"coarse",FX_OP_COARSE},{"ftype",FX_OP_FTYPE},
    {"delaysync",FX_OP_DELAYTIME},
    {"shape",FX_OP_SHAPE},{"crush",FX_OP_CRUSH},{"distort",FX_OP_DISTORT},
    {"distortvol",FX_OP_DISTORTVOL},{"distorttype",FX_OP_DISTORTTYPE},
    {"postgain",FX_OP_POST},{"chorus",FX_OP_CHORUS},{"shapevol",FX_OP_SHAPEVOL},
    {"phaser",FX_OP_PHASER},{"phaserrate",FX_OP_PHASER},
    {"phaserdepth",FX_OP_PHASERDEPTH},{"phasercenter",FX_OP_PHASERCENTER},
    {"phasersweep",FX_OP_PHASERSWEEP},{"tremolo",FX_OP_TREMOLO},
    {"tremolodepth",FX_OP_TREMOLODEPTH},{"tremoloskew",FX_OP_TREMOLOSKEW},
    {"tremolophase",FX_OP_TREMOLOPHASE},{"tremoloshape",FX_OP_TREMOLOSHAPE},
    {"compressor",FX_OP_COMPRESSOR},{"compressorRatio",FX_OP_COMPRESSORRATIO},
    {"compressorKnee",FX_OP_COMPRESSORKNEE},
    {"compressorAttack",FX_OP_COMPRESSORATTACK},
    {"compressorRelease",FX_OP_COMPRESSORRELEASE},{"pcurve",FX_OP_PCURVE},
    {"fm",FX_OP_FM_INDEX},{"fmi",FX_OP_FM_INDEX},{"fmh",FX_OP_FM_RATIO},
    {"fmattack",FX_OP_FM_ATTACK},{"fmdecay",FX_OP_FM_DECAY},
    {"fmsustain",FX_OP_FM_SUSTAIN},{"fmrelease",FX_OP_FM_RELEASE},
    {"fmenv",FX_OP_FM_ENV},{"fmwave",FX_OP_FM_WAVE},
    {"ir",FX_OP_IR},{"irspeed",FX_OP_IRSPEED},{"irbegin",FX_OP_IRBEGIN},
    {"roomfade",FX_OP_ROOMFADE},{"roomlp",FX_OP_ROOMLP},{"roomdim",FX_OP_ROOMDIM},
    {"analyze",FX_OP_ANALYZE},{"fft",FX_OP_FFT}
};

static int fx_op(const char *name) {
    if (!name) return -1;
    for (unsigned i=0;i<sizeof(fx_names)/sizeof(fx_names[0]);i++)
        if (!strcmp(name,fx_names[i].name)) return fx_names[i].op;
    return -1;
}

/* FM controls accept an optional operator suffix (1..8) and fmi matrices use
   two digits (operator then source). Unnumbered names remain operator 1. */
static int fx_fm_parse(const char *name, int *operation, int *operator_index,
                       int *matrix_a, int *matrix_b) {
    if (!name || strncmp(name,"fm",2)) return 0;
    size_t length=strlen(name);
    if (length==5 && !strncmp(name,"fmi",3) &&
        name[3]>='1' && name[3]<='8' && name[4]>='0' && name[4]<='8') {
        *operation=FX_OP_FM_MATRIX;
        *matrix_a=name[3]-'1';
        *matrix_b=name[4]-'0';
        return 1;
    }
    const char *fields[] = {"fmi","fmh","fmattack","fmdecay","fmsustain",
        "fmrelease","fmenv","fmwave"};
    const int ops[] = {FX_OP_FM_INDEX,FX_OP_FM_RATIO,FX_OP_FM_ATTACK,
        FX_OP_FM_DECAY,FX_OP_FM_SUSTAIN,FX_OP_FM_RELEASE,FX_OP_FM_ENV,
        FX_OP_FM_WAVE};
    for (unsigned i=0;i<sizeof(fields)/sizeof(fields[0]);i++) {
        size_t base=strlen(fields[i]);
        if (length==base && !strcmp(name,fields[i])) {
            *operation=ops[i]; *operator_index=0; return 1;
        }
        if (length==base+1 && !strncmp(name,fields[i],base) &&
            name[base]>='1' && name[base]<='8') {
            *operation=ops[i]; *operator_index=name[base]-'1'; return 1;
        }
    }
    return 0;
}

static int fx_outer_has(const cJSON *v, int operation) {
    if (operation<0) return 0;
    for (unsigned i=0;i<sizeof(fx_names)/sizeof(fx_names[0]);i++)
        if (fx_names[i].op==operation && cJSON_HasObjectItem(v,fx_names[i].name))
            return 1;
    return 0;
}

static const cJSON *fx_step_array(const cJSON *step, int position) {
    const cJSON *args=cJSON_GetObjectItemCaseSensitive(step,"args");
    const cJSON *arg=cJSON_GetArrayItem(args,position);
    return cJSON_IsArray(arg)?arg:NULL;
}

static int fx_filter_type(int operation) {
    switch (operation) {
    case FX_OP_LPF: case FX_OP_LPQ: return AUDIO_FX_LPF;
    case FX_OP_HPF: case FX_OP_HPQ: return AUDIO_FX_HPF;
    case FX_OP_BPF: case FX_OP_BPQ: return AUDIO_FX_BPF;
    default: return -1;
    }
}

static int fx_distort_variant(const char *name) {
    static const struct { const char *name; int variant; } types[] = {
        {"soft",0}, {"fold",1}, {"hard",2}, {"sinefold",3},
        {"chebyshev",4}, {"scurve",5}, {"diode",6}, {"asym",7},
        {"cubic",8}
    };
    if (!name) return -1;
    for (unsigned i=0;i<sizeof(types)/sizeof(types[0]);i++)
        if (!strcmp(name,types[i].name)) return types[i].variant;
    return -1;
}

/* Numeric distorttype uses the deployed worklet's key order:
   scurve, soft, hard, cubic, diode, asym, fold, sinefold, chebyshev. */
static int fx_distort_site_variant(int index) {
    static const int variants[9] = {5,0,2,8,6,7,1,3,4};
    return index>=0 && index<9 ? variants[index] : 5;
}

static int fx_last_distort_stage(const AudioEvent *ev) {
    for (int i=ev->fx_stage_count-1;i>=0;i--)
        if (ev->fx_stages[i].type==AUDIO_FX_DISTORT) return i;
    return -1;
}

static void fx_add_stage(AudioEvent *ev, int type, double a, double b) {
    if (ev->fx_stage_count>=AUDIO_FX_STAGES_MAX) return;
    ev->fx_stages[ev->fx_stage_count++]=(AudioFxStage){type,a,b,0};
}

static int fx_last_filter_stage(const AudioEvent *ev, int type) {
    for (int i=ev->fx_stage_count-1;i>=0;i--)
        if (ev->fx_stages[i].type==type) return i;
    return -1;
}

/* Apply query-time FX(...) chains. The deployed renderer ignores callable
   arguments (they arrive as empty control objects) and keeps the chain's
   default 0.8 gain; pattern arguments carry serializable controls and replay
   here. Legacy non-pattern steps keep outer-control precedence. */
static void apply_fx_steps(AudioEvent *ev, const cJSON *v) {
    ev->fx_chain_gain = 1;
    ev->fx_chain_folded = 0;
    /* Source DSP effects consume the event gain before the FX chain in the
       native renderer; keep their established chain-gain folding. Pure FX
       chains apply stage gains before the stages and the main gain after. */
    int fold_chain=(int)ev->dsp.p[DSP_COARSE]>1 || ev->dsp.p[DSP_CRUSH]<16 ||
        ev->dsp.p[DSP_SHAPE]>0 || ev->dsp.p[DSP_DISTORT]>0 ||
        ev->dsp.p[DSP_COMP]<0 || ev->dsp.p[DSP_TREM]>0 ||
        ev->dsp.p[DSP_PHASER]>0 || ev->dsp.p[DSP_CHORUS]>0;
    ev->fx_chain_folded=fold_chain;
    const cJSON *chains = cJSON_GetObjectItemCaseSensitive(v,"_fxSteps");
    if (!cJSON_IsArray(chains)) return;
    int count = cJSON_GetArraySize(chains);
    if (count <= 0 || count > 8) return;
    /* A K() graph source replaces the oscillator/sample outside the FX chain:
       the deployed renderer does not apply the chain's default gain there.
       Worklet audioin effects and ordinary sources still enter the chain. */
    const cJSON *program=cJSON_GetObjectItemCaseSensitive(v,"_nativeProgram");
    const cJSON *kind=cJSON_IsObject(program)?
        cJSON_GetObjectItemCaseSensitive(program,"kind"):NULL;
    const cJSON *effect=cJSON_IsObject(program)?
        cJSON_GetObjectItemCaseSensitive(program,"effect"):NULL;
    if (cJSON_IsString(kind) && !strcmp(kind->valuestring,"graph") &&
        !cJSON_IsTrue(effect)) return;
    for (int i = 0; i < count; i++) {
        double chain_gain = .8;
        double pending_q[3] = {1,1,1};
        const cJSON *steps = cJSON_GetObjectItemCaseSensitive(
            cJSON_GetArrayItem(chains,i),"steps");
        if (!cJSON_IsArray(steps)) continue;
        const cJSON *chain = cJSON_GetArrayItem(chains,i);
        int pattern = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(chain,"pattern"));
        int closure = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(chain,"closure"));
        int nsteps = cJSON_GetArraySize(steps);
        if (nsteps > 32) nsteps = 32;
        for (int j = 0; j < nsteps; j++) {
            if (closure) break;
            const cJSON *step = cJSON_GetArrayItem(steps,j);
            const cJSON *op = cJSON_IsObject(step)
                ? cJSON_GetObjectItemCaseSensitive(step,"op") : NULL;
            if (!cJSON_IsString(op) || !op->valuestring) continue;
            const char *name=op->valuestring;
            if (!pattern && cJSON_HasObjectItem(v,name)) continue;
            /* The deployed renderer ignores shared-send and room/IR controls
               inside FX patterns; source controls remain in force. Presence
               of an FX room request still selects its stereophonic send. */
            if (pattern && !strcmp(name,"room")) ev->fx_room_present=1;
            if (pattern && (!strcmp(name,"room") || !strcmp(name,"size") ||
                !strcmp(name,"roomsize") || !strcmp(name,"dry") ||
                !strcmp(name,"delay") || !strcmp(name,"delaytime") ||
                !strcmp(name,"delaysync") || !strcmp(name,"delayfeedback") ||
                !strcmp(name,"ir") || !strcmp(name,"irspeed") ||
                !strcmp(name,"irbegin") || !strcmp(name,"roomfade") ||
                !strcmp(name,"roomlp") || !strcmp(name,"roomdim") ||
                !strcmp(name,"attack") || !strcmp(name,"decay") ||
                !strcmp(name,"sustain") || !strcmp(name,"release") ||
                !strcmp(name,"speed") || !strcmp(name,"note") ||
                !strcmp(name,"n") || !strcmp(name,"s") ||
                !strcmp(name,"bank")))
                continue;
            if (pattern && !strcmp(name,"gain")) {
                double value=0;
                if (fx_arg_number(step,0,&value) && value>=0 && value<=16)
                    chain_gain=value;
                continue;
            }
            if (!strcmp(name,"partials") || !strcmp(name,"phases")) {
                const cJSON *array=fx_step_array(step,0);
                if (!array) continue;
                int count=cJSON_GetArraySize(array);
                if (!strcmp(name,"partials")) {
                    if (count<1 || count>32) { ev->dsp.n_partials=-1; continue; }
                    ev->dsp.n_partials=count;
                    ev->dsp.periodic_numeric=0;
                    for (int i=0;i<count;i++) {
                        const cJSON *x=cJSON_GetArrayItem(array,i);
                        double value=cJSON_IsNumber(x) && isfinite(x->valuedouble)?
                            x->valuedouble:0;
                        ev->dsp.partials[i]=value<-1000000?-1000000:
                            value>1000000?1000000:value;
                    }
                } else {
                    if (ev->dsp.n_partials<1) continue;
                    if (count>ev->dsp.n_partials) count=ev->dsp.n_partials;
                    for (int i=0;i<count;i++) {
                        const cJSON *x=cJSON_GetArrayItem(array,i);
                        if (cJSON_IsNumber(x) && isfinite(x->valuedouble))
                            ev->dsp.phases[i]=x->valuedouble-floor(x->valuedouble);
                    }
                }
                continue;
            }
            int fm_operator=0, matrix_a=-1, matrix_b=-1;
            int operation=fx_op(name);
            int dsp_index=-1;
            double dsp_min=0,dsp_max=0;
            if (operation<0) {
                fx_fm_parse(name,&operation,&fm_operator,&matrix_a,&matrix_b);
                if (operation<0) {
                    if (!dsp_parameter_lookup(name,&dsp_index,&dsp_min,&dsp_max))
                        continue;
                    /* Source loop windows in an FX pattern are not applied by
                       the deployed renderer; they stay source defaults. */
                    if (pattern && (dsp_index==DSP_LOOP ||
                        dsp_index==DSP_LOOPBEGIN || dsp_index==DSP_LOOPEND))
                        continue;
                    double value=0;
                    if (!fx_arg_number(step,0,&value)) continue;
                    value=value<dsp_min?dsp_min:value>dsp_max?dsp_max:value;
                    ev->dsp.p[dsp_index]=value;
                    if (dsp_index==DSP_PW) ev->pw=value;
                    continue;
                }
            }
            if (!pattern && fx_outer_has(v,operation)) continue;
            if (operation==FX_OP_IR) {
                if (fx_arg_string(step,0,ev->fx_ir,sizeof(ev->fx_ir)))
                    ev->fx_room_fields|=1u<<6; /* keep the custom IR explicit */
                continue;
            }
            if (operation==FX_OP_DISTORTTYPE && pattern) {
                char algorithm[32];
                if (fx_arg_string(step,0,algorithm,sizeof(algorithm))) {
                    int variant=fx_distort_variant(algorithm);
                    int at=fx_last_distort_stage(ev);
                    if (variant>=0 && at>=0) ev->fx_stages[at].variant=variant;
                    continue;
                }
            }
            double value=0;
            if (!fx_arg_number(step,0,&value)) continue;
            switch (operation) {
            case FX_OP_ROOM:
                if (value>=0 && value<=1) ev->dsp.p[DSP_ROOM]=value;
                break;
            case FX_OP_SIZE:
                if (value>=0 && value<=10) {
                    ev->dsp.p[DSP_SIZE]=value;
                    ev->fx_room_fields|=1u<<0;
                }
                break;
            case FX_OP_DRY:
                if (value>=0 && value<=1) ev->dsp.p[DSP_DRY]=value;
                break;
            case FX_OP_DELAY:
                if (value>=0 && value<=1) ev->delay_send=value;
                break;
            case FX_OP_DELAYTIME:
                if (value>=0 && value<=1) ev->delay_time=value;
                break;
            case FX_OP_DELAYFEEDBACK:
                if (value>=0 && value<=.98) ev->delay_fb=value;
                break;
            case FX_OP_LPF:
                if (value>=0 && value<=20000) {
                    if (pattern) {
                        int type=fx_filter_type(operation);
                        fx_add_stage(ev,type,value,pending_q[type]);
                        pending_q[type]=1;
                    } else ev->dsp.p[DSP_LPF]=value;
                }
                break;
            case FX_OP_LPQ:
                if (value>=.05 && value<=50) {
                    if (pattern) {
                        int at=fx_last_filter_stage(ev,AUDIO_FX_LPF);
                        if (at>=0) ev->fx_stages[at].b=value;
                        else pending_q[AUDIO_FX_LPF]=value;
                    } else ev->dsp.p[DSP_LPQ]=value;
                }
                break;
            case FX_OP_LPA:
                if (value>=0 && value<=30) ev->dsp.p[DSP_LPA]=value;
                break;
            case FX_OP_LPD:
                if (value>=0 && value<=30) ev->dsp.p[DSP_LPD]=value;
                break;
            case FX_OP_LPS:
                if (value>=0 && value<=1) ev->dsp.p[DSP_LPS]=value;
                break;
            case FX_OP_LPR:
                if (value>=0 && value<=30) ev->dsp.p[DSP_LPR]=value;
                break;
            case FX_OP_LPE:
                if (value>=-16 && value<=16) ev->dsp.p[DSP_LPE]=value;
                break;
            case FX_OP_HPF:
                if (value>=0 && value<=20000) {
                    if (pattern) {
                        fx_add_stage(ev,AUDIO_FX_HPF,value,pending_q[AUDIO_FX_HPF]);
                        pending_q[AUDIO_FX_HPF]=1;
                    } else ev->dsp.p[DSP_HPF]=value;
                }
                break;
            case FX_OP_HPQ:
                if (value>=.05 && value<=50) {
                    if (pattern) {
                        int at=fx_last_filter_stage(ev,AUDIO_FX_HPF);
                        if (at>=0) ev->fx_stages[at].b=value;
                        else pending_q[AUDIO_FX_HPF]=value;
                    } else ev->dsp.p[DSP_HPQ]=value;
                }
                break;
            case FX_OP_HPA:
                if (value>=0 && value<=30) ev->dsp.p[DSP_HPA]=value;
                break;
            case FX_OP_HPD:
                if (value>=0 && value<=30) ev->dsp.p[DSP_HPD]=value;
                break;
            case FX_OP_HPS:
                if (value>=0 && value<=1) ev->dsp.p[DSP_HPS]=value;
                break;
            case FX_OP_HPR:
                if (value>=0 && value<=30) ev->dsp.p[DSP_HPR]=value;
                break;
            case FX_OP_HPE:
                if (value>=-16 && value<=16) ev->dsp.p[DSP_HPE]=value;
                break;
            case FX_OP_BPF:
                if (value>=0 && value<=20000) {
                    if (pattern) {
                        fx_add_stage(ev,AUDIO_FX_BPF,value,pending_q[AUDIO_FX_BPF]);
                        pending_q[AUDIO_FX_BPF]=1;
                    } else ev->dsp.p[DSP_BPF]=value;
                }
                break;
            case FX_OP_BPQ:
                if (value>=.05 && value<=50) {
                    if (pattern) {
                        int at=fx_last_filter_stage(ev,AUDIO_FX_BPF);
                        if (at>=0) ev->fx_stages[at].b=value;
                        else pending_q[AUDIO_FX_BPF]=value;
                    } else ev->dsp.p[DSP_BPQ]=value;
                }
                break;
            case FX_OP_BPA:
                if (value>=0 && value<=30) ev->dsp.p[DSP_BPA]=value;
                break;
            case FX_OP_BPD:
                if (value>=0 && value<=30) ev->dsp.p[DSP_BPD]=value;
                break;
            case FX_OP_BPS:
                if (value>=0 && value<=1) ev->dsp.p[DSP_BPS]=value;
                break;
            case FX_OP_BPR:
                if (value>=0 && value<=30) ev->dsp.p[DSP_BPR]=value;
                break;
            case FX_OP_BPE:
                if (value>=-16 && value<=16) ev->dsp.p[DSP_BPE]=value;
                break;
            case FX_OP_PAN:
                if (value>=0 && value<=1) {
                    if (pattern) fx_add_stage(ev,AUDIO_FX_PAN,value*2-1,0);
                    else {
                        ev->pan=value*2-1;
                        ev->has_pan=1;
                    }
                }
                break;
            case FX_OP_COARSE:
                if (value>=1 && value<=1024) ev->dsp.p[DSP_COARSE]=value;
                break;
            case FX_OP_FTYPE:
                if (value>=0 && value<=2) ev->dsp.p[DSP_FTYPE]=value;
                break;
            case FX_OP_SHAPE:
                if (value>=0 && value<=.99) {
                    if (pattern) fx_add_stage(ev,AUDIO_FX_SHAPE,value,1);
                    else ev->dsp.p[DSP_SHAPE]=value;
                }
                break;
            case FX_OP_CRUSH:
                if (value>=1 && value<=24) ev->dsp.p[DSP_CRUSH]=value;
                break;
            case FX_OP_DISTORT:
                if (value>=0 && value<=100) {
                    if (pattern) {
                        fx_add_stage(ev,AUDIO_FX_DISTORT,value,1);
                        int at=fx_last_distort_stage(ev);
                        if (at>=0) ev->fx_stages[at].variant=5;
                    } else ev->dsp.p[DSP_DISTORT]=value;
                }
                break;
            case FX_OP_DISTORTVOL:
                if (value>=0 && value<=16) {
                    if (pattern) {
                        int at=-1;
                        for (int k=ev->fx_stage_count-1;k>=0;k--)
                            if (ev->fx_stages[k].type==AUDIO_FX_DISTORT) {
                                at=k; break;
                            }
                        if (at>=0) ev->fx_stages[at].b=value;
                    } else ev->dsp.p[DSP_DISTVOL]=value;
                }
                break;
            case FX_OP_DISTORTTYPE:
                if (value>=0 && value<=8) {
                    if (pattern) {
                        int at=fx_last_distort_stage(ev);
                        if (at>=0)
                            ev->fx_stages[at].variant=fx_distort_site_variant((int)value);
                    } else ev->dsp.p[DSP_DISTTYPE]=value;
                }
                break;
            case FX_OP_POST:
                if (value>=0 && value<=16) ev->dsp.p[DSP_POST]=value;
                break;
            case FX_OP_CHORUS:
                if (value>=0 && value<=1) ev->dsp.p[DSP_CHORUS]=value;
                break;
            case FX_OP_SHAPEVOL:
                if (value>=0 && value<=16) {
                    if (pattern) {
                        int at=-1;
                        for (int k=ev->fx_stage_count-1;k>=0;k--)
                            if (ev->fx_stages[k].type==AUDIO_FX_SHAPE) {
                                at=k; break;
                            }
                        if (at>=0) ev->fx_stages[at].b=value;
                    } else ev->dsp.p[DSP_SHAPEVOL]=value;
                }
                break;
            case FX_OP_PHASER:
                if (value>=0 && value<=200) ev->dsp.p[DSP_PHASER]=value;
                break;
            case FX_OP_PHASERDEPTH:
                if (value>=0 && value<=1) ev->dsp.p[DSP_PHDEPTH]=value;
                break;
            case FX_OP_PHASERCENTER:
                if (value>=20 && value<=16000) ev->dsp.p[DSP_PHCENTER]=value;
                break;
            case FX_OP_PHASERSWEEP:
                if (value>=0 && value<=16000) ev->dsp.p[DSP_PHSWEEP]=value;
                break;
            case FX_OP_TREMOLO:
                if (value>=0 && value<=20000) ev->dsp.p[DSP_TREM]=value;
                break;
            case FX_OP_TREMOLODEPTH:
                if (value>=0 && value<=1) ev->dsp.p[DSP_TDEPTH]=value;
                break;
            case FX_OP_TREMOLOSKEW:
                if (value>=.001 && value<=.999) ev->dsp.p[DSP_TSKEW]=value;
                break;
            case FX_OP_TREMOLOPHASE:
                if (value>=-64 && value<=64) ev->dsp.p[DSP_TPHASE]=value;
                break;
            case FX_OP_TREMOLOSHAPE:
                if (value>=0 && value<=4) ev->dsp.p[DSP_TSHAPE]=value;
                break;
            case FX_OP_COMPRESSOR:
                if (value>=-100 && value<=0) ev->dsp.p[DSP_COMP]=value;
                break;
            case FX_OP_COMPRESSORRATIO:
                if (value>=1 && value<=20) ev->dsp.p[DSP_COMPRATIO]=value;
                break;
            case FX_OP_COMPRESSORKNEE:
                if (value>=0 && value<=40) ev->dsp.p[DSP_COMPKNEE]=value;
                break;
            case FX_OP_COMPRESSORATTACK:
                if (value>=0 && value<=1) ev->dsp.p[DSP_COMPATT]=value;
                break;
            case FX_OP_COMPRESSORRELEASE:
                if (value>=0 && value<=1) ev->dsp.p[DSP_COMPREL]=value;
                break;
            case FX_OP_PCURVE:
                if (value>=0 && value<=1) ev->dsp.p[DSP_PCURVE]=value;
                break;
            case FX_OP_IRSPEED:
                if (value>=-8 && value<=8) {
                    ev->room_speed=value;
                    ev->fx_room_fields|=1u<<4;
                }
                break;
            case FX_OP_IRBEGIN:
                if (value>=0 && value<=1) {
                    ev->room_begin=value;
                    ev->fx_room_fields|=1u<<5;
                }
                break;
            case FX_OP_ROOMFADE:
                if (value>=0 && value<=10) {
                    ev->dsp.p[DSP_ROOMFADE]=value;
                    ev->fx_room_fields|=1u<<1;
                }
                break;
            case FX_OP_ROOMLP:
                if (value>=0 && value<=22050) {
                    ev->dsp.p[DSP_ROOMLP]=value;
                    ev->fx_room_fields|=1u<<2;
                }
                break;
            case FX_OP_ROOMDIM:
                if (value>=0 && value<=22050) {
                    ev->dsp.p[DSP_ROOMDIM]=value;
                    ev->fx_room_fields|=1u<<3;
                }
                break;
            case FX_OP_FM_INDEX:
                if (value>=-64 && value<=64)
                    ev->dsp.p[DSP_FMBASE+fm_operator*8]=value;
                break;
            case FX_OP_FM_RATIO:
                if (value>=-64 && value<=64)
                    ev->dsp.p[DSP_FMBASE+fm_operator*8+1]=value;
                break;
            case FX_OP_FM_ATTACK:
                if (value>=0 && value<=30) {
                    ev->dsp.p[DSP_FMBASE+fm_operator*8+2]=value;
                    ev->dsp.fm_envelopes|=1u<<(unsigned)fm_operator;
                }
                break;
            case FX_OP_FM_DECAY:
                if (value>=0 && value<=30) {
                    ev->dsp.p[DSP_FMBASE+fm_operator*8+3]=value;
                    ev->dsp.fm_envelopes|=1u<<(unsigned)fm_operator;
                }
                break;
            case FX_OP_FM_SUSTAIN:
                if (value>=0 && value<=1) {
                    ev->dsp.p[DSP_FMBASE+fm_operator*8+4]=value;
                    ev->dsp.fm_envelopes|=1u<<(unsigned)fm_operator;
                }
                break;
            case FX_OP_FM_RELEASE:
                if (value>=0 && value<=30) {
                    ev->dsp.p[DSP_FMBASE+fm_operator*8+5]=value;
                    ev->dsp.fm_envelopes|=1u<<(unsigned)fm_operator;
                }
                break;
            case FX_OP_FM_ENV:
                if (value>=0 && value<=1)
                    ev->dsp.p[DSP_FMBASE+fm_operator*8+6]=value;
                break;
            case FX_OP_FM_WAVE:
                if (value>=0 && value<=6)
                    ev->dsp.p[DSP_FMBASE+fm_operator*8+7]=value;
                break;
            case FX_OP_FM_MATRIX:
                if (value>=-64 && value<=64 && matrix_a>=0 && matrix_a<8 &&
                    matrix_b>=0 && matrix_b<9)
                    ev->dsp.matrix[matrix_a][matrix_b]=value;
                break;
            case FX_OP_ANALYZE:
                if (value>=0 && value<=N_ANALYSERS)
                    ev->analyser_id=(int)round(value);
                break;
            case FX_OP_FFT:
                if (value>=0 && value<=10)
                    ev->analyser_fft=(int)round(value);
                break;
            default:
                break;
            }
        }
        if (fold_chain) ev->gain *= chain_gain;
        else ev->fx_chain_gain *= chain_gain;
    }
    if (ev->delay_time<=0 || ev->delay_fb==0) ev->delay_send=0;
}

/* Generic Strudel `.lfo()` modulation. Matches the deployed superdough
   lfo-processor: defaults rate=1 depth=1 dcoffset=-.5 triangle skew=.5
   curve=1; depth scales by the target value (1 when the target is 0);
   frequency targets clamp the LFO output to [20-base, 24000-base] when the
   base is >= 30 Hz. Unsupported targets keep their static value and surface
   one warning instead of silently misrendering. */
static double lfo_alias_number(const cJSON *entry, const char *a, const char *b,
    const char *c, double initial, double lo, double hi) {
    const char *names[3] = {a, b, c};
    for (int i = 0; i < 3; i++) {
        if (!names[i]) continue;
        const cJSON *p = cJSON_GetObjectItemCaseSensitive(entry, names[i]);
        if (cJSON_IsNumber(p) && isfinite(p->valuedouble))
            return fmax(lo, fmin(hi, p->valuedouble));
    }
    return initial;
}

static int lfo_shape_number(const cJSON *entry) {
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(entry, "shape");
    if (!p) p = cJSON_GetObjectItemCaseSensitive(entry, "sh");
    if (cJSON_IsNumber(p) && isfinite(p->valuedouble)) {
        int shape = (int)lround(p->valuedouble);
        return shape >= 0 && shape <= 4 ? shape : 0;
    }
    if (cJSON_IsString(p) && p->valuestring) {
        static const char *const names[] = {"tri","triangle","sine","ramp","saw","square"};
        static const int shapes[] = {0,0,1,2,3,4};
        for (int i = 0; i < 6; i++)
            if (!strcasecmp(p->valuestring, names[i])) return shapes[i];
    }
    return 0;
}

/* Generic LFO targets that modulate DSP params through per-sample voice
   offsets (see GEN_*). The node must exist upstream: its primary control
   must be present in the hap, except postgain whose node always exists. */
typedef struct { const char *name; int gen; int base; int freq; const char *need; } GenTarget;
static const GenTarget gen_targets[] = {
    {"tremolo", GEN_TREM_RATE, DSP_TREM, 1, "tremolo"},
    {"tremolosync", GEN_TREM_RATE, DSP_TREM, 1, "tremolosync"},
    {"tremolodepth", GEN_TREM_GAIN, DSP_TDEPTH, 0, "tremolo"},
    {"tremoloskew", GEN_TREM_SKEW, DSP_TSKEW, 0, "tremolo"},
    {"tremoloshape", GEN_TREM_SHAPE, DSP_TSHAPE, 0, "tremolo"},
    {"coarse", GEN_COARSE, DSP_COARSE, 0, "coarse"},
    {"crush", GEN_CRUSH, DSP_CRUSH, 0, "crush"},
    {"shape", GEN_SHAPE, DSP_SHAPE, 0, "shape"},
    {"shapevol", GEN_SHAPEVOL, DSP_SHAPEVOL, 0, "shape"},
    {"distort", GEN_DISTORT, DSP_DISTORT, 0, "distort"},
    {"distortvol", GEN_DISTVOL, DSP_DISTVOL, 0, "distort"},
    {"distorttype", GEN_DISTORT, DSP_DISTORT, 0, "distort"},
    {"phasercenter", GEN_PHCENTER, DSP_PHCENTER, 1, "phaserrate"},
    {"phasersweep", GEN_PHSWEEP, DSP_PHSWEEP, 0, "phaserrate"},
    {"phaserdepth", GEN_PHDEPTH, DSP_PHDEPTH, 0, "phaserrate"},
    {"phaserrate", GEN_PHRATE, DSP_PHASER, 1, "phaserrate"},
    {"compressor", GEN_COMP, DSP_COMP, 0, "compressor"},
    {"compressorRatio", GEN_COMPRATIO, DSP_COMPRATIO, 0, "compressor"},
    {"compressorKnee", GEN_COMPKNEE, DSP_COMPKNEE, 0, "compressor"},
    {"compressorAttack", GEN_COMPATT, DSP_COMPATT, 0, "compressor"},
    {"compressorRelease", GEN_COMPREL, DSP_COMPREL, 0, "compressor"},
    {"postgain", GEN_POSTGAIN, DSP_POST, 0, NULL},
    {"vibmod", GEN_VIBMOD, DSP_VIBMOD, 0, "vib"},
    {"pw", GEN_PW, DSP_PW, 0, "pulse-voice"},
    {"pwrate", GEN_PWRATE, DSP_PWRATE, 1, "pulse-voice"},
    {"pwsweep", GEN_PWSWEEP, DSP_PWSWEEP, 0, "pulse-voice"},
    {"vowel", GEN_VOWEL, DSP_VOWEL, 1, "vowel"},
    {NULL, 0, 0, 0, NULL}
};

static const GenTarget *gen_target(const char *name, const cJSON *value, const char *sound) {
    for (int i = 0; gen_targets[i].name; i++) {
        if (strcmp(name, gen_targets[i].name)) continue;
        const char *need = gen_targets[i].need;
        if (!need) return &gen_targets[i];
        if (!strcmp(need, "tremolo")) {
            if (cJSON_HasObjectItem(value,"tremolo") || cJSON_HasObjectItem(value,"tremolosync")) return &gen_targets[i];
        } else if (!strcmp(need, "phaserrate")) {
            if (cJSON_HasObjectItem(value,"phaserrate") && control(value,"phaserdepth",.75,0,16) > 0) return &gen_targets[i];
        } else if (!strcmp(need, "pulse-voice")) {
            /* The deployed pulsewidth nodes only exist on pulse-oscillator
               voices, and the pw LFO node only when the (defaulted) sweep
               is nonzero. */
            if (!sound || strcmp(sound, "pulse")) return NULL;
            if (!strcmp(name, "pw")) return &gen_targets[i];
            const cJSON *sw = cJSON_GetObjectItemCaseSensitive(value, "pwsweep");
            double sweep = cJSON_IsNumber(sw) && isfinite(sw->valuedouble) ? sw->valuedouble :
                (cJSON_HasObjectItem(value, "pwrate") ? 0.3 : 0.0);
            if (sweep != 0) return &gen_targets[i];
        } else if (cJSON_HasObjectItem(value,need)) return &gen_targets[i];
        return NULL;
    }
    return NULL;
}

static void read_generic_lfo(AudioEvent *ev, const cJSON *value, const char *sound) {
    const cJSON *lfo=cJSON_GetObjectItemCaseSensitive(value,"lfo");
    if (!cJSON_IsObject(lfo)) return;
    /* The deployed renderer connects every entry and sums repeats on the
       same target. Native keeps one slot per entry in encounter order (up
       to LFO_SLOTS; further entries warn), and one LFO per filter type, so
       scanning continues and later unsupported targets still warn instead
       of vanishing silently. Sample routings use the detune fallback
       (cents) exactly like the deployed getNodeParam frequency->detune
       fallback. */
    int filt_used=0;
    /* Mirror the sample-resolution branch: oscillator, wavetable and native
       sounds stay frequency-routed; everything else takes the deployed
       detune fallback (cents). Procedural drums ignore voice LFOs in either
       routing, and noteless sample playback has no oscillator pitch. */
    int is_osc = sound && oscillator(sound) >= 0;
    int wavetable = sound && !strncmp(sound,"wt_",3);
    int sample_routed = sound && sound[0] && !is_osc && !wavetable &&
        ev->inst != INST_NATIVE && ev->inst != INST_ADDITIVE;
    const cJSON *entry;
    cJSON_ArrayForEach(entry,lfo) {
        if (!entry->string || !strcmp(entry->string,"__ids") ||
            !cJSON_IsObject(entry)) continue;
        const cJSON *target=cJSON_GetObjectItemCaseSensitive(entry,"control");
        if (!cJSON_IsString(target) || !target->valuestring) continue;
        /* Route to a different node than the default chain when the web
           would: sub-control modulation and non-main FX buses need node
           graphs native does not reproduce. */
        const cJSON *sub=cJSON_GetObjectItemCaseSensitive(entry,"subControl");
        if (!sub) sub=cJSON_GetObjectItemCaseSensitive(entry,"sc");
        const cJSON *fxi=cJSON_GetObjectItemCaseSensitive(entry,"fxi");
        int fx_main = !cJSON_IsNumber(fxi) || fxi->valuedouble==0;
        if (!fx_main) { ev->lfo_unsupported=1; continue; }
        if ((cJSON_IsString(sub) && sub->valuestring && sub->valuestring[0]) ||
            (cJSON_IsNumber(sub) && isfinite(sub->valuedouble))) {
            ev->lfo_unsupported=1; continue;
        }
        double rate=lfo_alias_number(entry,"rate","r",NULL,1,0,1000);
        double sync=lfo_alias_number(entry,"sync","s",NULL,0,0,64);
        if (cJSON_HasObjectItem(entry,"sync") || cJSON_HasObjectItem(entry,"s"))
            rate=sync*ev->cps;
        double depth=lfo_alias_number(entry,"depth","dep","dr",1,-100,100);
        double absolute=lfo_alias_number(entry,"depthabs","da",NULL,0,-100000,100000);
        double dc=lfo_alias_number(entry,"dcoffset","dc",NULL,-.5,-100,100);
        double skew=lfo_alias_number(entry,"skew","sk",NULL,.5,.001,.999);
        double curve=lfo_alias_number(entry,"curve","cu",NULL,1,.001,64);
        double phaseoff=lfo_alias_number(entry,"phaseoffset",NULL,NULL,0,0,1);
        if (!(rate>0)) continue;
        const char *name=target->valuestring;
        int is_gain=!strcmp(name,"gain") || !strcmp(name,"amp");
        int is_source=!strcmp(name,"s") || !strcmp(name,"source") ||
            !strcmp(name,"frequency") || !strcmp(name,"Freq") ||
            !strcmp(name,"freq") || !strcmp(name,"note");
        int is_pan=!strcmp(name,"pan");
        int type=!strcmp(name,"cutoff")?0:!strcmp(name,"hcutoff")?1:
            !strcmp(name,"bandf")?2:-1;
        int qtype=!strcmp(name,"resonance")?0:!strcmp(name,"hresonance")?1:
            !strcmp(name,"bandq")?2:-1;
        const GenTarget *gent = gen_target(name, value, sound);
        /* Upstream only builds a pan node when the hap carries pan, only
           builds each filter node when its cutoff is set, and only builds
           effect nodes when their control is set; otherwise the connection
           warns instead of modulating. */
        if (is_pan && !ev->has_pan) { ev->lfo_unsupported=1; continue; }
        if (qtype>=0 && !(ev->dsp.p[qtype*7]>0)) { ev->lfo_unsupported=1; continue; }
        if (!is_gain && !is_source && !is_pan && qtype<0 && !gent && type<0) { ev->lfo_unsupported=1; continue; }
        double base=0;
        if (is_gain) base=ev->gain;
        else if (is_source) base=sample_routed?1:ev->freq;
        else if (is_pan) base=ev->pan;
        else if (type>=0) base=ev->dsp.p[type*7];
        else if (qtype>=0) base=ev->dsp.p[qtype*7+1];
        else if (gent && gent->gen == GEN_VOWEL) {
            /* The deployed vowel LFO connects to all five formant
               frequencies; the depth scales by the first formant, which
               is also the connected AudioParam the range clamp reads.
               Order matches the vowel table in dsp_init. */
            static const double first[15] = {660,440,270,430,370,650,560,
                500,250,400,600,500,600,700,500};
            int vi = (int)ev->dsp.p[DSP_VOWEL];
            if (vi < 0 || vi > 14) { ev->lfo_unsupported=1; continue; }
            base = first[vi];
        }
        else if (gent) base=ev->dsp.p[gent->base];
        /* The amGain tremolo node is constructed with the static gain, so a
           generic tremolodepth LFO scales by that value (0 becomes 1). */
        if (gent && gent->gen == GEN_TREM_GAIN)
            base = 1.0-base > 0 ? 1.0-base : 0.0;
        /* The deployed phaser filter is constructed at center+282 Hz with
           Q from depth, so generic phasercenter/phasedepth LFOs scale by
           those node values. */
        if (gent && gent->gen == GEN_PHCENTER) base += 282.0;
        if (gent && gent->gen == GEN_PHDEPTH)
            base = 2.0 - fmin(fmax(base*2.0, 0.0), 1.9);
        /* The deployed vibrato gain is in cents (semitones * 100), so a
           generic vibmod LFO scales by the cents value (0 maps to 1,
           like every target). The DSP path divides the offset back to
           semitones at use. */
        if (gent && gent->gen == GEN_VIBMOD) {
            base = base*100.0;
            if (!(base>0) && !(base<0)) base = 1.0;
        }
        if (!(base>0) && !(base<0)) {
            if (is_gain || is_source) base=1;
            else if (is_pan) base=1;
            else if (qtype>=0) base=1;
            else if (gent) base=1;
            else if (type>=0 && !(base>0)) { ev->lfo_unsupported=1; continue; }
        }
        if (depth==0 && absolute==0) continue;
        double scaled=absolute?absolute:depth*base;
        double min, max;
        if (is_source && !sample_routed && base>=30) { min=20-base; max=24000-base; }
        else if (gent && gent->freq && base>=30) { min=20-base; max=24000-base; }
        else { min=dc*scaled; max=dc*scaled+scaled; }
        int shape=lfo_shape_number(entry);
        /* Voice slots fill in encounter order; repeats on the same target
           take their own slots and sum in the renderer, matching the
           deployed node-per-entry graph. Filter frequency targets use the
           DSP path below instead; resonance targets ride voice slots whose
           offsets the DSP update reads per sample. */
        if (is_gain || is_source || is_pan || qtype>=0 || gent) {
            int slot=-1;
            for (int i = 0; i < LFO_SLOTS; i++)
                if (!ev->lfo[i].tgt) { slot=i; break; }
            if (slot<0) { ev->lfo_unsupported=1; continue; }
            int tgt=is_gain?2:is_source?(sample_routed?7:5):is_pan?8:qtype>=0?9+qtype:GEN_TGT_BASE+gent->gen;
            /* The deployed node phases from cycle/cps (onset time divided by
               tempo); retrig restarts from the phase offset instead. */
            const cJSON *retrig=cJSON_GetObjectItemCaseSensitive(entry,"retrig");
            if (!retrig) retrig=cJSON_GetObjectItemCaseSensitive(entry,"rt");
            int retrigger=cJSON_IsNumber(retrig) && isfinite(retrig->valuedouble) &&
                retrig->valuedouble>.5;
            /* Upstream phases from cycle/cps; the offline render path passes
               the onset time as cycle, so onset/cps matches the gated PCM.
               Live transport offsets are approximated by the same base. */
            ev->lfo[slot].tgt=tgt;
            ev->lfo[slot].wave=shape;
            ev->lfo[slot].hz=rate;
            ev->lfo[slot].depth=scaled;
            ev->lfo[slot].skew=skew;
            ev->lfo[slot].curve=curve;
            ev->lfo[slot].dc=dc;
            ev->lfo[slot].phase=phaseoff;
            ev->lfo[slot].min=min;
            ev->lfo[slot].max=max;
            ev->lfo[slot].timebase=(retrigger||!(ev->cps>0))?0:ev->time/ev->cps;
            if (tgt>=9) ev->dsp.lfo_sample_rate=1;
            continue;
        }
        if (type>=0 && ev->dsp.p[type*7]>0) {
            if (filt_used&(1<<type)) continue;
            filt_used|=(1<<type);
            int p=DSP_LPRATE+type*7;
            ev->dsp.p[p]=rate;
            ev->dsp.p[p+1]=absolute?0:depth;
            ev->dsp.p[p+2]=absolute;
            ev->dsp.p[p+3]=shape==0?1:shape==1?0:shape==2?4:shape==3?3:2;
            ev->dsp.p[p+4]=dc;
            ev->dsp.p[p+5]=skew;
            ev->dsp.p[p+6]=curve;
            ev->dsp.lfo_sample_rate=1;
            continue;
        }
        ev->lfo_unsupported=1;
    }
}

/* Generic Strudel `.env()` modulation. Matches the deployed
   envelope-processor: ADSR segments with warp curves, output
   clamp(val*depth, min, max), depth scaled by the target value (1 when the
   target is 0), same frequency-target clamp as the LFO path. Cutoff targets
   stay unsupported for now (no per-sample generic envelope in the filter
   path); other unsupported targets warn instead of silently misrendering. */
static void read_generic_env(AudioEvent *ev, const cJSON *value, const char *sound) {
    const cJSON *env=cJSON_GetObjectItemCaseSensitive(value,"env");
    if (!cJSON_IsObject(env)) return;
    int is_osc = sound && oscillator(sound) >= 0;
    int wavetable = sound && !strncmp(sound,"wt_",3);
    int sample_routed = sound && sound[0] && !is_osc && !wavetable &&
        ev->inst != INST_NATIVE && ev->inst != INST_ADDITIVE;
    const cJSON *entry;
    cJSON_ArrayForEach(entry,env) {
        if (!entry->string || !strcmp(entry->string,"__ids") ||
            !cJSON_IsObject(entry)) continue;
        const cJSON *target=cJSON_GetObjectItemCaseSensitive(entry,"control");
        if (!cJSON_IsString(target) || !target->valuestring) continue;
        const cJSON *sub=cJSON_GetObjectItemCaseSensitive(entry,"subControl");
        if (!sub) sub=cJSON_GetObjectItemCaseSensitive(entry,"sc");
        const cJSON *fxi=cJSON_GetObjectItemCaseSensitive(entry,"fxi");
        int fx_main = !cJSON_IsNumber(fxi) || fxi->valuedouble==0;
        if (!fx_main) { ev->env_unsupported=1; continue; }
        if ((cJSON_IsString(sub) && sub->valuestring && sub->valuestring[0]) ||
            (cJSON_IsNumber(sub) && isfinite(sub->valuedouble))) {
            ev->env_unsupported=1; continue;
        }
        double attack=lfo_alias_number(entry,"attack","att","a",.005,0,30);
        double decay=lfo_alias_number(entry,"decay","dec","d",.14,0,30);
        double sustain=lfo_alias_number(entry,"sustain","sus","s",0,0,1);
        double rel=lfo_alias_number(entry,"release","rel","r",.1,0,30);
        double acurve=lfo_alias_number(entry,"acurve","ac",NULL,0,-1,1);
        double dcurve=lfo_alias_number(entry,"dcurve","dc",NULL,0,-1,1);
        double rcurve=lfo_alias_number(entry,"rcurve","rc",NULL,0,-1,1);
        double depth=lfo_alias_number(entry,"depth","dep","dr",1,-100,100);
        double absolute=lfo_alias_number(entry,"depthabs","da",NULL,0,-100000,100000);
        const char *name=target->valuestring;
        int is_gain=!strcmp(name,"gain") || !strcmp(name,"amp");
        int is_source=!strcmp(name,"s") || !strcmp(name,"source") ||
            !strcmp(name,"frequency") || !strcmp(name,"Freq") ||
            !strcmp(name,"freq") || !strcmp(name,"note");
        int is_pan=!strcmp(name,"pan");
        if (is_pan && !ev->has_pan) { ev->env_unsupported=1; continue; }
        if (!is_gain && !is_source && !is_pan) { ev->env_unsupported=1; continue; }
        double base=0;
        if (is_gain) base=ev->gain;
        else if (is_source) base=sample_routed?1:ev->freq;
        else if (is_pan) base=ev->pan;
        if (!(base>0) && !(base<0)) base=1;
        if (depth==0 && absolute==0) continue;
        double scaled=absolute?absolute:depth*base;
        double min, max;
        /* connectEnvelope clamps only audio-rate frequency targets; the
           worklet itself defaults min/max to +-1e9 (no dcoffset term). */
        if (is_source && !sample_routed && base>=30) { min=20-base; max=24000-base; }
        else { min=-1e9; max=1e9; }
        /* The deployed envelope node never starts (connectEnvelope omits
           begin/end, so begin stays equal to the worklet's initial
           beginTime): entries are recorded for diagnostics but never
           rendered, matching the site sample-for-sample. */
        ev->env_unsupported = 1;
        int slot=-1;
        for (int i = 0; i < ENV_SLOTS; i++)
            if (!ev->env[i].tgt) { slot=i; break; }
        if (slot<0) continue;
        int tgt=is_gain?2:is_source?(sample_routed?7:5):8;
        ev->env[slot].tgt=tgt;
        ev->env[slot].attack=attack;
        ev->env[slot].decay=decay;
        ev->env[slot].sustain=sustain;
        ev->env[slot].release=rel;
        ev->env[slot].acurve=acurve;
        ev->env[slot].dcurve=dcurve;
        ev->env[slot].rcurve=rcurve;
        ev->env[slot].depth=scaled;
        ev->env[slot].min=min;
        ev->env[slot].max=max;
    }
}

static int audio_event_values(const cJSON *hap, double origin, double seconds, AudioEvent *ev, int include_ducks) {
    const cJSON *begin = cJSON_GetObjectItemCaseSensitive(hap,"begin");
    const cJSON *end = cJSON_GetObjectItemCaseSensitive(hap,"end");
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(hap,"value");
    if (!ev || !cJSON_IsObject(v) || !cJSON_IsNumber(begin) || !cJSON_IsNumber(end) ||
        !isfinite(begin->valuedouble) || !isfinite(end->valuedouble) ||
        end->valuedouble <= begin->valuedouble || !isfinite(origin) || seconds <= 0 || !isfinite(seconds)) return 0;
    memset(ev,0,sizeof(*ev));
    ev->scope_mask = (unsigned)control(hap,"scopeMask",0,0,4294967295.0);
    ev->out_target = audio_output_target(text_control(v,"_out"));
    /* Superdough analyze(id) attaches the voice to a shared analyser node.
       fft is an exponent: fftSize = 2 ** (fft + 5), default 8 (8192). */
    ev->analyser_id = (int)round(control(v,"analyze",0,0,(double)N_ANALYSERS));
    ev->analyser_fft = (int)round(control(v,"fft",8,0,10));
    ev->time = origin + begin->valuedouble * seconds;
    ev->cps = seconds > 0 ? 1 / seconds : 0;
    ev->duration = fmin(60,(end->valuedouble - begin->valuedouble) * seconds);
    double clip = control(v,"clip",1,0,64);
    if (clip == 0) clip=1; /* Superdough's clip || 1 fallback. */
    ev->clip = clip;
    ev->duration = fmin(60,ev->duration * clip);
    ev->duration = control(v,"duration",ev->duration,.00001,60);
    /* Superdough multiplies velocity with no 0..1 cap. 16 matches the other amplitude stages. */
    ev->gain = control(v,"gain",.8,0,16) * control(v,"velocity",1,0,16) * control(v,"amp",1,0,16)
        * control(v,"busgain",1,0,16) * control(v,"overgain",1,0,16);
    ev->pan = control(v,"pan",.5,0,1) * 2 - 1;
    ev->has_pan=cJSON_HasObjectItem(v,"pan");
    const char *envelope_names[4] = {"attack","decay","sustain","release"};
    const double envelope_defaults[4] = {.001,.05,.6,.01};
    double envelope[4];
    dsp_read_adsr(v,envelope_names,envelope_defaults,envelope);
    ev->att = envelope[0]; ev->dec = envelope[1];
    ev->sus = envelope[2]; ev->rel = envelope[3];
    ev->fx_release = control(v,"FXrelease",0,0,60);
    ev->graph_release = control(v,"release",.01,0,60);
    ev->smp = ev->wt = ev->midi_chan = ev->midi_note = -1;
    ev->delay_send = control(v,"delay",0,0,1);
    ev->delay_time = cJSON_HasObjectItem(v,"delaytime") ? control(v,"delaytime",0,0,1)
        : control(v,"delaysync",.1875,0,64)*seconds;
    ev->delay_time=fmin(1,ev->delay_time); /* Web orbit DelayNode maxDelayTime. */
    ev->delay_fb = control(v,"delayfeedback",.5,0,.98);
    /* Superdough does not create/update/send a delay when either parameter
       is zero. Existing shared tails keep their previous automation. */
    if (ev->delay_time<=0 || ev->delay_fb==0) ev->delay_send=0;
    ev->orbit = (int)control(v,"orbit",control(v,"o",1,0,N_ORBITS-1),0,N_ORBITS-1);
    if (include_ducks) duck_controls(v,ev);
    ev->smp_speed = control(v,"speed",1,-8,8);
    if (cJSON_HasObjectItem(v,"stretch")) {
        double shift = control(v,"stretch",0,-4,16);
        ev->stretch = fmax(0,1+(shift < 0 ? shift*.25 : shift));
        if (ev->stretch == 0) return 0;
    }
    ev->smp_end = control(v,"end",1,0,1);
    ev->pw = control(v,"pw",.5,.01,.99);
    dsp_params_read(&ev->dsp,v);
    if (cJSON_HasObjectItem(v,"tremolosync"))
        ev->dsp.p[DSP_TREM] = control(v,"tremolosync",0,0,1024)/seconds;
    const char *sync[] = {"lpsync","hpsync","bpsync"};
    for (int i = 0; i < 3; i++)
        if (cJSON_HasObjectItem(v,sync[i])) ev->dsp.p[DSP_LPRATE+i*7] = control(v,sync[i],0,0,1024)/seconds;
    apply_fx_steps(ev,v);
    const char *sound = text_control(v,"s");
    if (!strncmp(sound,"lv2:",4))
        snprintf(ev->lv2_uri,sizeof(ev->lv2_uri),"%s",sound+4);
    for (const cJSON *item=v->child; item && ev->lv2_param_count<AUDIO_LV2_PARAMS_MAX;
         item=item->next) {
        if (!item->string || strncmp(item->string,"lv2param_",9) ||
            !cJSON_IsNumber(item) || !isfinite(item->valuedouble)) continue;
        const char *name=item->string+9;
        if (!*name || strlen(name)>=sizeof(ev->lv2_params[0].name)) continue;
        AudioLv2Param *param=&ev->lv2_params[ev->lv2_param_count++];
        snprintf(param->name,sizeof(param->name),"%s",name);
        param->value=(float)item->valuedouble;
    }
    const cJSON *preset=cJSON_GetObjectItemCaseSensitive(v,"lv2preset");
    if (cJSON_IsString(preset) && preset->valuestring && preset->valuestring[0] &&
        strlen(preset->valuestring)<sizeof(ev->lv2_preset))
        snprintf(ev->lv2_preset,sizeof(ev->lv2_preset),"%s",preset->valuestring);
    const cJSON *state=cJSON_GetObjectItemCaseSensitive(v,"lv2state");
    if (cJSON_IsString(state) && state->valuestring && state->valuestring[0] &&
        strlen(state->valuestring)<sizeof(ev->lv2_state))
        snprintf(ev->lv2_state,sizeof(ev->lv2_state),"%s",state->valuestring);
    if ((ev->lv2_param_count>0 || ev->lv2_preset[0] || ev->lv2_state[0]) &&
        ev->lv2_uri[0] && !cJSON_HasObjectItem(v,"note")) ev->control_only=1;
    const cJSON *carla_project=cJSON_GetObjectItemCaseSensitive(v,"carlaProject");
    const cJSON *carla_target=cJSON_GetObjectItemCaseSensitive(v,"carlaTarget");
    if (cJSON_IsString(carla_project) && carla_project->valuestring &&
        carla_project->valuestring[0] &&
        strlen(carla_project->valuestring)<sizeof(ev->carla_project))
        snprintf(ev->carla_project,sizeof(ev->carla_project),"%s",
            carla_project->valuestring);
    if (cJSON_IsString(carla_target) && carla_target->valuestring &&
        strlen(carla_target->valuestring)<sizeof(ev->carla_target))
        snprintf(ev->carla_target,sizeof(ev->carla_target),"%s",
            carla_target->valuestring);
    int wavetable=!strncmp(sound,"wt_",3);
    if (wavetable && !cJSON_HasObjectItem(v,"loop")) ev->dsp.p[DSP_LOOP]=1;
    ev->waveform_gain=(!strcmp(sound,"saw") || !strcmp(sound,"sawtooth"))?dsp_periodic_gain(0):
        !strcmp(sound,"square")?dsp_periodic_gain(1):1;
    const cJSON *note = cJSON_GetObjectItemCaseSensitive(v,"note");
    double midi = midi_value(note);
    int inst = oscillator(sound);
    ev->inst = inst >= 0 ? inst : INST_TRI;
    ev->is_drum = !note && inst < 0;
    if (wavetable) ev->is_drum=0;
    if (!*sound) ev->is_drum = 0;
    if (ev->dsp.n_partials) { ev->inst = INST_ADDITIVE; ev->is_drum = 0; ev->waveform_gain=1; }
    if (ev->is_drum) ev->inst = INST_HH;
    const cJSON *native=cJSON_GetObjectItemCaseSensitive(v,"_nativeProgram");
    if (native && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(native,"effect"))) {
        ev->inst=INST_NATIVE; ev->is_drum=0;
    }
    ev->freq = control(v,"freq",440*pow(2,(midi-69)/12),.01,SR*.45);
    /* Superdough multiplies the resolved frequency by 2^octave, including
       explicit freq values. The LFO source base below follows from this. */
    double octave = control(v,"octave",0,-16,16);
    if (octave) ev->freq = fmax(.01,fmin(SR*.45,ev->freq*pow(2,octave)));
    /* Static detune is a unison-width control in the REPL, not a generic
       transposition. Pitch envelopes and vibrato automate source detune. */
    if (ev->is_drum) ev->freq = 0;
    /* Generic LFO depth scales by its target value, so parse it after the
       frequency, gain and filter base values are known. */
    read_generic_lfo(ev,v,sound);
    /* Generic envelope depth scales by its target value like the LFO, so
       parse it after the frequency, gain and filter base values are known. */
    read_generic_env(ev,v,sound);
    snprintf(ev->token,sizeof(ev->token),"%.63s",*sound ? sound : native?"kabelsalat":"triangle");
    if (ev->inst == INST_WT) ev->wt = wt_find(sound+3);
    const cJSON *channel = cJSON_GetObjectItemCaseSensitive(v,"midichan");
    if (cJSON_IsNumber(channel) && isfinite(channel->valuedouble) && channel->valuedouble >= 1 && channel->valuedouble <= 16) {
        ev->midi_chan = channel->valueint - 1;
        ev->midi_note = (int)fmax(0,fmin(127,round(midi)));
    }
    return isfinite(ev->time) && isfinite(ev->freq);
}

int js_audio_event(const cJSON *hap, double origin, double seconds, AudioEvent *ev) {
    return audio_event_values(hap,origin,seconds,ev,1);
}

/* "misc:2" is a name plus a sample index. A non-numeric suffix stays intact. */
static void sample_bank_name(const char *sound, char *out, size_t n) {
    char *colon;
    char *end = NULL;
    double v;
    if (!out || n == 0) return;
    out[0] = 0;
    if (!sound || !sound[0]) return;
    snprintf(out, n, "%s", sound);
    colon = strchr(out, ':');
    if (!colon || colon == out || colon[1] == 0) return;
    v = strtod(colon + 1, &end);
    if (end == colon + 1 || (end && *end) || !isfinite(v)) return;
    *colon = 0;
}

static unsigned room_explicit_fields(const cJSON *value) {
    const char *room_names[]={"roomsize","roomfade","roomlp","roomdim","irspeed","irbegin"};
    unsigned room_explicit=0;
    for (int i=0;i<6;i++) {
        const cJSON *parameter=cJSON_GetObjectItemCaseSensitive(value,room_names[i]);
        if (cJSON_IsNumber(parameter) && isfinite(parameter->valuedouble)) room_explicit|=1u<<i;
    }
    if (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(value,"size"))) room_explicit|=1;
    return room_explicit;
}

/* Worker-safe: resolves immutable source data and leaves shared orbit state,
   including inherited room controls and custom-IR FFTs, to the publisher. */
static int resolve_sample_source(AudioEvent *ev, const cJSON *value) {
    if (ev->dsp.n_partials<0) {
        log_console("oscillator: invalid harmonic count or more than 32 explicit partials"); return 0;
    }
    if (ev->inst==INST_ADDITIVE && !dsp_prepare_periodic(&ev->dsp)) {
        log_console("oscillator: periodic-wave preparation/cache limit exceeded"); return 0;
    }
    int noise_color=ev->inst==INST_NOISE?0:ev->inst==INST_PINK?1:ev->inst==INST_BROWN?2:-1;
    if (noise_color<0 && ev->dsp.noise_mix>0 && (ev->inst==INST_SINE || ev->inst==INST_SAW ||
        ev->inst==INST_TRI || ev->inst==INST_SQUARE || ev->inst==INST_ADDITIVE)) noise_color=1;
    if (noise_color>=0 && !(ev->dsp.source_noise=dsp_prepare_noise(noise_color))) {
        log_console("oscillator: noise-buffer allocation failed"); return 0;
    }
    if (!dsp_prepare_fm(&ev->dsp)) {
        log_console("oscillator: FM noise-buffer allocation failed"); return 0;
    }
    if (ev->inst==INST_NATIVE || cJSON_HasObjectItem(value,"_nativeProgram")) {
        ev->native_synth=native_synth_prepare(value,ev->freq,ev->duration);
        if (!ev->native_synth) { log_console("native synth: unsupported program or preparation limit exceeded"); return 0; }
    }
    const cJSON *ir = cJSON_GetObjectItemCaseSensitive(value,"ir");
    ev->room_params_valid=1; ev->room_sample=-1;
    if (!(ev->fx_room_fields&(1u<<4)))
        ev->room_speed=control(value,"irspeed",1,-8,8);
    if (!(ev->fx_room_fields&(1u<<5)))
        ev->room_begin=control(value,"irbegin",0,0,1);
    const char *ir_name = cJSON_IsString(ir)?ir->valuestring:
        (ev->fx_ir[0]?ev->fx_ir:NULL);
    if (ir_name && ev->dsp.p[DSP_ROOM]>0) {
        if (!ir_name[0]) return 0;
        char token[256];
        int n=snprintf(token,sizeof(token),"%s:%d",ir_name,
            (int)floor(control(value,"i",1,-1000000,1000000)+.5));
        if (n<0 || (size_t)n>=sizeof(token)) return 0;
        ev->room_sample=websamp_acquire(token,-1);
        if (ev->room_sample<0) return 0;
    }
    const char *sound = text_control(value,"s"), *bank = text_control(value,"bank");
    if (!*sound || oscillator(sound) >= 0 || ev->dsp.n_partials || (ev->native_synth && !native_synth_effect(ev->native_synth))) return 1;
    double midi = ev->is_drum ? -1 : fmax(0,fmin(127,69+12*log2(ev->freq/440)));
    double n = floor(control(value,"n",0,-1000000,1000000)+.5);
    char token[1024];
    int len = *bank ? snprintf(token,sizeof(token),"%s_%s:%d",bank,sound,(int)n)
                    : snprintf(token,sizeof(token),"%s:%d",sound,(int)n);
    if (len < 0 || (size_t)len >= sizeof(token)) return 0;
    char mapped_name[256];
    int name_length = *bank ? snprintf(mapped_name,sizeof(mapped_name),"%s_%s",bank,sound)
                           : snprintf(mapped_name,sizeof(mapped_name),"%s",sound);
    int mapped = name_length > 0 && (size_t)name_length < sizeof(mapped_name) &&
        websamp_slot_count(mapped_name) > 0;
    ev->smp = websamp_acquire(token,midi);
    /* Explicit sample maps must not silently change instrument on failure. */
    if (mapped && ev->smp < 0) return 0;
    if (!mapped && ev->smp<0 && !*bank && gm_synth_program(sound)>=0) {
        /* WebAudioFont pack path: on success fall through to the shared sample
           epilogue below so ADSR defaults, gating and stretch match plain
           pitched samples; otherwise keep the FluidSynth fallback. */
        if (!gm_synth_prepare_sample(ev,value,midi)) return gm_synth_prepare(ev);
    }
    if (ev->smp < 0 && *bank) {
        char dir[1024];
        if (bank_find_dir(bank,dir,sizeof(dir))) {
            char local[128];
            int nbytes = snprintf(local,sizeof(local),"%s:%d",sound,(int)n);
            if (nbytes > 0 && (size_t)nbytes < sizeof(local))
                ev->smp = midi >= 0 ? smp_acquire_note(dir,midi) : smp_acquire(dir,local);
        }
    }
    if (ev->smp < 0 && !*bank && !strncmp(sound,"lv2:",4)) {
        /* A MIDI instrument is hosted live for the scheduler; effects and
           plugins without a MIDI input keep the bounded offline render. */
        int live=live_lv2_acquire(sound+4,SR,audio_time_now(),NULL,0);
        if (live>=0) { ev->live_lv2=live+1; return 1; }
        double lv2_gain=control(value,"gain",1,0,2);
        lv2_prepare_sample(ev,sound,ev->duration,lv2_gain);
    }
    if (ev->smp < 0 && !*bank) {
        if (strchr(sound,'/') || strchr(sound,'.')) ev->smp=smp_acquire_path(sound);
    }
    if (ev->smp < 0 && !*bank) {
        char dir[1024];
        char bare[256];
        sample_bank_name(sound, bare, sizeof(bare));
        if (bare[0] && bank_find_dir(bare,dir,sizeof(dir)))
            ev->smp = midi >= 0 ? smp_acquire_note(dir,midi) : smp_acquire(dir,token);
    }
    if (ev->smp >= 0) {
        if (ev->smp_speed == 0) return 0;
        int wavetable=!strncmp(sound,"wt_",3);
        /* gm_* packs are always note-gated like the web's soundfont trigger,
           which stops every voice at note end. */
        ev->smp_gate = wavetable || cJSON_HasObjectItem(value,"clip") || cJSON_HasObjectItem(value,"loop") ||
            cJSON_HasObjectItem(value,"release") || gm_synth_program(sound) >= 0;
        if (midi < 0) ev->freq = 0;
        const char *names[4] = {"attack","decay","sustain","release"};
        const double defaults[4] = {.001,.001,1,.01};
        double envelope[4];
        dsp_read_adsr(value,names,defaults,envelope);
        ev->att = envelope[0]; ev->dec = envelope[1];
        ev->sus = envelope[2]; ev->rel = envelope[3];
        if (wavetable) {
            float *buffer; int length;
            smp_get(ev->smp,&buffer,&length);
            if (!buffer || length<1) return 0;
            /* Superdough always slices wt_* samples into 2048-sample frames
               (the last frame may be shorter). A single short AKWF cycle is
               one frame with its own length. */
            ev->wt_frame_len=2048;
            ev->wt_frames=length>=2048 ? length/2048 : 1;
            ev->wt_position=control(value,"wt",0,0,1);
            ev->wt_warp=control(value,"warp",0,0,1);
            ev->wt_warp_mode=warp_mode_control(value,"warpmode");
            int wt_lfo_params=number_control(value,"wtrate") || number_control(value,"wtshape") ||
                number_control(value,"wtskew") || number_control(value,"wtsync");
            ev->wt_lfo_depth=number_control(value,"wtdepth") ?
                control(value,"wtdepth",0,0,16) : wt_lfo_params ? .5 : 0;
            if (number_control(value,"wtrate"))
                ev->wt_lfo_hz=control(value,"wtrate",0,0,20000);
            else if (number_control(value,"wtsync") && ev->cps>0)
                ev->wt_lfo_hz=fabs(ev->cps)*control(value,"wtsync",0,-64,64);
            else ev->wt_lfo_hz=ev->wt_lfo_depth>0 ? 1 : 0;
            ev->wt_lfo_shape=lfo_shape_control(value,"wtshape");
            ev->wt_lfo_skew=control(value,"wtskew",.5,.001,.999);
            const char *wt_env_names[4]={"wtattack","wtdecay","wtsustain","wtrelease"};
            const char *warp_env_names[4]={"warpattack","warpdecay","warpsustain","warprelease"};
            const double wt_env_defaults[4]={0,.5,0,.1};
            ev->wt_env_amount=number_control(value,"wtenv") ?
                control(value,"wtenv",0,-16,16) :
                any_number_control(value,wt_env_names,4) ? .5 : 0;
            dsp_read_adsr(value,wt_env_names,wt_env_defaults,ev->wt_env);
            ev->wt_warp_env_amount=number_control(value,"warpenv") ?
                control(value,"warpenv",0,-16,16) :
                any_number_control(value,warp_env_names,4) ? .5 : 0;
            dsp_read_adsr(value,warp_env_names,wt_env_defaults,ev->wt_warp_env);
            int warp_lfo_params=number_control(value,"warprate") || number_control(value,"warpshape") ||
                number_control(value,"warpskew") || number_control(value,"warpsync");
            ev->wt_warp_lfo_depth=number_control(value,"warpdepth") ?
                control(value,"warpdepth",0,0,16) : warp_lfo_params ? .5 : 0;
            if (number_control(value,"warprate"))
                ev->wt_warp_lfo_hz=control(value,"warprate",0,0,20000);
            else if (number_control(value,"warpsync") && ev->cps>0)
                ev->wt_warp_lfo_hz=fabs(ev->cps)*control(value,"warpsync",0,-64,64);
            else ev->wt_warp_lfo_hz=ev->wt_warp_lfo_depth>0 ? 1 : 0;
            ev->wt_warp_lfo_shape=lfo_shape_control(value,"warpshape");
            ev->wt_warp_lfo_skew=control(value,"warpskew",.5,.001,.999);
            ev->wt_voices=(int)control(value,"unison",1,1,WT_SAMPLE_VOICES);
            ev->wt_spread=control(value,"spread",.7,0,1);
            ev->wt_detune=control(value,"detune",0,-9600,9600);
            ev->wt_phase_rand=number_control(value,"wtphaserand") ?
                (control(value,"wtphaserand",0,0,1)>0 ? 1 : 0) : ev->wt_voices>1 ? 1 : 0;
            /* Each selected AKWF file is one cycle. Its length, not the
               manifest's nominal recording note, sets exact oscillator pitch. */
            ev->smp_speed*=length/SR*midi_to_freq(smp_base_midi(ev->smp));
        }
        const char *unit = text_control(value,"unit");
        if (!wavetable && !strcmp(unit,"c")) {
            float *buffer; int length;
            smp_get(ev->smp,&buffer,&length);
            if (buffer && length > 0) ev->smp_speed *= length/SR;
        }
        if (ev->stretch>0) {
            ev->stretch_buffer=stretch_prepare(ev);
            if (!ev->stretch_buffer) {
                log_console("stretch: preparation duration or memory limit exceeded"); return 0;
            }
        }
    }
    return ev->smp>=0;
}

/* Publisher-side shared state: inherit omitted room controls and build the
   custom-IR kernel in hap order. The kernel FFT itself runs on the shared
   preparation pool; inherited controls are applied before it is submitted. */
typedef struct {
    PrepJob base;
    int sample;
    double size, speed, begin;
    const ConvKernel *kernel;
} ConvPrepJob;

static void conv_prep_run(PrepJob *base) {
    ConvPrepJob *job=(ConvPrepJob *)base;
    job->kernel=convolution_prepare(job->sample,job->size,job->speed,job->begin);
}

static void conv_prep_destroy(PrepJob *base) {
    free(base);
}

static const ConvKernel *prepare_kernel_pooled(int sample, double size,
                                               double speed, double begin) {
    ConvPrepJob *job=calloc(1,sizeof(*job));
    if (!job) return convolution_prepare(sample,size,speed,begin);
    prep_job_init(&job->base,conv_prep_run,conv_prep_destroy);
    job->sample=sample; job->size=size; job->speed=speed; job->begin=begin;
    if (!prep_pool_submit(&job->base)) prep_job_run_inline(&job->base);
    prep_pool_wait(&job->base);
    const ConvKernel *kernel=job->kernel;
    prep_job_release(&job->base);
    return kernel;
}

static int finish_sample(AudioEvent *ev, const cJSON *value) {
    const cJSON *ir = cJSON_GetObjectItemCaseSensitive(value,"ir");
    audio_inherit_room(ev,room_explicit_fields(value)|ev->fx_room_fields);
    if (ir && ev->dsp.p[DSP_ROOM] > 0) {
        if (ev->dsp.p[DSP_SIZE] == 0) ev->dsp.p[DSP_ROOM] = 0;
        else {
            ev->ir_kernel = prepare_kernel_pooled(ev->room_sample,ev->dsp.p[DSP_SIZE],
                ev->room_speed,ev->room_begin);
            if (!ev->ir_kernel) return 0;
        }
    }
    return 1;
}

int js_audio_resolve_sample(AudioEvent *event, const cJSON *value) {
    if (!resolve_sample_source(event,value)) return 0;
    if (event->live_lv2>0) return 1;
    return finish_sample(event,value);
}

int js_audio_schedule(JsProgram *program, double cycle_start, int cycle_index, unsigned generation) {
    double seconds = audio_sec_per_cycle();
    return js_audio_schedule_arc(program,cycle_start-cycle_index*seconds,seconds,cycle_index,cycle_index+1.0,generation);
}

int js_audio_schedule_arc(JsProgram *program, double origin, double seconds,
                          double begin, double end, unsigned generation) {
    JsAudioClock clock={.seconds=seconds,.anchor_time=origin,.end=end};
    return js_audio_schedule_clock(program,&clock,begin,end,origin+end*seconds,generation);
}

static int schedule_local_ducks(const cJSON *value, double time, unsigned generation) {
    AudioEvent controls={.generation=generation,.time=time,.control_only=1,
        .smp=-1,.wt=-1,.midi_chan=-1,.midi_note=-1};
    controls.dsp.enabled=1;
    controls.orbit=(int)control(value,"orbit",control(value,"o",1,0,N_ORBITS-1),0,N_ORBITS-1);
    unsigned existing=audio_register_orbit(controls.orbit);
    const cJSON *targets=cJSON_GetObjectItemCaseSensitive(value,"duckorbit");
    if (!targets) targets=cJSON_GetObjectItemCaseSensitive(value,"duck");
    if (cJSON_IsArray(targets)) {
        int index=0; const cJSON *target;
        cJSON_ArrayForEach(target,targets) {
            int at=index++,orbit=duck_target(target);
            if (orbit<0 || !(existing&(1u<<orbit))) continue;
            if (!audio_prepare_duck_timer(orbit,time)) return 0;
            controls.duck_mask=0;
            duck_assign(value,target,at,&controls);
            event_queue_push(&controls);
        }
    } else {
        int orbit=duck_target(targets);
        if (orbit>=0 && (existing&(1u<<orbit))) {
            if (!audio_prepare_duck_timer(orbit,time)) return 0;
            duck_controls(value,&controls);
            event_queue_push(&controls);
        }
    }
    return 1;
}

static void schedule_source_eviction(double time, double stretch, unsigned generation) {
    AudioEvent ev={.time=time,.stretch=stretch,.generation=generation,
        .control_only=1,.eviction_only=1,.smp=-1,.wt=-1,.midi_chan=-1,.midi_note=-1};
    pthread_mutex_lock(&tracks_lock);
    if (generation==event_queue_generation()) event_queue_push(&ev);
    pthread_mutex_unlock(&tracks_lock);
}

typedef struct {
    PrepJob base;
    AudioEvent event;
    const cJSON *value;
    int ok;
    int inline_pending;
    uint64_t submitted;
} AudioPrepJob;

static JsAudioPrepObserver prep_observer;

void js_audio_set_prep_observer(JsAudioPrepObserver observer) {
    prep_observer=observer;
}

static void audio_prep_run(PrepJob *base) {
    AudioPrepJob *job=(AudioPrepJob *)base;
    if (prep_observer) prep_observer(&job->event);
    job->ok=resolve_sample_source(&job->event,job->value);
    prep_profile_job_completed(job->submitted);
}

static void audio_prep_destroy(PrepJob *base) {
    AudioPrepJob *job=(AudioPrepJob *)base;
    native_synth_release(job->event.native_synth);
    stretch_release(job->event.stretch_buffer);
    free(job);
}

enum { HAP_SKIP=0, HAP_SOURCE, HAP_IMMEDIATE };
typedef struct {
    const cJSON *hap;
    double time, seconds, origin;
    int kind;
} HapPlan;

static void warn_unknown_controls(JsProgram *program, const cJSON *value) {
    const cJSON *field;
    cJSON_ArrayForEach(field,value) {
        const char *name = field->string;
        if (!name || *name == '_' || dsp_supports_control(name)) continue;
        if (!strncmp(name,"lv2param_",9)) continue;
        if (!strcmp(name,"FXrelease") && cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(value,"_nativeProgram"))) continue;
        const char *known[] = {"s","n","note","freq","bank","gain","amp","velocity","pan","cps",
            "attack","decay","sustain","release","speed","detune","delay","delaytime",
            "delayfeedback","midichan","midiport","midicmd","ccn","ccv","progNum","sysexid","sysexdata",
            "midibend","miditouch","nrpnn","nrpv","midimap","duration","analyze","fft","unit","tremolosync","lpsync","hpsync","bpsync","delaysync",
            "busgain","overgain","csound","orbit","o","duckorbit","duck","duckattack","duckatt","datt",
            "duckdepth","duckonset","stretch","ir","i","irspeed","irbegin","lv2preset","lv2state","carlaProject","carlaTarget","color","colour","label","activeLabel","markcss",
            "zrand","curve","slide","deltaSlide","pitchJump","pitchJumpTime","lfo","znoise","zmod","zcrush","zdelay","zzfx",
            "byteBeatExpression","byteBeatStartTime","workletSrc","workletInputs",
            "wt","warp","warpmode","wtrate","wtdepth","wtshape","wtskew","wtsync","wtdc",
            "wtattack","wtdecay","wtsustain","wtrelease","wtenv","wtphaserand",
            "warprate","warpdepth","warpshape","warpskew","warpsync","warpdc",
            "warpattack","warpdecay","warpsustain","warprelease","warpenv",NULL};
        int implemented = 0;
        for (int i = 0; known[i]; i++) if (!strcmp(name,known[i])) { implemented = 1; break; }
        if (!implemented) javascript_warn_control(program,name);
    }
}

/* Classify without touching shared orbit state. Source preparation is
   submitted to the bounded pool; immediate haps are committed in order. */
static int plan_hap(JsProgram *program, const cJSON *hap, double time,
                    double seconds, HapPlan *plan) {
    memset(plan,0,sizeof(*plan));
    if (!isfinite(time) || !isfinite(seconds) || seconds<=0) return 0;
    const cJSON *begin=cJSON_GetObjectItemCaseSensitive(hap,"begin");
    if (!cJSON_IsNumber(begin) || !isfinite(begin->valuedouble)) return 0;
    plan->hap=hap; plan->time=time; plan->seconds=seconds;
    plan->origin=time-begin->valuedouble*seconds;
    double cutoff=event_queue_not_before();
    if (time<=cutoff || time+1/SR<audio_time_now()) return 0;
    const cJSON *value=cJSON_GetObjectItemCaseSensitive(hap,"value");
    javascript_cache_hap(program,hap,plan->origin,seconds);
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(value,"_osc")) ||
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(value,"_midi"))) {
        plan->kind=HAP_IMMEDIATE; return 1;
    }
    int csound=cJSON_HasObjectItem(value,"csound");
    if (!csound) {
        const char *sound=text_control(value,"s");
        if (!strcmp(sound,"~") || !strcmp(sound,"-") || !strcmp(sound,"_")) {
            plan->kind=HAP_IMMEDIATE; return 1;
        }
    }
    warn_unknown_controls(program,value);
    AudioEvent event;
    if (!audio_event_values(hap,plan->origin,seconds,&event,0)) return 0;
    if (event.lfo_unsupported)
        javascript_warn_once(program,"lfo-target",
            "native audio: LFO modulation is implemented for gain, cutoff, source and pan");
    if (event.env_unsupported)
        javascript_warn_once(program,"env-target",
            "native audio: generic .env() does not modulate (matches the deployed player, whose envelope node never starts)");
    plan->kind=csound?HAP_IMMEDIATE:HAP_SOURCE;
    return 1;
}

static AudioPrepJob *prepare_hap_job(const HapPlan *plan, unsigned generation) {
    AudioEvent event;
    if (!audio_event_values(plan->hap,plan->origin,plan->seconds,&event,0)) return NULL;
    event.generation=generation; event.duck_mask=0;
    AudioPrepJob *job=calloc(1,sizeof(*job));
    if (!job) return NULL;
    prep_job_init(&job->base,audio_prep_run,audio_prep_destroy);
    job->event=event;
    job->submitted=prep_profile_now_ns();
    prep_profile_job_submitted();
    job->value=cJSON_GetObjectItemCaseSensitive(plan->hap,"value");
    /* If the pool is unavailable, run at this hap's publication turn instead
       of blocking preparation of an earlier, independent hap. */
    if (!prep_pool_submit(&job->base)) job->inline_pending=1;
    return job;
}

static void discard_prep_job(AudioPrepJob *job) {
    if (!job) return;
    if (!job->inline_pending) prep_pool_wait(&job->base);
    prep_job_release(&job->base);
}

/* Dominant MIDI extras, matching the deployed web output's message set and
   validation. Sends run in web order (program, sysex, CC, bend, touch,
   cycle-zero start, transport command); a rejected extra drops itself and
   every later extra, matching the web scheduler's throw-abort behavior.
   String CC names, MIDI maps and NRPN stay unimplemented and warn through
   the unknown-control path. */
static int midi_int(const cJSON *item, int min, int max, int *out) {
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)) return 0;
    double truncated = item->valuedouble < 0 ? ceil(item->valuedouble) : floor(item->valuedouble);
    if (truncated < min || truncated > max || truncated != item->valuedouble) return 0;
    *out = (int)truncated;
    return 1;
}

/* WebMidi's controller names, including the deliberately spelled
   "sustenutopedal". Unknown strings must abort later extras, not become 0. */
static int midi_controller(const cJSON *item, int *out) {
    static const char *const names[128]={
        "bankselectcoarse","modulationwheelcoarse","breathcontrollercoarse","controller3",
        "footcontrollercoarse","portamentotimecoarse","dataentrycoarse","volumecoarse",
        "balancecoarse","controller9","pancoarse","expressioncoarse","effectcontrol1coarse","effectcontrol2coarse",
        "controller14","controller15","generalpurposeslider1","generalpurposeslider2","generalpurposeslider3","generalpurposeslider4",
        "controller20","controller21","controller22","controller23","controller24","controller25","controller26","controller27",
        "controller28","controller29","controller30","controller31","bankselectfine","modulationwheelfine","breathcontrollerfine","controller35",
        "footcontrollerfine","portamentotimefine","dataentryfine","volumefine","balancefine","controller41","panfine","expressionfine",
        "effectcontrol1fine","effectcontrol2fine","controller46","controller47","controller48","controller49","controller50","controller51",
        "controller52","controller53","controller54","controller55","controller56","controller57","controller58","controller59",
        "controller60","controller61","controller62","controller63","holdpedal","portamento","sustenutopedal","softpedal",
        "legatopedal","hold2pedal","soundvariation","resonance","soundreleasetime","soundattacktime","brightness","soundcontrol6",
        "soundcontrol7","soundcontrol8","soundcontrol9","soundcontrol10","generalpurposebutton1","generalpurposebutton2","generalpurposebutton3","generalpurposebutton4",
        "controller84","controller85","controller86","controller87","controller88","controller89","controller90","reverblevel",
        "tremololevel","choruslevel","celestelevel","phaserlevel","databuttonincrement","databuttondecrement","nonregisteredparameterfine","nonregisteredparametercoarse",
        "registeredparameterfine","registeredparametercoarse","controller102","controller103","controller104","controller105","controller106","controller107",
        "controller108","controller109","controller110","controller111","controller112","controller113","controller114","controller115",
        "controller116","controller117","controller118","controller119","allsoundoff","resetallcontrollers","localcontrol","allnotesoff",
        "omnimodeoff","omnimodeon","monomodeon","polymodeon"};
    if (cJSON_IsString(item) && item->valuestring) {
        for (int i=0;i<128;i++) if (!strcmp(item->valuestring,names[i])) { *out=i; return 1; }
        return 0;
    }
    /* WebMidi converts a valid numeric CC using parseInt. */
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) || item->valuedouble<0 || item->valuedouble>=128) return 0;
    *out=(int)item->valuedouble; return 1;
}

/* Documented REPL string form: "0x77" for a manufacturer id and
   "0x01:0x02:0x03:0x04" for data. A token may be hex only with the 0x prefix;
   bare tokens are decimal. ':' separates tokens because control strings are
   mini-notation, where a space would split the value into a sequence before it
   reaches this parser. The deployed REPL throws on these strings, so this is a
   bounded superset that keeps the numeric forms untouched. */
static int midi_sysex_string(const char *text, unsigned char *out, int cap, int *len) {
    if (!text || !*text || cap < 1) return 0;
    int n = 0;
    const char *p = text;
    for (;;) {
        while (*p == ':') p++;
        if (!*p) return 0; /* separator with no following token */
        int hex = 0, digits = 0, value = 0;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { hex = 1; p += 2; }
        while (*p && *p != ':') {
            char c = *p;
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (hex && c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (hex && c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return 0;
            value = value * (hex ? 16 : 10) + d;
            if (value > 255 || ++digits > (hex ? 2 : 3)) return 0;
            p++;
        }
        if (!digits || n >= cap) return 0;
        out[n++] = (unsigned char)value;
        if (!*p) break;
    }
    *len = n;
    return 1;
}

static int midi_sysex_bytes(const cJSON *item, unsigned char *out, int cap, int *len) {
    *len = 0;
    if (cJSON_IsString(item) && item->valuestring)
        return midi_sysex_string(item->valuestring, out, cap, len);
    if (cJSON_IsNumber(item)) {
        int byte = 0;
        if (cap < 1 || !midi_int(item, 0, 255, &byte)) return 0;
        out[0] = (unsigned char)byte; *len = 1;
        return 1;
    }
    if (!cJSON_IsArray(item)) return 0;
    int count = cJSON_GetArraySize(item);
    if (count < 0 || count > cap) return 0;
    const cJSON *entry = NULL;
    int n = 0;
    cJSON_ArrayForEach(entry, item) {
        int byte = 0;
        if (!midi_int(entry, 0, 255, &byte)) return 0;
        out[n++] = (unsigned char)byte;
    }
    *len = n;
    return 1;
}

static int midi_parse_extras(JsProgram *program, const cJSON *value, AudioEvent *event) {
    const cJSON *prog = cJSON_GetObjectItemCaseSensitive(value, "progNum");
    if (prog) {
        int number = 0;
        if (!cJSON_IsNumber(prog) || !isfinite(prog->valuedouble) ||
            prog->valuedouble < 0 || prog->valuedouble > 127) {
            javascript_warn_control(program, "midi progNum (requires a number 0..127)");
            return 0;
        }
        number = (int)floor(prog->valuedouble);
        event->midi_has_prog = 1; event->midi_prog = number;
    }
    const cJSON *sysex_id = cJSON_GetObjectItemCaseSensitive(value, "sysexid");
    const cJSON *sysex_data = cJSON_GetObjectItemCaseSensitive(value, "sysexdata");
    if (sysex_id && sysex_data) {
        unsigned char id[3]; int id_len = 0, data_len = 0;
        if (!midi_sysex_bytes(sysex_id, id, 3, &id_len) ||
            !midi_sysex_bytes(sysex_data, event->midi_sysex,
                (int)sizeof(event->midi_sysex), &data_len)) {
            javascript_warn_control(program,
                "midi sysex (requires bytes 0..255 as integers, arrays or '0x01:0x02' strings)");
            return 0;
        }
        memcpy(event->midi_sysex_id, id, (size_t)id_len);
        event->midi_has_sysex = 1;
        event->midi_sysex_id_len = id_len; event->midi_sysex_len = data_len;
    }
    const cJSON *ccn = cJSON_GetObjectItemCaseSensitive(value, "ccn");
    const cJSON *ccv = cJSON_GetObjectItemCaseSensitive(value, "ccv");
    if (ccn && ccv) {
        int number = 0;
        if (!midi_controller(ccn, &number) || !cJSON_IsNumber(ccv) ||
            !isfinite(ccv->valuedouble) || ccv->valuedouble < 0 || ccv->valuedouble > 1) {
            javascript_warn_control(program, "midi CC (requires integer ccn 0..127 and ccv 0..1)");
            return 0;
        }
        event->midi_has_cc = 1; event->midi_cc = number;
        event->midi_ccv = (int)fmax(0, fmin(127, round(ccv->valuedouble * 127)));
    }
    const cJSON *nrpnn=cJSON_GetObjectItemCaseSensitive(value,"nrpnn");
    const cJSON *nrpv=cJSON_GetObjectItemCaseSensitive(value,"nrpv");
    if (nrpnn && nrpv) {
        int number[2], data[2], count=cJSON_IsArray(nrpv)?cJSON_GetArraySize(nrpv):1;
        int n=cJSON_GetArraySize(nrpnn);
        if (!cJSON_IsArray(nrpnn) || n<2 || n>256 || count<1 || count>256) {
            javascript_warn_control(program,"midi NRPN (requires bounded parameter/data arrays)"); return 0;
        }
        int valid=1;
        const cJSON *byte;
        cJSON_ArrayForEach(byte,nrpnn) { int ignored; if (!midi_int(byte,0,255,&ignored)) valid=0; }
        for (int i=0;i<2;i++) if (!midi_int(cJSON_GetArrayItem(nrpnn,i),0,127,&number[i])) valid=0;
        for (int i=0;i<count;i++) {
            const cJSON *v=cJSON_IsArray(nrpv)?cJSON_GetArrayItem(nrpv,i):nrpv;
            if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble<0 || v->valuedouble>127) valid=0;
            else if (i<2) data[i]=(int)v->valuedouble;
        }
        if (!valid) { javascript_warn_control(program,"midi NRPN (requires two parameter bytes and data bytes 0..127)"); return 0; }
        unsigned char cc[6][2]={{99,number[0]},{98,number[1]},{6,data[0]},{38,count>1?data[1]:0},{101,127},{100,127}};
        for (int i=0;i<6;i++) if (i!=3 || count>1) {
            memcpy(event->midi_nrpn[event->midi_nrpn_count++],cc[i],2);
        }
    }
    const cJSON *bend = cJSON_GetObjectItemCaseSensitive(value, "midibend");
    if (bend) {
        if (!cJSON_IsNumber(bend) || !isfinite(bend->valuedouble) ||
            bend->valuedouble < -1 || bend->valuedouble > 1) {
            javascript_warn_control(program, "midi bend (requires midibend -1..1)");
            return 0;
        }
        event->midi_has_bend = 1;
        event->midi_bend = (int)(round((bend->valuedouble + 1) * 16383 / 2) - 8192);
    }
    const cJSON *touch = cJSON_GetObjectItemCaseSensitive(value, "miditouch");
    if (touch) {
        if (!cJSON_IsNumber(touch) || !isfinite(touch->valuedouble) ||
            touch->valuedouble < 0 || touch->valuedouble > 1) {
            javascript_warn_control(program, "midi touch (requires miditouch 0..1)");
            return 0;
        }
        event->midi_has_touch = 1;
        event->midi_touch = (int)fmax(0, fmin(127, round(touch->valuedouble * 127)));
    }
    return 1;
}

static void midi_parse_command(JsProgram *program, const cJSON *value, AudioEvent *event) {
    const cJSON *cmd = cJSON_GetObjectItemCaseSensitive(value, "midicmd");
    if (cJSON_IsString(cmd) && cmd->valuestring) {
        if (!strcmp(cmd->valuestring, "clock") || !strcmp(cmd->valuestring, "midiClock"))
            event->midi_cmd = 1;
        else if (!strcmp(cmd->valuestring, "start"))
            event->midi_cmd = 2;
        else if (!strcmp(cmd->valuestring, "continue"))
            event->midi_cmd = 3;
        else if (!strcmp(cmd->valuestring, "stop"))
            event->midi_cmd = 4;
        return;
    }
    if (!cJSON_IsArray(cmd)) return;
    if (cJSON_GetArraySize(cmd) < 1) return;
    const cJSON *head = cJSON_GetArrayItem(cmd, 0);
    const char *name = cJSON_IsString(head) ? head->valuestring : "";
    if (!strcmp(name, "progNum") && cJSON_GetArraySize(cmd) == 2) {
        const cJSON *number = cJSON_GetArrayItem(cmd, 1);
        if (!cJSON_IsNumber(number) || !isfinite(number->valuedouble) ||
            number->valuedouble < 0 || number->valuedouble > 127) {
            javascript_warn_control(program, "midi progNum (requires a number 0..127)");
            return;
        }
        event->midi_has_post_prog=1; event->midi_post_prog=(int)floor(number->valuedouble);
        return;
    }
    if (!strcmp(name, "sysex") && cJSON_GetArraySize(cmd) == 3) {
        if (midi_sysex_bytes(cJSON_GetArrayItem(cmd,1),event->midi_post_sysex_id,3,&event->midi_post_sysex_id_len) &&
                cJSON_IsArray(cJSON_GetArrayItem(cmd,2)) &&
                midi_sysex_bytes(cJSON_GetArrayItem(cmd,2),event->midi_post_sysex,256,&event->midi_post_sysex_len)) event->midi_has_post_sysex=1;
        else javascript_warn_control(program,"midi sysex (requires bytes 0..255 as integers, arrays or '0x01:0x02' strings)");
        return;
    }
    if (!strcmp(name, "cc")) {
        javascript_warn_control(program, "midi CC (requires integer ccn 0..127 and ccv 0..1)");
        return;
    }
}

/* Dominant MIDI bypasses local audio like the web output; the channel
   defaults to 1 there. A note is sent only for haps carrying a note key,
   matching the web scheduler; rests and pitch-less drums carry no note but
   still deliver extras. Returns 0 when there is nothing to send. */
static int midi_prepare_dominant(JsProgram *program, const cJSON *value,
    const cJSON *hap, double seconds, AudioEvent *event) {
    const cJSON *settings=cJSON_GetObjectItemCaseSensitive(value,"_midiOptions");
    const cJSON *channels=cJSON_GetObjectItemCaseSensitive(value,"_midiChannels");
    const cJSON *port=cJSON_GetObjectItemCaseSensitive(value,"midiport");
    if (!cJSON_IsString(port) && !cJSON_IsNumber(port))
        port=cJSON_GetObjectItemCaseSensitive(settings,"port");
    if (cJSON_IsString(port) && port->valuestring && port->valuestring[0])
        snprintf(event->midi_port,sizeof(event->midi_port),"%.*s",
            (int)sizeof(event->midi_port)-1,port->valuestring);
    else if (cJSON_IsNumber(port) && isfinite(port->valuedouble))
        snprintf(event->midi_port,sizeof(event->midi_port),"#%ld",lround(port->valuedouble));
    if (channels) {
        if (!cJSON_IsArray(channels) || cJSON_GetArraySize(channels)>16) {
            javascript_warn_control(program,"midi channels (at most 16 destinations per hap)"); return 0;
        }
        const cJSON *channel;
        cJSON_ArrayForEach(channel,channels) {
            if (!cJSON_IsNumber(channel) || !isfinite(channel->valuedouble) ||
                channel->valuedouble<0 || channel->valuedouble>15 || floor(channel->valuedouble)!=channel->valuedouble) {
                javascript_warn_control(program,"midi channels (requires resolved integers 0..15)"); return 0;
            }
            event->midi_channels[event->midi_channel_count++]=(unsigned char)channel->valueint;
        }
        if (!event->midi_channel_count) event->midi_channel_count=-1;
        event->midi_chan=event->midi_channel_count>0?event->midi_channels[0]:0;
    }
    int controller=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(settings,"isController"));
    if (!channels && !cJSON_HasObjectItem(value,"midichan")) {
        const cJSON *channel=cJSON_GetObjectItemCaseSensitive(settings,"midichannel");
        if (cJSON_IsNumber(channel) && channel->valuedouble>=1 && channel->valuedouble<=16)
            event->midi_chan=(int)channel->valuedouble-1;
    }
    if (event->midi_chan < 0) event->midi_chan = 0;
    const cJSON *mapped=cJSON_GetObjectItemCaseSensitive(value,"_midiMapped");
    if (mapped) {
        const cJSON *entry;
        int valid=cJSON_IsArray(mapped) && cJSON_GetArraySize(mapped)<=32;
        cJSON_ArrayForEach(entry,mapped) {
            int cc;
            const cJSON *v=cJSON_GetObjectItemCaseSensitive(entry,"ccv");
            if (!valid || !midi_controller(cJSON_GetObjectItemCaseSensitive(entry,"ccn"),&cc) ||
                !cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble<0 || v->valuedouble>1) {
                javascript_warn_control(program,"midi map (requires valid CCs; at most 32 sends per hap)");
                event->midi_note=-1; return event->midi_map_count>0;
            }
            event->midi_map[event->midi_map_count][0]=(unsigned char)cc;
            event->midi_map[event->midi_map_count++][1]=(unsigned char)round(v->valuedouble*127);
        }
        if (!valid) { javascript_warn_control(program,"midi map (requires an array)"); return 0; }
    }
    /* MIDI has its own defaults and ignores the local audio gain stages. */
    const cJSON *gain = cJSON_GetObjectItemCaseSensitive(value, "gain");
    const cJSON *velocity = cJSON_GetObjectItemCaseSensitive(value, "velocity");
    if (!gain) gain=cJSON_GetObjectItemCaseSensitive(settings,"gain");
    if (!velocity) velocity=cJSON_GetObjectItemCaseSensitive(settings,"velocity");
    if (!controller && cJSON_HasObjectItem(value,"note")) {
        const cJSON *offset=cJSON_GetObjectItemCaseSensitive(settings,"noteOffsetMs");
        double offset_ms=cJSON_IsNumber(offset) && isfinite(offset->valuedouble)?offset->valuedouble:1;
        if (offset_ms<0) {
            javascript_warn_control(program,"midi noteOffsetMs (requires nonnegative milliseconds)");
            event->midi_note=-1; return event->midi_map_count>0;
        }
        const cJSON *begin=cJSON_GetObjectItemCaseSensitive(hap,"begin");
        const cJSON *end=cJSON_GetObjectItemCaseSensitive(hap,"end");
        double duration=cJSON_IsNumber(begin) && cJSON_IsNumber(end)?(end->valuedouble-begin->valuedouble)*seconds:event->duration;
        const cJSON *gate=cJSON_GetObjectItemCaseSensitive(value,"duration");
        const cJSON *clip=cJSON_GetObjectItemCaseSensitive(value,"clip");
        if (cJSON_IsNumber(gate)) duration=gate->valuedouble*seconds;
        if (cJSON_IsNumber(clip)) duration*=clip->valuedouble;
        if (!isfinite(duration) || duration<0 || duration>86400) {
            javascript_warn_control(program,"midi duration (requires bounded nonnegative seconds)");
            event->midi_note=-1; return event->midi_map_count>0;
        }
        event->midi_duration=duration-fmin(offset_ms/1000,duration/2);
    }
    event->gain = (gain && cJSON_IsNumber(gain) ? gain->valuedouble : 1) *
        (velocity && cJSON_IsNumber(velocity) ? velocity->valuedouble : .9);
    const cJSON *resolved_gain=cJSON_GetObjectItemCaseSensitive(value,"_midiGain");
    if (resolved_gain) event->gain=cJSON_IsNumber(resolved_gain)?resolved_gain->valuedouble:NAN;
    if (!controller && cJSON_HasObjectItem(value, "note") &&
        (!isfinite(event->gain) || event->gain < 0 || event->gain > 1)) {
        javascript_warn_control(program, "midi velocity (requires gain * velocity 0..1)");
        event->midi_note=-1; return event->midi_map_count>0;
    }
    int clean = midi_parse_extras(program, value, event);
    int extras = event->midi_has_prog || event->midi_has_sysex || event->midi_has_cc ||
        event->midi_has_bend || event->midi_has_touch || event->midi_nrpn_count;
    if (clean) {
        const cJSON *begin = cJSON_GetObjectItemCaseSensitive(hap, "begin");
        if (cJSON_IsNumber(begin) && isfinite(begin->valuedouble) && begin->valuedouble == 0)
            event->midi_cycle_start = 1;
        midi_parse_command(program, value, event);
    }
    if (cJSON_HasObjectItem(value, "note") && event->midi_note < 0) {
        if (event->freq > 0)
            event->midi_note = (int)fmax(0, fmin(127, round(69 + 12 * log2(event->freq / 440))));
    } else if (!cJSON_HasObjectItem(value, "note")) {
        event->midi_note = -1;
    }
    if (controller) event->midi_note=-1;
    return event->midi_note >= 0 || extras || event->midi_map_count || event->midi_cycle_start ||
        event->midi_cmd || event->midi_has_post_prog || event->midi_has_post_sysex;
}

static void commit_immediate_hap(JsProgram *program, const HapPlan *plan, unsigned generation) {
    if (plan->time+1/SR<audio_time_now()) return;
    const cJSON *hap=plan->hap;
    const cJSON *value=cJSON_GetObjectItemCaseSensitive(hap,"value");
    int osc=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(value,"_osc"));
    int midi=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(value,"_midi"));
    if (osc) {
        AudioEvent event;
        if (audio_event_values(hap,plan->origin,plan->seconds,&event,0)) {
            double cycle=control(hap,"begin",0,-1e12,1e12);
            if (osc_output_prepare(&event.osc,value,cycle,1/plan->seconds,event.duration)) {
                event.osc_only=1; event.generation=generation;
                if (midi && midi_prepare_dominant(program,value,hap,plan->seconds,&event)) event.midi_only=1;
                event_queue_push(&event);
            } else javascript_warn_control(program,"OSC (invalid address, value or packet limit)");
        }
        return;
    }
    int csound=cJSON_HasObjectItem(value,"csound");
    if (!csound && !midi) {
        if (!schedule_local_ducks(value,plan->time,generation)) return;
        const char *sound=text_control(value,"s");
        if (!strcmp(sound,"~") || !strcmp(sound,"-") || !strcmp(sound,"_")) {
            schedule_source_eviction(plan->time,cJSON_HasObjectItem(value,"stretch")?1:0,generation);
            return;
        }
    } else if (midi && !csound) {
        const char *sound=text_control(value,"s");
        if ((!strcmp(sound,"~") || !strcmp(sound,"-") || !strcmp(sound,"_")) &&
            !cJSON_HasObjectItem(value,"ccn") && !cJSON_HasObjectItem(value,"ccv") &&
            !cJSON_HasObjectItem(value,"progNum") && !cJSON_HasObjectItem(value,"sysexid") &&
            !cJSON_HasObjectItem(value,"sysexdata") && !cJSON_HasObjectItem(value,"midibend") &&
            !cJSON_HasObjectItem(value,"miditouch") && !cJSON_HasObjectItem(value,"midicmd"))
            return;
    }
    AudioEvent event;
    if (!audio_event_values(hap,plan->origin,plan->seconds,&event,0)) return;
    event.generation=generation; event.duck_mask=0;
    if (midi) {
        if (!midi_prepare_dominant(program,value,hap,plan->seconds,&event)) return;
        event.midi_only=1;
    }
    if (csound) {
        if (!midi) event.midi_chan=event.midi_note=-1; /* Dominant output bypasses ALSA too. */
        pthread_mutex_lock(&tracks_lock);
        int current=generation==event_queue_generation();
        int scheduled=current && csound_program_schedule(javascript_csound(program),value,event.time,event.duration,event.freq);
        /* MIDI fires independently of Csound scheduling: a live renderer
           failure still warns, but the external note goes out. */
        if (current && !scheduled)
            javascript_warn_control(program,"csound (renderer unavailable or event rejected)");
        if (scheduled) event.csound_only=1;
        if (scheduled || midi) event_queue_push(&event);
        pthread_mutex_unlock(&tracks_lock);
    } else if (midi) event_queue_push(&event);
}

static void commit_source_hap(JsProgram *program, const HapPlan *plan,
                              AudioPrepJob *job, unsigned generation) {
    const cJSON *value=cJSON_GetObjectItemCaseSensitive(plan->hap,"value");
    if (!job) {
        schedule_source_eviction(plan->time,0,generation);
        javascript_warn_control(program,"sound (preparation allocation failed)");
        return;
    }
    if (job->inline_pending) {
        prep_job_run_inline(&job->base);
        job->inline_pending=0;
    }
    prep_pool_wait(&job->base);
    /* Once the program changed, shared state and queue events are obsolete.
       The completed job is discarded without withdrawing committed audio. */
    if (generation!=event_queue_generation()) {
        prep_job_release(&job->base); return;
    }
    if (!job->ok || !finish_sample(&job->event,value)) {
        schedule_source_eviction(job->event.time,job->event.stretch,generation);
        if (cJSON_HasObjectItem(value,"ir"))
            javascript_warn_control(program,"ir (sample unavailable or convolution limit exceeded)");
        else javascript_warn_control(program,"sound (sample unavailable or unknown source)");
        prep_job_release(&job->base); return;
    }
    if (job->event.time+1/SR<audio_time_now()) {
        /* Preparation missed the onset. A note that is still sounding starts
           now and keeps its original end. One that has already ended is dropped. */
        double now=audio_time_now();
        prep_profile_event(PREP_LATE_SOURCE,job->event.time,now);
        double late=now-job->event.time;
        if (!isfinite(late) || !(job->event.duration>late+1/SR)) {
            javascript_warn_once(program,"late sound preparation",
                "native audio: sound preparation finished after the note ended; that note was skipped");
            prep_job_release(&job->base); return;
        }
        job->event.duration-=late;
        job->event.time=now;
    }
    pthread_mutex_lock(&tracks_lock);
    int current=generation==event_queue_generation();
    if (current && audio_prepare_delay(&job->event)) {
        audio_prepare_room(&job->event);
        event_queue_push(&job->event);
    }
    else if (current) javascript_warn_control(program,"delay (pending control capacity exceeded)");
    pthread_mutex_unlock(&tracks_lock);
    prep_job_release(&job->base);
}

typedef struct { const cJSON *hap; int index; } OrderedHap;
static int hap_order(const void *left, const void *right) {
    const OrderedHap *a=left,*b=right;
    double ta=cJSON_GetObjectItemCaseSensitive(a->hap,"begin")->valuedouble;
    double tb=cJSON_GetObjectItemCaseSensitive(b->hap,"begin")->valuedouble;
    if (ta<tb) return -1;
    if (ta>tb) return 1;
    int ca=cJSON_HasObjectItem(cJSON_GetObjectItemCaseSensitive(a->hap,"value"),"cps");
    int cb=cJSON_HasObjectItem(cJSON_GetObjectItemCaseSensitive(b->hap,"value"),"cps");
    return ca!=cb?cb-ca:a->index-b->index;
}

#define PREP_BATCH 64
int js_audio_schedule_clock(JsProgram *program, JsAudioClock *clock,
                            double begin, double end, double tick_end, unsigned generation) {
    clock->end=end;
    for (int pass=0;pass<32;pass++) {
        cJSON *haps=javascript_query_audio(program,begin,end,1/clock->seconds);
        if (!cJSON_IsArray(haps)) { cJSON_Delete(haps); return 0; }
        int count=cJSON_GetArraySize(haps);
        if (count>4096) { cJSON_Delete(haps); return 0; }
        OrderedHap *ordered=count?malloc((size_t)count*sizeof(*ordered)):NULL;
        if (count && !ordered) { cJSON_Delete(haps); return 0; }
        const cJSON *hap; int n=0;
        cJSON_ArrayForEach(hap,haps) {
            const cJSON *onset=cJSON_GetObjectItemCaseSensitive(hap,"begin");
            const cJSON *finish=cJSON_GetObjectItemCaseSensitive(hap,"end");
            const cJSON *value=cJSON_GetObjectItemCaseSensitive(hap,"value");
            if (!cJSON_IsNumber(onset) || !cJSON_IsNumber(finish) || !cJSON_IsObject(value) ||
                !isfinite(onset->valuedouble) || !isfinite(finish->valuedouble) || finish->valuedouble<onset->valuedouble) {
                javascript_warn_control(program,"invalid scheduled hap"); continue;
            }
            ordered[n]=(OrderedHap){hap,n}; n++;
        }
        count=n;
        if (count) qsort(ordered,(size_t)count,sizeof(*ordered),hap_order);
        HapPlan *plans=count?calloc((size_t)count,sizeof(*plans)):NULL;
        if (count && !plans) { free(ordered); cJSON_Delete(haps); return 0; }
        int nplans=0, changed=0;
        for (int i=0;i<count;i++) {
            hap=ordered[i].hap;
            double onset=cJSON_GetObjectItemCaseSensitive(hap,"begin")->valuedouble;
            double target=(onset-clock->anchor_cycle)*clock->seconds+clock->anchor_time;
            if (!isfinite(target) || target>tick_end+1e-9) continue;
            const cJSON *value=cJSON_GetObjectItemCaseSensitive(hap,"value");
            const cJSON *cps=cJSON_GetObjectItemCaseSensitive(value,"cps");
            double event_seconds=clock->seconds;
            if (cps) {
                if (!cJSON_IsNumber(cps) || !isfinite(cps->valuedouble) || cps->valuedouble<0 ||
                    (cps->valuedouble>0 && !isfinite(1/cps->valuedouble)) || !isfinite(240*cps->valuedouble)) {
                    javascript_warn_control(program,"cps (requires a nonnegative finite tempo)"); continue;
                }
                event_seconds=cps->valuedouble==0?HUGE_VAL:1/cps->valuedouble;
            }
            if (plan_hap(program,hap,target,event_seconds,&plans[nplans])) nplans++;
            if (cps && event_seconds!=clock->seconds) {
                clock->seconds=event_seconds;
                clock->anchor_cycle=onset; clock->anchor_time=target;
                changed=1;
            }
        }
        int ok=1;
        int group=PREP_BATCH;
        int pool_capacity=prep_pool_capacity();
        if (pool_capacity>0 && pool_capacity<group) group=pool_capacity;
        if (group<1) group=1;
        for (int base=0;base<nplans && ok;base+=group) {
            int stop=base+group; if (stop>nplans) stop=nplans;
            if (generation!=event_queue_generation()) { ok=0; break; }
            AudioPrepJob **jobs=calloc((size_t)(stop-base),sizeof(*jobs));
            if (!jobs) { ok=0; break; }
            for (int i=base;i<stop;i++) if (plans[i].kind==HAP_SOURCE)
                jobs[i-base]=prepare_hap_job(&plans[i],generation);
            for (int i=base;i<stop;i++) {
                AudioPrepJob *job=jobs[i-base];
                if (!ok || generation!=event_queue_generation()) {
                    ok=0;
                    discard_prep_job(job);
                    continue;
                }
                int plan_owned=event_queue_publish_begin();
                if (plans[i].kind==HAP_SOURCE) {
                    /* Superdough creates duck timers before awaiting its
                       source. Commit that accepted prefix independently of
                       source completion, including partial timer failures. */
                    const cJSON *value=cJSON_GetObjectItemCaseSensitive(plans[i].hap,"value");
                    int due=plans[i].time+1/SR>=audio_time_now();
                    if (!due) prep_profile_event(PREP_LATE_SOURCE,plans[i].time,audio_time_now());
                    /* A slow earlier source (including a failed lookup) must
                       not drop this sibling. Skip late duck timers, but still
                       let commit_source_hap start the note immediately when
                       its original duration has not elapsed. */
                    int accepted=!due || schedule_local_ducks(value,plans[i].time,generation);
                    int current=generation==event_queue_generation();
                    int committed=event_queue_publish_finish(plan_owned,current);
                    if (!current || !committed) ok=0;
                    if (!accepted || !ok) { discard_prep_job(job); continue; }
                    plan_owned=event_queue_publish_begin();
                }
                if (plans[i].kind==HAP_SOURCE) commit_source_hap(program,&plans[i],job,generation);
                else commit_immediate_hap(program,&plans[i],generation);
                int current=generation==event_queue_generation();
                int committed=event_queue_publish_finish(plan_owned,current);
                if (plan_owned && !committed) {
                    ok=0;
                    log_console("event queue: publication discarded");
                }
                if (!current) ok=0;
            }
            free(jobs);
        }
        free(plans); free(ordered); cJSON_Delete(haps);
        if (!ok) return 0;
        if (!changed) return 1;
        begin=end;
        end=clock->anchor_cycle+(tick_end-clock->anchor_time)/clock->seconds;
        clock->end=end;
        if (end<begin || !isfinite(clock->seconds)) return 1;
    }
    javascript_warn_control(program,"cps (too many tempo changes in one scheduling window)");
    return 0;
}
