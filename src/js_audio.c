#include "js_audio.h"
#include "gm_synth.h"
#include "audio.h"
#include "scheduler.h"
#include "sampler.h"
#include "websamp.h"
#include "wavetable.h"
#include "stretch.h"
#include <ctype.h>

static double control(const cJSON *value, const char *name, double initial, double min, double max) {
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(value, name);
    if (!cJSON_IsNumber(p) || !isfinite(p->valuedouble)) return initial;
    return fmax(min, fmin(max, p->valuedouble));
}

static const char *text_control(const cJSON *value, const char *name) {
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(value, name);
    return cJSON_IsString(p) ? p->valuestring : "";
}

static int oscillator(const char *s) {
    if (native_synth_sound(s)) return INST_NATIVE;
    if (!strcmp(s,"sine")) return INST_SINE;
    if (!strcmp(s,"saw") || !strcmp(s,"sawtooth")) return INST_SAW;
    if (!strcmp(s,"square") || !strcmp(s,"pulse")) return INST_SQUARE;
    if (!strcmp(s,"tri") || !strcmp(s,"triangle")) return INST_TRI;
    if (!strcmp(s,"noise") || !strcmp(s,"white")) return INST_NOISE;
    if (!strcmp(s,"pink")) return INST_PINK;
    if (!strcmp(s,"brown")) return INST_BROWN;
    if (!strcmp(s,"supersaw")) return INST_SUPERSAW;
    if (!strncmp(s,"wt:",3)) return INST_WT;
    return -1;
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

int js_audio_event(const cJSON *hap, double origin, double seconds, AudioEvent *ev) {
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
    if (clip == 0) return 0;
    ev->duration = fmin(60,ev->duration * clip);
    ev->duration = control(v,"duration",ev->duration,.00001,60);
    ev->gain = control(v,"gain",.8,0,16) * control(v,"velocity",1,0,1) * control(v,"amp",1,0,16)
        * control(v,"busgain",1,0,16) * control(v,"overgain",1,0,16);
    if (ev->gain <= 0) return 0;
    ev->pan = control(v,"pan",.5,0,1) * 2 - 1;
    ev->has_pan=cJSON_HasObjectItem(v,"pan");
    const char *envelope_names[4] = {"attack","decay","sustain","release"};
    const double envelope_defaults[4] = {.001,.05,.6,.01};
    double envelope[4];
    dsp_read_adsr(v,envelope_names,envelope_defaults,envelope);
    ev->att = envelope[0]; ev->dec = envelope[1];
    ev->sus = envelope[2]; ev->rel = envelope[3];
    ev->smp = ev->wt = ev->midi_chan = ev->midi_note = -1;
    ev->delay_send = control(v,"delay",0,0,1);
    ev->delay_time = control(v,"delaytime",-1,0,1.99);
    ev->delay_fb = control(v,"delayfeedback",-1,0,.98);
    ev->orbit = (int)control(v,"orbit",control(v,"o",1,0,N_ORBITS-1),0,N_ORBITS-1);
    ev->duck_attack = control(v,"duckattack",control(v,"duckatt",control(v,"datt",.2,0,10),0,10),0,10);
    ev->duck_depth = control(v,"duckdepth",1,0,1);
    {
        const cJSON *duck = cJSON_GetObjectItemCaseSensitive(v,"duckorbit");
        if (!duck) duck = cJSON_GetObjectItemCaseSensitive(v,"duck");
        if (cJSON_IsNumber(duck) && isfinite(duck->valuedouble)) {
            int o = (int)duck->valuedouble;
            if (o >= 0 && o < N_ORBITS) ev->duck_mask = 1u << o;
        } else if (cJSON_IsString(duck) && duck->valuestring) {
            char text[64];
            snprintf(text,sizeof(text),"%.63s",duck->valuestring);
            char *save = NULL;
            for (char *tok = strtok_r(text,":",&save); tok; tok = strtok_r(NULL,":",&save)) {
                char *end = NULL;
                long o = strtol(tok,&end,10);
                if (end != tok && end && !*end && o >= 0 && o < N_ORBITS) ev->duck_mask |= 1u << o;
            }
        }
    }
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
    if (cJSON_HasObjectItem(v,"delaysync")) ev->delay_time = control(v,"delaysync",.1875,0,64)*seconds;
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

static int resolve_sample(AudioEvent *ev, const cJSON *value) {
    if (ev->inst==INST_NATIVE || cJSON_HasObjectItem(value,"_nativeProgram")) {
        ev->native_synth=native_synth_prepare(value,ev->freq,ev->duration);
        if (!ev->native_synth) { log_console("native synth: unsupported program or preparation limit exceeded"); return 0; }
    }
    const cJSON *ir = cJSON_GetObjectItemCaseSensitive(value,"ir");
    if (!ir && ev->dsp.p[DSP_ROOM]>0)
        ev->ir_kernel=convolution_prepare_reverb(ev->dsp.p[DSP_SIZE],ev->dsp.p[DSP_ROOMFADE],ev->dsp.p[DSP_ROOMLP],ev->dsp.p[DSP_ROOMDIM]);
    if (ir && ev->dsp.p[DSP_ROOM] > 0) {
        if (!cJSON_IsString(ir) || !ir->valuestring[0]) return 0;
        char token[256];
        int n = snprintf(token,sizeof(token),"%s:%d",ir->valuestring,
            (int)floor(control(value,"i",1,-1000000,1000000)+.5));
        if (n < 0 || (size_t)n >= sizeof(token)) return 0;
        int sample = websamp_acquire(token,-1);
        if (sample < 0) return 0;
        if (ev->dsp.p[DSP_SIZE] == 0) ev->dsp.p[DSP_ROOM] = 0;
        else {
            ev->ir_kernel = convolution_prepare(sample,ev->dsp.p[DSP_SIZE],
                control(value,"irspeed",1,-8,8),control(value,"irbegin",0,0,1));
            if (!ev->ir_kernel) return 0;
        }
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
    if (!mapped && ev->smp<0 && !*bank && gm_synth_program(sound)>=0) return gm_synth_prepare(ev);
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
        ev->smp_gate = wavetable || cJSON_HasObjectItem(value,"clip") || cJSON_HasObjectItem(value,"loop") ||
            cJSON_HasObjectItem(value,"release");
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
    return 1;
}

void js_audio_schedule(JsProgram *program, double cycle_start, int cycle_index, unsigned generation) {
    double seconds = audio_sec_per_cycle();
    cJSON *haps = javascript_query_audio(program,cycle_index,cycle_index+1.0,1/seconds);
    if (!cJSON_IsArray(haps)) { cJSON_Delete(haps); return; }
    double origin = cycle_start - cycle_index * seconds;
    javascript_cache_spans(program,haps,origin,seconds);
    const cJSON *hap;
    cJSON_ArrayForEach(hap,haps) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(hap,"value");
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(value,"_osc"))) {
            AudioEvent ev;
            if (js_audio_event(hap,origin,seconds,&ev)) {
                double cycle=control(hap,"begin",0,-1e12,1e12);
                if (osc_output_prepare(&ev.osc,value,cycle,1/seconds,ev.duration)) {
                    ev.osc_only=1; ev.generation=generation; event_queue_push(&ev);
                } else javascript_warn_control(program,"OSC (invalid address, value or packet limit)");
            }
            continue;
        }
        const cJSON *field;
        cJSON_ArrayForEach(field,value) {
            const char *name = field->string;
            if (!name || *name == '_' || dsp_supports_control(name)) continue;
            const char *known[] = {"s","n","note","freq","bank","gain","amp","velocity","pan",
                "attack","decay","sustain","release","speed","detune","delay","delaytime",
                "delayfeedback","midichan","duration","unit","tremolosync","lpsync","hpsync","bpsync","delaysync",
                "busgain","overgain","csound","orbit","o","duckorbit","duck","duckattack","duckatt","datt",
                "duckdepth","stretch","ir","i","irspeed","irbegin","color","colour","label","activeLabel","markcss",
                "zrand","curve","slide","deltaSlide","pitchJump","pitchJumpTime","lfo","znoise","zmod","zcrush","zdelay","zzfx",
                "byteBeatExpression","byteBeatStartTime","workletSrc","workletInputs",NULL};
            int implemented = 0;
            for (int i = 0; known[i]; i++) if (!strcmp(name,known[i])) { implemented = 1; break; }
            if (!implemented) javascript_warn_control(program,name);
        }
        AudioEvent ev;
        if (!js_audio_event(hap,origin,seconds,&ev)) continue;
        ev.generation = generation;
        if (cJSON_HasObjectItem(value,"csound")) {
            if (!csound_program_schedule(javascript_csound(program),value,ev.time,ev.duration,ev.freq))
                javascript_warn_control(program,"csound (renderer unavailable or event rejected)");
            else { ev.csound_only = 1; event_queue_push(&ev); }
            continue;
        }
        if (!resolve_sample(&ev,value)) {
            native_synth_release(ev.native_synth);
            stretch_release(ev.stretch_buffer);
            if (cJSON_HasObjectItem(value,"ir"))
                javascript_warn_control(program,"ir (sample unavailable or convolution limit exceeded)");
            continue;
        }
        if (ev.dsp.p[DSP_ROOM]>0 && !ev.ir_kernel && !cJSON_HasObjectItem(value,"ir"))
            javascript_warn_control(program,"generated room (IR duration/cache limit; using native Schroeder fallback)");
        event_queue_push(&ev);
        native_synth_release(ev.native_synth);
        stretch_release(ev.stretch_buffer);
    }
    cJSON_Delete(haps);
}
