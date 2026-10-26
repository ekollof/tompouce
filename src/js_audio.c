#include "js_audio.h"
#include "gm_synth.h"
#include "audio.h"
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

static int audio_event_values(const cJSON *hap, double origin, double seconds, AudioEvent *ev, int include_ducks) {
    const cJSON *begin = cJSON_GetObjectItemCaseSensitive(hap,"begin");
    const cJSON *end = cJSON_GetObjectItemCaseSensitive(hap,"end");
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(hap,"value");
    if (!ev || !cJSON_IsObject(v) || !cJSON_IsNumber(begin) || !cJSON_IsNumber(end) ||
        !isfinite(begin->valuedouble) || !isfinite(end->valuedouble) ||
        end->valuedouble <= begin->valuedouble || !isfinite(origin) || seconds <= 0 || !isfinite(seconds)) return 0;
    memset(ev,0,sizeof(*ev));
    ev->scope_mask = (unsigned)control(hap,"scopeMask",0,0,4294967295.0);
    ev->time = origin + begin->valuedouble * seconds;
    ev->duration = fmin(60,(end->valuedouble - begin->valuedouble) * seconds);
    double clip = control(v,"clip",1,0,64);
    if (clip == 0) clip=1; /* Superdough's clip || 1 fallback. */
    ev->duration = fmin(60,ev->duration * clip);
    ev->duration = control(v,"duration",ev->duration,.00001,60);
    ev->gain = control(v,"gain",.8,0,16) * control(v,"velocity",1,0,1) * control(v,"amp",1,0,16)
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
        if (cJSON_HasObjectItem(v,sync[i])) ev->dsp.p[DSP_LPRATE+i*6] = control(v,sync[i],0,0,1024)/seconds;
    const char *sound = text_control(v,"s");
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
    if (ev->inst!=INST_SUPERSAW)
        ev->freq *= pow(2,control(v,"detune",0,-9600,9600)/1200);
    if (ev->is_drum) ev->freq = 0;
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
    if (ev->inst==INST_NATIVE || cJSON_HasObjectItem(value,"_nativeProgram")) {
        ev->native_synth=native_synth_prepare(value,ev->freq,ev->duration);
        if (!ev->native_synth) { log_console("native synth: unsupported program or preparation limit exceeded"); return 0; }
    }
    const cJSON *ir = cJSON_GetObjectItemCaseSensitive(value,"ir");
    ev->room_params_valid=1; ev->room_sample=-1;
    ev->room_speed=control(value,"irspeed",1,-8,8);
    ev->room_begin=control(value,"irbegin",0,0,1);
    if (ir && ev->dsp.p[DSP_ROOM]>0) {
        if (!cJSON_IsString(ir) || !ir->valuestring[0]) return 0;
        char token[256];
        int n=snprintf(token,sizeof(token),"%s:%d",ir->valuestring,
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
    audio_inherit_room(ev,room_explicit_fields(value));
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
    return resolve_sample_source(event,value) && finish_sample(event,value);
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
        if (!strcmp(name,"FXrelease") && cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(value,"_nativeProgram"))) continue;
        const char *known[] = {"s","n","note","freq","bank","gain","amp","velocity","pan","cps",
            "attack","decay","sustain","release","speed","detune","delay","delaytime",
            "delayfeedback","midichan","midiport","midicmd","ccn","ccv","progNum","sysexid","sysexdata",
            "midibend","miditouch","nrpnn","nrpv","midimap","duration","unit","tremolosync","lpsync","hpsync","bpsync","delaysync",
            "busgain","overgain","csound","orbit","o","duckorbit","duck","duckattack","duckatt","datt",
            "duckdepth","duckonset","stretch","ir","i","irspeed","irbegin","color","colour","label","activeLabel","markcss",
            "zrand","curve","slide","deltaSlide","pitchJump","pitchJumpTime","lfo","znoise","zmod","zcrush","zdelay","zzfx",
            "byteBeatExpression","byteBeatStartTime","workletSrc","workletInputs",NULL};
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

static int midi_sysex_bytes(const cJSON *item, unsigned char *out, int cap, int *len) {
    *len = 0;
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
            !cJSON_IsArray(sysex_data) ||
            !midi_sysex_bytes(sysex_data, event->midi_sysex,
                (int)sizeof(event->midi_sysex), &data_len)) {
            javascript_warn_control(program, "midi sysex (requires integer id/data bytes 0..255)");
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
        else javascript_warn_control(program,"midi sysex (requires integer id/data bytes 0..255)");
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
    if (plan->time+1/SR<audio_time_now()) {
        prep_profile_event(PREP_LATE_SOURCE,plan->time,audio_time_now());
        discard_prep_job(job);
        return;
    }
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
        prep_profile_event(PREP_LATE_SOURCE,job->event.time,audio_time_now());
        javascript_warn_control(program,"late sound preparation (note skipped)");
        prep_job_release(&job->base); return;
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
                    int accepted=due &&
                        schedule_local_ducks(value,plans[i].time,generation);
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
