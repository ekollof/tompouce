#include "audio.h"
#include <portaudio.h>
#include <math.h>
#include <ctype.h>
#include "wavetable.h"
#include "sampler.h"
#include "midi.h"
#ifdef __linux__
#include <alsa/asoundlib.h>
/* ALSA probing spams stderr, which shares the terminal with the S-Lang
   UI and corrupts arbitrary screen rows. Silence it; we log our own. */
static void alsa_silent(const char *file, int line, const char *func,
                         int err, const char *fmt, ...) {
    (void)file; (void)line; (void)func; (void)err; (void)fmt;
}
#endif

static PaStream *stream = NULL;
static double bpm = 120.0;
static int playing = 0;
static unsigned long audio_block_frames = 256;

void audio_set_block(unsigned long n) {
    if (n < 64) n = 64;
    if (n > 4096) n = 4096;
    audio_block_frames = n;
}

/* master fx params (engine reads them in the callback; see Batch: fx chain) */
static double fx_drive = 0.0;
static double fx_cutoff = 1.0;
static double fx_delay = 0.0;
static double fx_verb = 0.0;

void audio_fx_set(const char *name, double v) {
    if (!name || !isfinite(v)) return;
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    if (strcmp(name, "drive") == 0) fx_drive = v;
    else if (strcmp(name, "cutoff") == 0) fx_cutoff = v;
    else if (strcmp(name, "delay") == 0) fx_delay = v;
    else if (strcmp(name, "verb") == 0) fx_verb = v;
}
static double audio_time = 0.0;
static Voice voices[MAX_VOICES];
static pthread_mutex_t voice_lock = PTHREAD_MUTEX_INITIALIZER;

/* scope ring for the viz pane: single writer (callback), lock-free reader */
#define SCOPE_N 4096
static float scope_buf[SCOPE_N];
static unsigned scope_pos = 0;

int audio_scope_copy(float *out, int n) {
    if (!out || n <= 0) return 0;
    if (n > SCOPE_N) n = SCOPE_N;
    unsigned p = scope_pos; /* torn read = 1-sample glitch at most */
    unsigned start = p > (unsigned)n ? p - (unsigned)n : 0;
    for (int i = 0; i < n; i++)
        out[i] = scope_buf[(start + (unsigned)i) & (SCOPE_N - 1)];
    return n;
}


double audio_time_now(void){ return audio_time; }
void audio_set_bpm(double b){ if (!isfinite(b)) return; if (b<20) b=20; if (b>400) b=400; bpm=b; }
double audio_get_bpm(void){ return bpm; }
double audio_sec_per_beat(void){ return 60.0/bpm; }
double audio_sec_per_cycle(void){ return 4.0*60.0/bpm; } // 4 beats per cycle
double audio_cycle_beats(void){ return 4.0; }

int note_name_to_midi(const char *name) {
    if (!name) return 60;
    // if numeric
    char *end;
    long v = strtol(name, &end, 10);
    if (*end == 0 && end != name) {
        if (v < 0) v = 0;
        if (v > 127) v = 127;
        return (int)v;
    }
    // parse note name
    const char *p = name;
    char note = tolower((unsigned char)*p++);
    int semitone = 0;
    if (note == 'c') semitone = 0;
    else if (note == 'd') semitone = 2;
    else if (note == 'e') semitone = 4;
    else if (note == 'f') semitone = 5;
    else if (note == 'g') semitone = 7;
    else if (note == 'a') semitone = 9;
    else if (note == 'b') semitone = 11;
    else return 60;
    // accidental
    while (*p == '#' || *p == 'b' || *p == 's' || *p == 'f') {
        if (*p == '#') { semitone++; p++; }
        else if (*p == 'b') { semitone--; p++; }
        else if (*p == 's') { p++; } // sharp shorthand? skip
        else break;
    }
    int octave = 4;
    if (*p) {
        // parse number possibly negative
        octave = atoi(p);
    }
    int midi = (octave + 1)*12 + semitone;
    if (midi < 0) midi = 0;
    if (midi > 127) midi = 127;
    return midi;
}
double midi_to_freq(int midi) {
    return 440.0 * pow(2.0, (midi - 69)/12.0);
}

static void trigger_voice(Voice *v, AudioEvent *ev) {
    memset(v, 0, sizeof(Voice));
    v->active = 1;
    v->inst = ev->is_drum ? ev->inst : ev->inst;
    // if drum token, map
    if (ev->is_drum) {
        if (strcmp(ev->token, "bd")==0) v->inst = INST_KICK;
        else if (strcmp(ev->token, "sd")==0 || strcmp(ev->token,"sn")==0) v->inst = INST_SNARE;
        else if (strcmp(ev->token, "hh")==0 || strcmp(ev->token,"hat")==0) v->inst = INST_HH;
        else if (strcmp(ev->token, "cp")==0 || strcmp(ev->token,"clap")==0) v->inst = INST_CP;
        else if (strcmp(ev->token, "tom")==0) v->inst = INST_TOM;
        else v->inst = INST_HH;
    }
    v->freq = ev->freq > 0 ? ev->freq : 440.0;
    v->amp = ev->gain > 0 ? ev->gain : 0.8;
    v->pan = ev->pan;
    v->start_time = audio_time; // trigger now
    v->note_off_time = audio_time + (ev->duration > 0 ? ev->duration : 0.25);
    v->gate_end = v->note_off_time;
    v->phase = 0;
    v->age = 0;
    v->env = 0;
    v->released = 0;
    v->vatt = ev->att;
    v->vrel = ev->rel;
    v->vdec = ev->dec;
    v->vsus = ev->sus;
    v->pw = ev->pw;
    v->lpf_hz = ev->lpf;
    v->lpf_z = 0;
    v->lfo_tgt = ev->lfo_tgt;
    v->lfo_wave = ev->lfo_wave;
    v->lfo_hz = ev->lfo_hz;
    v->lfo_depth = ev->lfo_depth;
    v->lfo_phase = 0;
    v->wt = ev->wt;
    if (v->wt < 0 && ev->is_drum) v->wt = wt_find("sine");
    v->wtpos = 0;
    v->smp = ev->smp;
    v->smpbuf = NULL;
    v->smplen = 0;
    v->smppos = 0;
    v->smpstep = 1.0;
    v->smp_gate = ev->smp_gate;
    if (ev->smp >= 0) {
        smp_get(ev->smp, &v->smpbuf, &v->smplen);
        if (v->smpbuf && v->smplen > 0 && ev->freq > 0) {
            double base = midi_to_freq(smp_base_midi(ev->smp));
            if (base > 0) {
                v->smpstep = ev->freq / base;
                if (v->smpstep < 0.125) v->smpstep = 0.125;
                if (v->smpstep > 8.0) v->smpstep = 8.0;
            }
        }
    }
}

void audio_trigger_event(AudioEvent *ev) {
    if (!playing) return; // still trigger? allow
    pthread_mutex_lock(&voice_lock);    // find free voice
    int chosen = -1;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!voices[i].active) { chosen = i; break; }
    }
    if (chosen < 0) {
        /* steal quietest (level*amp), tie-break oldest */
        chosen = 0;
        double worst = voices[0].env * voices[0].amp;
        double oldest = voices[0].start_time;
        for (int i = 1; i < MAX_VOICES; i++) {
            double lvl = voices[i].env * voices[i].amp;
            if (lvl < worst || (lvl == worst && voices[i].start_time < oldest)) {
                worst = lvl;
                oldest = voices[i].start_time;
                chosen = i;
            }
        }
    }
    trigger_voice(&voices[chosen], ev);
    pthread_mutex_unlock(&voice_lock);
}

/* Audio callback */
static int pa_callback(const void *input, void *output, unsigned long frameCount,
                       const PaStreamCallbackTimeInfo *timeInfo, PaStreamCallbackFlags statusFlags, void *userData) {
    (void)input; (void)timeInfo; (void)statusFlags; (void)userData;
    float *out = (float*)output;
    for (unsigned long i = 0; i < frameCount; i++) {
        double L = 0, R = 0;
        if (playing) {
            pthread_mutex_lock(&voice_lock);
            double dt = 1.0/SR;
            for (int v = 0; v < MAX_VOICES; v++) {
                Voice *V = &voices[v];
                if (!V->active) continue;
                double age = V->age;
                // envelope
                double env = 0;
                // ADSR (for synth). For drums use custom.
                int is_drum = (V->inst >= INST_KICK && V->inst <= INST_TOM);
                if (V->smpbuf && (int)V->smppos < V->smplen) {
                    /* one-shot bank sample, mono center, linear interp.
                       Gated voices (melodic banks) stop at note end. */
                    if (V->smp_gate && !V->released && audio_time >= V->note_off_time)
                        V->released = 1;
                    double g = 1.0;
                    if (V->smp_gate && V->released) {
                        double r = V->vrel >= 0 ? V->vrel : 0.08;
                        if (r < 0.005) r = 0.005;
                        double rt = audio_time - V->note_off_time;
                        if (rt >= r) { V->active = 0; continue; }
                        g = 1.0 - rt / r;
                    }
                    int i0 = (int)V->smppos;
                    double fr = V->smppos - i0;
                    double a = V->smpbuf[i0];
                    double b = (i0 + 1 < V->smplen) ? V->smpbuf[i0 + 1] : 0.0;
                    double s = (a + (b - a) * fr) * V->amp * g;
                    L += s; R += s;
                    V->smppos += V->smpstep;
                    V->age += dt;
                    if ((int)V->smppos >= V->smplen) V->active = 0;
                    continue;
                } else if (V->smpbuf) { V->active = 0; continue; }
                if (is_drum) {
                    // per-drum envelope
                    if (V->inst == INST_KICK) {
                        double t = age;
                        double dur = 0.35;
                        if (t >= dur) { V->active = 0; continue; }
                        double fenv = exp(-t*8.0);
                        env = fenv;
                        double pitch = 150.0 - 100.0*(t/dur);
                        if (pitch < 50) pitch = 50;
                        V->freq = (1-pow(t/dur,0.5))*100 + 50; // overridden each sample? okay
                        // phase
                        V->phase += (2*M_PI * V->freq)/SR;
                        double s = wt_sample(V->wt, V->phase/(2*M_PI)) * env;
                        L += s * V->amp * 0.9; R += s * V->amp * 0.9;
                    } else if (V->inst == INST_SNARE) {
                        double t = age;
                        double dur = 0.25;
                        if (t >= dur) { V->active = 0; continue; }
                        double env = exp(-t*10.0);
                        // noise + tone
                        double noise = ((double)rand()/RAND_MAX)*2-1;
                        double tone = wt_sample(V->wt, 200.0*audio_time); // fixed 200
                        double s = (0.7*noise + 0.3*tone)*env;
                        L += s * V->amp * 0.7; R += s * V->amp * 0.7;
                    } else if (V->inst == INST_HH) {
                        double t = age;
                        double dur = 0.08;
                        if (t >= dur) { V->active = 0; continue; }
                        double env = exp(-t*40.0);
                        double noise = ((double)rand()/RAND_MAX)*2-1;
                        // simple highpass: difference
                        static double prev = 0;
                        double hp = noise - prev; prev = noise*0.5 + prev*0.5; // crude
                        double s = hp*env;
                        L += s * V->amp * 0.5; R += s * V->amp * 0.5;
                    } else if (V->inst == INST_CP) {
                        double t = age;
                        double dur = 0.2;
                        if (t >= dur) { V->active = 0; continue; }
                        double env = exp(-t*12.0);
                        double noise = ((double)rand()/RAND_MAX)*2-1;
                        double s = noise*env;
                        L += s * V->amp * 0.6; R += s * V->amp * 0.6;
                    } else if (V->inst == INST_TOM) {
                        double t = age;
                        double dur = 0.3;
                        if (t >= dur) { V->active = 0; continue; }
                        double env = exp(-t*6.0);
                        double pitch = V->freq;
                        V->phase += (2*M_PI * pitch)/SR;
                        double s = wt_sample(V->wt, V->phase/(2*M_PI))*env;
                        L += s * V->amp * 0.8; R += s * V->amp * 0.8;
                    }
                } else {
                    // synth with ADSR gate until note_off
                    // determine if released
                    if (!V->released && audio_time >= V->note_off_time) {
                        V->released = 1;
                        V->release_start_level = V->env;
                        V->release_time = audio_time;
                    }
                    double a = V->vatt >= 0 ? V->vatt : 0.005;
                    double d = V->vdec >= 0 ? V->vdec : 0.1;
                    double sus = V->vsus >= 0 ? V->vsus : 0.6;
                    double r = V->vrel >= 0 ? V->vrel : 0.15;
                    if (a < 0.001) a = 0.001;
                    if (d < 0) d = 0;
                    if (sus < 0) sus = 0;
                    if (sus > 1) sus = 1;
                    if (r < 0.005) r = 0.005;
                    // ADSR simple
                    double tgate = audio_time - V->start_time;
                    double envtarget = 0;
                    if (!V->released) {
                        if (tgate < a) envtarget = tgate/a;
                        else if (d > 0 && tgate < a+d) envtarget = 1.0 - (1.0-sus)*(tgate-a)/d;
                        else envtarget = sus;
                    } else {
                        double rt = audio_time - V->release_time;
                        if (rt >= r) { V->active = 0; envtarget = 0; }
                        else envtarget = V->release_start_level * (1.0 - rt/r);
                    }
                    V->env += (envtarget - V->env) * 0.02; // smooth
                    env = V->env;
                    /* one LFO per voice: triangle, sine, saw, square */
                    double freq = V->freq;
                    double amp = V->amp;
                    double pwuse = V->pw;
                    double fc = V->lpf_hz;
                    if (V->lfo_tgt > 0 && V->lfo_hz > 0.0) {
                        double u = V->lfo_phase / (2.0 * M_PI);
                        u -= floor(u);
                        double lfo;
                        if (V->lfo_wave == 1) lfo = sin(V->lfo_phase);
                        else if (V->lfo_wave == 2) lfo = 2.0 * u - 1.0;
                        else if (V->lfo_wave == 3) lfo = (u < 0.5) ? 1.0 : -1.0;
                        else lfo = (u < 0.5) ? (4.0 * u - 1.0) : (3.0 - 4.0 * u);
                        V->lfo_phase += (2.0 * M_PI * V->lfo_hz) / SR;
                        if (V->lfo_phase > 2.0 * M_PI)
                            V->lfo_phase -= 2.0 * M_PI * floor(V->lfo_phase / (2.0 * M_PI));
                        if (V->lfo_tgt == 1) {
                            double semi = lfo * V->lfo_depth;
                            if (semi > 24.0) semi = 24.0;
                            if (semi < -24.0) semi = -24.0;
                            freq *= pow(2.0, semi / 12.0);
                        } else if (V->lfo_tgt == 2) {
                            amp *= 1.0 + lfo * V->lfo_depth;
                            if (amp < 0.0) amp = 0.0;
                        } else if (V->lfo_tgt == 3) {
                            if (fc <= 0.0) fc = 1000.0;
                            fc += lfo * V->lfo_depth;
                        } else if (V->lfo_tgt == 4) {
                            if (pwuse < 0.0) pwuse = 0.5;
                            pwuse += lfo * V->lfo_depth;
                        }
                    }
                    // oscillator
                    double phaseinc = (2*M_PI * freq)/SR;
                    double sample = 0;
                    int w = V->inst;
                    switch (w) {
                        case INST_SINE: sample = sin(V->phase); break;
                        case INST_WT: {
                            double pos = V->wtpos / WT_SIZE;
                            sample = wt_sample(V->wt, pos);
                            V->wtpos += freq * WT_SIZE / SR;
                            if (V->wtpos >= WT_SIZE) V->wtpos -= WT_SIZE * floor(V->wtpos / WT_SIZE);
                            break;
                        }
                        case INST_SAW: sample = 2.0*(V->phase/(2*M_PI) - floor(V->phase/(2*M_PI)+0.5)); break;
                        case INST_SQUARE:
                            if (pwuse >= 0.0) {
                                if (pwuse < 0.05) pwuse = 0.05;
                                if (pwuse > 0.95) pwuse = 0.95;
                                double u = V->phase / (2.0 * M_PI);
                                u -= floor(u);
                                sample = (u < pwuse) ? 1.0 : -1.0;
                            } else {
                                sample = (sin(V->phase) >= 0) ? 1.0 : -1.0;
                            }
                            break;
                        case INST_TRI: {
                            double t = V->phase/(2*M_PI); t -= floor(t);
                            sample = 4.0*fabs(t-0.5)-1.0; break;
                        }
                        case INST_NOISE: sample = ((double)rand()/RAND_MAX)*2-1; break;
                        default: sample = sin(V->phase); break;
                    }
                    if (V->lpf_hz > 0.0 || V->lfo_tgt == 3) {
                        if (fc < 20.0) fc = 20.0;
                        if (fc > 18000.0) fc = 18000.0;
                        double coef = 1.0 - exp(-2.0 * M_PI * fc / SR);
                        V->lpf_z += coef * (sample - V->lpf_z);
                        sample = V->lpf_z;
                    }
                    V->phase += phaseinc;
                    if (V->phase > 2*M_PI) V->phase -= 2*M_PI;
                    V->age += dt;
                    double smp = sample * env * amp;
                    // pan
                    double lg = (V->pan <= 0) ? 1.0 : 1.0 - V->pan;
                    double rg = (V->pan >= 0) ? 1.0 : 1.0 + V->pan;
                    L += smp * lg;
                    R += smp * rg;
                    (void)age;
                    continue;
                }
                V->age += dt;
            }
            pthread_mutex_unlock(&voice_lock);
        }
        /* ---- master fx chain (all params plain doubles, lock-free) ---- */
        {
            int fx_on = fx_drive > 0.001 || fx_cutoff < 0.999 ||
                        fx_delay > 0.001 || fx_verb > 0.001;
            if (fx_on) {
                /* drive: pre-gain into saturation */
                if (fx_drive > 0.001) {
                    double g = 1.0 + 3.0 * fx_drive;
                    L = tanh(g * L);
                    R = tanh(g * R);
                }
                /* lowpass: one-pole, 200Hz..18kHz */
                {
                    static double lp_l = 0, lp_r = 0;
                    double fc = 200.0 * pow(90.0, fx_cutoff < 0 ? 0 : (fx_cutoff > 1 ? 1 : fx_cutoff));
                    double a = 1.0 - exp(-2.0 * 3.141592653589793 * fc / SR);
                    lp_l += a * (L - lp_l);
                    lp_r += a * (R - lp_r);
                    L = lp_l; R = lp_r;
                }
                /* delay: dotted-eighth feedback, capped 1.5s */
                {
                    static float dbuf[2][88200];
                    static int dpos = 0;
                    double dt = 0.75 * 60.0 / (bpm < 20 ? 120 : bpm);
                    if (dt > 1.5) dt = 1.5;
                    if (dt < 0.02) dt = 0.02;
                    int dl = (int)(dt * SR);
                    int rp = dpos - dl;
                    if (rp < 0) rp += 88200;
                    float de_l = dbuf[0][rp], de_r = dbuf[1][rp];
                    dbuf[0][dpos] = (float)(L + de_l * 0.35);
                    dbuf[1][dpos] = (float)(R + de_r * 0.35);
                    dpos++;
                    if (dpos >= 88200) dpos = 0;
                    L += de_l * fx_delay * 0.5;
                    R += de_r * fx_delay * 0.5;
                }
                /* schroeder verb: 4 combs + 2 allpass, mono-ized send */
                {
                    #define NCB 4
                    static float cb[NCB][1400];
                    static int cp[NCB] = {0};
                    static const int cl[NCB] = {1116, 1188, 1277, 1356};
                    static float ap[2][600];
                    static int app[2] = {0};
                    static const int apl[2] = {556, 441};
                    double mono = (L + R) * 0.5 * fx_verb;
                    double rev = 0;
                    for (int k = 0; k < NCB; k++) {
                        float d = cb[k][cp[k]];
                        cb[k][cp[k]] = (float)(mono + d * 0.84);
                        if (++cp[k] >= cl[k]) cp[k] = 0;
                        rev += d;
                    }
                    rev *= 0.25;
                    for (int k = 0; k < 2; k++) {
                        float d = ap[k][app[k]];
                        ap[k][app[k]] = (float)(rev + d * 0.5);
                        if (++app[k] >= apl[k]) app[k] = 0;
                        rev = d - (float)rev * 0.5;
                    }
                    L += rev * 0.4;
                    R += rev * 0.4;
                }
            }
        }
        /* DC blocker + tanh soft-clip limiter (replaces hard clip) */
        {
            static double dc_l = 0, dc_r = 0, lp_l = 0, lp_r = 0;
            double x = L - dc_l + 0.995 * lp_l;
            dc_l = L; lp_l = x; L = x;
            x = R - dc_r + 0.995 * lp_r;
            dc_r = R; lp_r = x; R = x;
            L = tanh(1.2 * L) * 1.1995;
            R = tanh(1.2 * R) * 1.1995;
        }
        out[i*2] = (float)L;
        out[i*2+1] = (float)R;
        /* scope tap: lock-free ring for the viz pane (single writer) */
        scope_buf[scope_pos & (SCOPE_N - 1)] = (float)((L + R) * 0.5);
        scope_pos++;
        audio_time += 1.0/SR; // careful increments per sample globally
    }
    // We incremented audio_time frameCount times? Actually loop does per sample. But audio_time update above per sample yes.
    return paContinue;
}

void audio_init(void) {
    wt_init();
#ifdef __linux__
    snd_lib_error_set_handler(alsa_silent);
#endif
    midi_init();
    Pa_Initialize();
    memset(voices, 0, sizeof(voices));
    PaStreamParameters op;
    memset(&op,0,sizeof(op));
    op.device = Pa_GetDefaultOutputDevice();
    op.channelCount = 2;
    op.sampleFormat = paFloat32;
    op.suggestedLatency = 0.02;
    unsigned long frames = audio_block_frames;
    Pa_OpenStream(&stream, NULL, &op, SR, frames, paClipOff, pa_callback, NULL);
    Pa_StartStream(stream);
    playing = 1;
}
void audio_shutdown(void) {
    midi_shutdown();
    if (stream) { Pa_StopStream(stream); Pa_CloseStream(stream); stream=NULL; }
    Pa_Terminate();
}
void audio_toggle_play(int on) { playing = on ? 1 : 0; }
int audio_is_playing(void) { return playing; }
int audio_is_stream_active(void) {
    if (!stream) return 0;
    return Pa_IsStreamActive(stream) == 1 ? 1 : 0;
}

const char *audio_inst_name(int inst) {
    switch (inst) {
        case INST_SINE: return "sine";
        case INST_SAW: return "saw";
        case INST_SQUARE: return "square";
        case INST_TRI: return "tri";
        case INST_NOISE: return "noise";
        case INST_KICK: return "kick";
        case INST_SNARE: return "snare";
        case INST_HH: return "hh";
        case INST_CP: return "clap";
        case INST_TOM: return "tom";
        case INST_WT: return "wt";
        default: return "?";
    }
}

int audio_active_voices(void) {
    int n = 0;
    pthread_mutex_lock(&voice_lock);
    for (int i = 0; i < MAX_VOICES; i++)
        if (voices[i].active) n++;
    pthread_mutex_unlock(&voice_lock);
    return n;
}

void audio_voice_stats(int *active_out, int by_inst[10]) {
    int n = 0;
    int tmp[10] = {0};
    pthread_mutex_lock(&voice_lock);
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!voices[i].active) continue;
        n++;
        int k = voices[i].inst;
        if (k >= 0 && k < 10) tmp[k]++;
    }
    pthread_mutex_unlock(&voice_lock);
    if (active_out) *active_out = n;
    if (by_inst) memcpy(by_inst, tmp, sizeof(tmp));
}