#include "audio.h"
#include "audio_output.h"
#include <portaudio.h>
#include <math.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <limits.h>
#include "js_number.h"
#include "wavetable.h"
#include "sampler.h"
#include "midi.h"
#include "convolution.h"
#include "stretch.h"
#include "scheduler.h"
#include "prep_pool.h"
#include "prep_profile.h"
#ifdef __linux__
#include <alsa/asoundlib.h>
/* ALSA probing spams stderr, which shares the terminal with the Notcurses
   UI and corrupts arbitrary screen rows. Silence it; we log our own. */
static void alsa_silent(const char *file, int line, const char *func,
                         int err, const char *fmt, ...) {
    (void)file; (void)line; (void)func; (void)err; (void)fmt;
}
#endif

static _Atomic double bpm = 120.0;
static _Atomic double effect_bpm = 120.0;
static atomic_int playing;
static atomic_int reference_mode;
void audio_set_reference_mode(int on) { atomic_store(&reference_mode,on != 0); }
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
/* Per-orbit delay. <0 keeps Strudel's defaults: 3/16 of a cycle, feedback 0.5.
   Ducking drops that orbit on a trigger and releases over duck_atk seconds. */
#define ORBIT_DELAY 88200
static double orbit_dt[N_ORBITS];
static double orbit_fb[N_ORBITS];
static double duck_depth[N_ORBITS];
static double duck_atk[N_ORBITS];
static double duck_t[N_ORBITS];
static double audio_time = 0.0;
static struct {
    int enabled, hold;
    double initial, target, begin, minimum, end;
} web_duck[N_ORBITS];
/* Accepted timer callbacks outlive source voices and program replacements.
   Match the event queue's capacity; all access is under voice_lock. */
typedef struct {
    double time, source_time;
    unsigned long long serial;
    unsigned mask;
    DuckControl controls[N_ORBITS];
} PendingDuck;
#define MAX_PENDING_DUCKS 8192
static PendingDuck pending_ducks[MAX_PENDING_DUCKS];
static int pending_duck_count;
static unsigned long long pending_duck_serial;
typedef struct {
    double time, delay, feedback;
    unsigned long long serial;
    int orbit;
    unsigned fields;
} PendingDelay;
#define MAX_PENDING_DELAYS 8192
static PendingDelay pending_delays[MAX_PENDING_DELAYS];
static int pending_delay_count;
static unsigned long long pending_delay_serial;
static int web_delay_created[N_ORBITS];
static int orbits_ready;
static ConvState convolvers[N_ORBITS];
static _Atomic(const ConvKernel *) requested_ir[N_ORBITS];
static _Atomic unsigned long long ir_revision[N_ORBITS];
static unsigned long long rendered_ir_revision[N_ORBITS];
static const int room_fields[]={DSP_SIZE,DSP_ROOMFADE,DSP_ROOMLP,DSP_ROOMDIM};
static double room_parameters[N_ORBITS][6];
static int room_samples[N_ORBITS];
static unsigned room_defined[N_ORBITS];
static int room_created[N_ORBITS];
static pthread_mutex_t voice_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic unsigned prepared_rooms;
enum { ROOM_REQUESTS=32 };
typedef struct {
    PrepJob job;
    int orbit;
    unsigned generation;
    double parameters[4];
} RoomJob;
static unsigned room_count;
static int room_worker_started,room_worker_busy,room_worker_stop;
static pthread_mutex_t room_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t room_wake=PTHREAD_COND_INITIALIZER;
static unsigned room_failures;
static _Atomic unsigned created_orbits;

unsigned audio_register_orbit(int orbit) {
    if (orbit<0 || orbit>=N_ORBITS) return atomic_load(&created_orbits);
    unsigned bit=1u<<orbit;
    return atomic_fetch_or(&created_orbits,bit)|bit;
}

/* Writers hold voice_lock. The callback takes one bounded atomic snapshot;
   an interrupted publication keeps the previous convolver for this frame. */
static void publish_room(int orbit, const ConvKernel *kernel, int rebuild) {
    if (!rebuild && atomic_load(&requested_ir[orbit])==kernel) return;
    atomic_fetch_add(&ir_revision[orbit],1);
    atomic_store(&requested_ir[orbit],kernel);
    atomic_fetch_add(&ir_revision[orbit],1);
}

static void room_job_run(PrepJob *base) {
    RoomJob *job=(RoomJob *)base;
    pthread_mutex_lock(&room_lock);
    if (room_count) room_count--;
    room_worker_busy=1;
    pthread_cond_broadcast(&room_wake);
    pthread_mutex_unlock(&room_lock);
    const double *p=job->parameters;
    const ConvKernel *kernel=convolution_prepare_reverb(p[0],p[1],p[2],p[3]);
    unsigned generation=event_queue_generation_guard_begin();
    int current=!job->generation || job->generation==generation;
    pthread_mutex_lock(&voice_lock);
    if (kernel && current) publish_room(job->orbit,kernel,1);
    else if (!kernel && current && room_failures<UINT_MAX) room_failures++;
    pthread_mutex_unlock(&voice_lock);
    event_queue_generation_guard_end();
    pthread_mutex_lock(&room_lock);
    room_worker_busy=0;
    pthread_cond_broadcast(&room_wake);
    pthread_mutex_unlock(&room_lock);
}

static void room_job_destroy(PrepJob *base) {
    free(base);
}

/* Start the shared pool on demand. The serial lane keeps generated rooms in
   request order without extending a callback-shared voice_lock window. */
static int start_room_worker(void) {
    if (!prep_pool_start(0,0)) return 0;
    pthread_mutex_lock(&room_lock);
    room_worker_stop=0;
    room_worker_started=1;
    pthread_mutex_unlock(&room_lock);
    return 1;
}

/* Producer only, with voice_lock held. The pool never holds room_lock while
   acquiring voice_lock. Requests own scalar parameters and process-lived IRs. */
static int request_room(const AudioEvent *event) {
    pthread_mutex_lock(&room_lock);
    if (!room_worker_started || room_count>=ROOM_REQUESTS || room_worker_stop) {
        pthread_mutex_unlock(&room_lock); return 0;
    }
    RoomJob *job=calloc(1,sizeof(*job));
    if (!job) { pthread_mutex_unlock(&room_lock); return 0; }
    prep_job_init(&job->job,room_job_run,room_job_destroy);
    job->orbit=event->orbit;
    job->generation=event->generation;
    for (int i=0;i<4;i++) job->parameters[i]=event->dsp.p[room_fields[i]];
    room_count++;
    pthread_mutex_unlock(&room_lock);
    if (!prep_pool_submit_serial(&job->job)) {
        pthread_mutex_lock(&room_lock);
        if (room_count) room_count--;
        pthread_cond_broadcast(&room_wake);
        pthread_mutex_unlock(&room_lock);
        prep_job_release(&job->job);
        return 0;
    }
    prep_job_release(&job->job);
    return 1;
}

/* Offline tools only. Live scheduling and rendering never wait for IR work. */
void audio_wait_room_preparation(void) {
    pthread_mutex_lock(&room_lock);
    while (room_count || room_worker_busy) pthread_cond_wait(&room_wake,&room_lock);
    pthread_mutex_unlock(&room_lock);
}

static void stop_room_worker(void) {
    pthread_mutex_lock(&room_lock);
    room_worker_stop=1;
    pthread_mutex_unlock(&room_lock);
    audio_wait_room_preparation();
    pthread_mutex_lock(&room_lock);
    room_worker_started=0; room_worker_stop=0;
    room_count=0; room_worker_busy=0;
    pthread_mutex_unlock(&room_lock);
}

static void orbits_ensure(void) {
    if (orbits_ready) return;
    for (int i = 0; i < N_ORBITS; i++) {
        orbit_dt[i] = -1.0;
        orbit_fb[i] = -1.0;
    }
    orbits_ready = 1;
}

static double orbit_duck_gain_at(int orbit, double time) {
    if (orbit < 0 || orbit >= N_ORBITS) return 1;
    if (web_duck[orbit].enabled) {
        double begin=web_duck[orbit].begin,minimum=web_duck[orbit].minimum,end=web_duck[orbit].end;
        double initial=web_duck[orbit].initial,target=web_duck[orbit].target;
        if (time<begin) return initial;
        if (time<minimum && minimum>begin)
            return initial*pow(target/initial,(time-begin)/(minimum-begin));
        if (web_duck[orbit].hold) return target;
        if (time<end && end>minimum)
            return target*pow(1/target,(time-minimum)/(end-minimum));
        return 1;
    }
    if (duck_depth[orbit] <= 0 || duck_atk[orbit] <= 0 || duck_t[orbit] >= duck_atk[orbit])
        return 1;
    return 1 - duck_depth[orbit] * (1 - duck_t[orbit] / duck_atk[orbit]);
}

static double orbit_duck_gain(int orbit) { return orbit_duck_gain_at(orbit,audio_time); }

void audio_fx_set(const char *name, double v) {
    if (!name || !isfinite(v)) return;
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    if (strcmp(name, "drive") == 0) fx_drive = v;
    else if (strcmp(name, "cutoff") == 0) fx_cutoff = v;
    else if (strcmp(name, "delay") == 0) fx_delay = v;
    else if (strcmp(name, "verb") == 0) fx_verb = v;
}
static double silent_clock_origin;
static int silent_clock;
/* Current sources and bounded retiring graphs have separate physical slots.
   Moving a Voice transfers its prepared references; the callback never frees. */
enum { VOICE_SLOTS = MAX_VOICES*2 };
static Voice voices[VOICE_SLOTS];
static unsigned retirement_overflow;
/* Producers already hold voice_lock. Only the scheduler formats/logs errors;
   the callback writes bounded records, including when playback is stopped. */
enum { DUCK_ERROR_CAPACITY = 64 };
typedef struct { int orbit, returning, timer; double time; } DuckError;
static DuckError duck_errors[DUCK_ERROR_CAPACITY];
static unsigned duck_error_count;
static int duck_error_overflow;

static void record_duck_error(int orbit, int stage) {
    if (duck_error_count < DUCK_ERROR_CAPACITY)
        duck_errors[duck_error_count++]=(DuckError){.orbit=orbit,.returning=stage==1,.timer=stage==2,.time=audio_time};
    else duck_error_overflow=1;
}

void audio_report_errors(void) {
    DuckError errors[DUCK_ERROR_CAPACITY];
    pthread_mutex_lock(&voice_lock);
    unsigned count=duck_error_count;
    int overflow=duck_error_overflow;
    unsigned lost_tails=retirement_overflow; retirement_overflow=0;
    unsigned failed_rooms=room_failures; room_failures=0;
    memcpy(errors,duck_errors,count*sizeof(*errors));
    duck_error_count=0; duck_error_overflow=0;
    pthread_mutex_unlock(&voice_lock);
    for (unsigned i=0;i<count;i++) {
        if (errors[i].timer)
            log_console("duck: orbit %d rejected timer creation at %.6f s (negative/non-finite stop time)",errors[i].orbit,errors[i].time);
        else log_console("duck: orbit %d rejected %s ramp at %.6f s",errors[i].orbit,
                         errors[i].returning?"return":"downward",errors[i].time);
    }
    if (overflow) log_console("duck: additional automation errors omitted");
    if (lost_tails) log_console("voices: retirement capacity exceeded; %u fading graphs replaced early",lost_tails);
    if (failed_rooms) log_console("room: %u IR preparations failed or exceeded queue/cache limits; previous room retained",failed_rooms);
}

int audio_prepare_duck_timer(int orbit,double time) {
    if (isfinite(time) && time-.01>=0) return 1;
    pthread_mutex_lock(&voice_lock);
    if (silent_clock) audio_time=audio_time_now();
    record_duck_error(orbit,2);
    pthread_mutex_unlock(&voice_lock);
    return 0;
}
static CsoundProgram *csound_program;
static double next_voice_onset=HUGE_VAL;
static unsigned long long voice_onset_serial;

void audio_set_csound(CsoundProgram *program) {
    csound_program_retain(program);
    pthread_mutex_lock(&voice_lock);
    CsoundProgram *old = csound_program;
    if (old==program) {
        pthread_mutex_unlock(&voice_lock);
        csound_program_free(program);
        return;
    }
    csound_program = program;
    csound_program_activate(program,audio_time);
    pthread_mutex_unlock(&voice_lock);
    csound_program_free(old);
}

/* scope ring for the viz pane: single writer (callback), lock-free reader */
#define SCOPE_N 4096
static float scope_buf[SCOPE_N];
static unsigned scope_pos = 0;

/* Fixed lock-free snapshots. Generation tags prevent a re-evaluated call site
   from displaying samples left behind by the previous program. */
_Static_assert(ATOMIC_LLONG_LOCK_FREE == 2,"pattern scopes require lock-free 64-bit atomics");
static atomic_ullong pattern_scope[N_PATTERN_SCOPES][SCOPE_N];
static atomic_uint pattern_scope_pos;
static unsigned pattern_scope_generation[N_PATTERN_SCOPES]; /* voice_lock */
static double pattern_scope_frame[N_PATTERN_SCOPES]; /* callback only */

int audio_pattern_scope_copy(unsigned mask, unsigned generation, float *out, int n) {
    if (!out || n <= 0) return 0;
    if (n > SCOPE_N) n = SCOPE_N;
    int feeds[N_PATTERN_SCOPES], count = 0;
    for (int i = 0; mask; i++, mask >>= 1) if (mask&1u) feeds[count++] = i;
    unsigned position = atomic_load_explicit(&pattern_scope_pos,memory_order_acquire);
    for (int j = 0; j < n; j++) {
        double value = 0;
        unsigned at = (position-(unsigned)n+(unsigned)j)&(SCOPE_N-1);
        if (position >= (unsigned)(n-j)) for (int i = 0; i < count; i++) {
            unsigned long long packed = atomic_load_explicit(&pattern_scope[feeds[i]][at],memory_order_relaxed);
            if (generation && (unsigned)(packed>>32) == generation) {
                unsigned bits = (unsigned)packed; float sample;
                memcpy(&sample,&bits,sizeof(sample)); value += sample;
            }
        }
        out[j] = (float)value;
    }
    return n;
}

static void pattern_scope_publish(void) {
    unsigned position = atomic_load_explicit(&pattern_scope_pos,memory_order_relaxed);
    for (int i = 0; i < N_PATTERN_SCOPES; i++) {
        float sample = (float)pattern_scope_frame[i]; unsigned bits;
        memcpy(&bits,&sample,sizeof(bits));
        unsigned long long packed = ((unsigned long long)pattern_scope_generation[i]<<32)|bits;
        atomic_store_explicit(&pattern_scope[i][position&(SCOPE_N-1)],packed,memory_order_relaxed);
    }
    atomic_store_explicit(&pattern_scope_pos,position+1,memory_order_release);
}

int audio_scope_copy(float *out, int n) {
    if (!out || n <= 0) return 0;
    if (n > SCOPE_N) n = SCOPE_N;
    unsigned p = scope_pos; /* torn read = 1-sample glitch at most */
    unsigned start = p > (unsigned)n ? p - (unsigned)n : 0;
    for (int i = 0; i < n; i++)
        out[i] = scope_buf[(start + (unsigned)i) & (SCOPE_N - 1)];
    return n;
}


double audio_time_now(void){ return silent_clock?now_seconds()-silent_clock_origin:audio_time; }
void audio_set_bpm(double b){
    if (!isfinite(b) || b<0 || (b>0 && (!isfinite(240/b) || 240/b<=0))) return;
    bpm=b;
    if (b>0) effect_bpm=b;
}
double audio_get_bpm(void){ return bpm; }
double audio_sec_per_beat(void){ double b=bpm; return b>0?60.0/b:HUGE_VAL; }
double audio_sec_per_cycle(void){ double b=bpm; return b>0?240.0/b:HUGE_VAL; } // 4 beats per cycle
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

/* Printable ASCII only, so a hap token cannot break the snapshot line. */
static void dbg_token(char *dst, size_t n, const char *src) {
    size_t w = 0;
    if (!dst || n == 0) return;
    if (src) {
        for (size_t i = 0; src[i] && w + 1 < n; i++) {
            unsigned char c = (unsigned char)src[i];
            if (c <= 32 || c >= 127) c = '_';
            dst[w++] = (char)c;
        }
    }
    if (w == 0) dst[w++] = '-';
    dst[w] = 0;
}

/* Called under voice_lock at the duck timer or source onset respectively.
   No allocation, decoding, JavaScript or resource destruction occurs here. */
static double duck_endpoint_number(const char *text) {
    return js_number_parse(text);
}

static void apply_duck(const PendingDuck *pending) {
    for (int o=0;o<N_ORBITS;o++) if (pending->mask&(1u<<o)) {
        DuckControl control=pending->controls[o];
        /* AudioParam.value is the last rendered sample when the timer fires,
           not the next frame's value. This also anchors failed replacement
           ramps after cancellation at the gain the artist actually heard. */
        double initial=fmax(.01,(float)orbit_duck_gain_at(o,fmax(0,audio_time-1/SR)));
        double minimum=fmax(pending->source_time,audio_time)+control.onset;
        double return_time=minimum+fmax(.002,control.attack);
        if (control.string_onset==DUCK_ONSET_SUMMARY) {
            duck_onset_times(control.long_onset,fmax(pending->source_time,audio_time),
                fmax(.002,control.attack),&minimum,&return_time);
        } else if (control.string_onset) {
            char text[224],attack[32]; JSDTOATempMem temporary;
            int length=js_dtoa(text,fmax(pending->source_time,audio_time),10,0,JS_DTOA_FORMAT_FREE,&temporary);
            size_t suffix=strlen(control.onset_text);
            memcpy(text+length,control.onset_text,suffix+1);
            minimum=duck_endpoint_number(text);
            int attack_length=js_dtoa(attack,fmax(.002,control.attack),10,0,JS_DTOA_FORMAT_FREE,&temporary);
            memcpy(text+length+suffix,attack,(size_t)attack_length+1);
            return_time=duck_endpoint_number(text);
        }
        web_duck[o].enabled=1;
        web_duck[o].hold=0;
        web_duck[o].initial=fmax(.01,initial);
        web_duck[o].target=fmin(initial,fmax(.01,1-sqrt(control.depth)));
        web_duck[o].begin=audio_time;
        web_duck[o].minimum=fmax(audio_time,minimum);
        web_duck[o].end=fmax(audio_time,return_time);
        /* WebAudio validates each ramp only after cancel/anchor. A failed
           first ramp holds the anchor; a failed return retains the first
           ramp and holds its target. Never propagate NaN into audio PCM. */
        if (isnan(control.depth) || control.depth<0 || !isfinite(minimum) || minimum<0) {
            record_duck_error(o,0);
            web_duck[o].target=initial;
            web_duck[o].minimum=audio_time;
            web_duck[o].end=audio_time;
            web_duck[o].hold=1;
        } else if (!isfinite(control.attack) || !isfinite(return_time) || return_time<0) {
            record_duck_error(o,1);
            web_duck[o].end=web_duck[o].minimum;
            web_duck[o].hold=1;
        }
        duck_depth[o]=control.depth; duck_atk[o]=control.attack; duck_t[o]=0;
    }
}

static int duck_before(const PendingDuck *a, const PendingDuck *b) {
    return a->time<b->time || (a->time==b->time && a->serial<b->serial);
}

static void dispatch_ducks_locked(void) {
    while (pending_duck_count && pending_ducks[0].time<=audio_time+1e-10) {
        PendingDuck due=pending_ducks[0],last=pending_ducks[--pending_duck_count];
        int at=0;
        while (at*2+1<pending_duck_count) {
            int child=at*2+1;
            if (child+1<pending_duck_count && duck_before(&pending_ducks[child+1],&pending_ducks[child])) child++;
            if (!duck_before(&pending_ducks[child],&last)) break;
            pending_ducks[at]=pending_ducks[child]; at=child;
        }
        if (pending_duck_count) pending_ducks[at]=last;
        apply_duck(&due);
    }
}

static int schedule_duck_locked(const AudioEvent *event) {
    if (!event->dsp.enabled || !event->duck_mask) return 1;
    dispatch_ducks_locked();
    if (pending_duck_count==MAX_PENDING_DUCKS) return 0;
    PendingDuck pending={.time=fmax(audio_time,event->time-.01),.source_time=event->time,
        .serial=++pending_duck_serial,.mask=event->duck_mask};
    for (int o=0;o<N_ORBITS;o++) if (event->duck_mask&(1u<<o))
        pending.controls[o]=event->duck_values?event->ducks[o]:
            (DuckControl){.onset=event->duck_onset,.attack=event->duck_attack,.depth=event->duck_depth};
    int at=pending_duck_count++;
    while (at>0) {
        int parent=(at-1)/2;
        if (!duck_before(&pending,&pending_ducks[parent])) break;
        pending_ducks[at]=pending_ducks[parent]; at=parent;
    }
    pending_ducks[at]=pending;
    dispatch_ducks_locked();
    return 1;
}

static int delay_before(const PendingDelay *a, const PendingDelay *b) {
    return a->time<b->time || (a->time==b->time && a->serial<b->serial);
}

static void dispatch_delays_locked(void) {
    while (pending_delay_count && pending_delays[0].time<=audio_time+1e-10) {
        PendingDelay due=pending_delays[0],last=pending_delays[--pending_delay_count];
        int at=0;
        while (at*2+1<pending_delay_count) {
            int child=at*2+1;
            if (child+1<pending_delay_count && delay_before(&pending_delays[child+1],&pending_delays[child])) child++;
            if (!delay_before(&pending_delays[child],&last)) break;
            pending_delays[at]=pending_delays[child]; at=child;
        }
        if (pending_delay_count) pending_delays[at]=last;
        if (due.fields&1) orbit_dt[due.orbit]=due.delay;
        if (due.fields&2) orbit_fb[due.orbit]=due.feedback;
    }
}

int audio_prepare_delay(AudioEvent *event) {
    if (!event) return 0;
    if (event->delay_prepared) return 1;
    if (!event->dsp.enabled || event->delay_send<=0 || event->delay_time<=0 || event->delay_fb<=0) {
        event->delay_prepared=1; return 1;
    }
    if (!isfinite(event->time) || !isfinite(event->delay_time) || !isfinite(event->delay_fb)) return 0;
    int orbit=event->orbit;
    if (orbit<0 || orbit>=N_ORBITS) return 0;
    double delay=(float)fmin(1,event->delay_time),feedback=(float)fmin(.98,event->delay_fb);
    pthread_mutex_lock(&voice_lock);
    if (silent_clock) audio_time=audio_time_now();
    orbits_ensure(); dispatch_delays_locked();
    double current_delay=web_delay_created[orbit]?orbit_dt[orbit]:delay;
    double current_feedback=web_delay_created[orbit]?orbit_fb[orbit]:feedback;
    unsigned fields=(current_delay!=event->delay_time?1u:0u)|(current_feedback!=event->delay_fb?2u:0u);
    if (fields && pending_delay_count==MAX_PENDING_DELAYS) {
        pthread_mutex_unlock(&voice_lock); return 0;
    }
    if (!web_delay_created[orbit]) {
        orbit_dt[orbit]=delay; orbit_fb[orbit]=feedback; web_delay_created[orbit]=1;
    }
    if (fields) {
        PendingDelay pending={.time=fmax(audio_time,event->time),.delay=delay,.feedback=feedback,
            .serial=++pending_delay_serial,.orbit=orbit,.fields=fields};
        int at=pending_delay_count++;
        while (at>0) {
            int parent=(at-1)/2;
            if (!delay_before(&pending,&pending_delays[parent])) break;
            pending_delays[at]=pending_delays[parent]; at=parent;
        }
        pending_delays[at]=pending;
    }
    dispatch_delays_locked();
    event->delay_prepared=1;
    pthread_mutex_unlock(&voice_lock);
    return 1;
}

void audio_inherit_room(AudioEvent *event, unsigned explicit_fields) {
    if (!event || !event->dsp.enabled || event->dsp.p[DSP_ROOM]<=0 ||
        event->orbit<0 || event->orbit>=N_ORBITS) return;
    pthread_mutex_lock(&voice_lock);
    event->room_defined=explicit_fields&48u;
    int regenerate=room_created[event->orbit] && room_samples[event->orbit]!=event->room_sample;
    if (room_created[event->orbit]) for (int i=0;i<4;i++)
        if ((explicit_fields&(1u<<i)) &&
            event->dsp.p[room_fields[i]]!=room_parameters[event->orbit][i]) regenerate=1;
    if (room_created[event->orbit] &&
        (((explicit_fields&16) && (!(room_defined[event->orbit]&16) || event->room_speed!=room_parameters[event->orbit][4])) ||
         ((explicit_fields&32) && (!(room_defined[event->orbit]&32) || event->room_begin!=room_parameters[event->orbit][5])))) regenerate=1;
    /* The web's equality guards ignore undefined arguments. Once any
       explicit argument changes, generate() receives the original arguments
       and its defaults apply to all omitted controls. */
    if (room_created[event->orbit] && !regenerate) for (int i=0;i<4;i++)
        if (!(explicit_fields&(1u<<i)))
            event->dsp.p[room_fields[i]]=room_parameters[event->orbit][i];
    if (room_created[event->orbit] && !regenerate) {
        event->room_defined=room_defined[event->orbit];
        if (!(explicit_fields&16)) event->room_speed=room_parameters[event->orbit][4];
        if (!(explicit_fields&32)) event->room_begin=room_parameters[event->orbit][5];
    }
    event->room_rebuild=!room_created[event->orbit] || regenerate;
    pthread_mutex_unlock(&voice_lock);
}

void audio_prepare_room(AudioEvent *event) {
    if (!event || event->room_prepared) return;
    if (event->dsp.enabled && event->dsp.p[DSP_ROOM]>0 &&
        event->orbit>=0 && event->orbit<N_ORBITS) {
        int generated=event->room_params_valid && event->room_sample<0 && !event->ir_kernel;
        int ready=!generated || !event->room_rebuild || start_room_worker();
        pthread_mutex_lock(&voice_lock);
        if (event->room_params_valid) {
            for (int i=0;i<4;i++) room_parameters[event->orbit][i]=event->dsp.p[room_fields[i]];
            room_parameters[event->orbit][4]=event->room_speed;
            room_parameters[event->orbit][5]=event->room_begin;
            room_samples[event->orbit]=event->room_sample;
            room_defined[event->orbit]=event->room_defined;
            room_created[event->orbit]=1;
        }
        atomic_fetch_or(&prepared_rooms,1u<<event->orbit);
        if (generated) {
            if (event->room_rebuild && (!ready || !request_room(event)) && room_failures<UINT_MAX) room_failures++;
        } else publish_room(event->orbit,event->ir_kernel,event->room_rebuild);
        pthread_mutex_unlock(&voice_lock);
    }
    event->room_prepared=1;
}

static void voice_onset(Voice *v) {
    v->onset_pending=0;
    /* BufferSource starts on the next render frame but retains the fractional
       source position and envelope time. Apply this once, after preparation;
       stretched PCM already includes its own alignment to the playback grid. */
    if (v->dsp.control.enabled && v->smpbuf && !v->stretch_buffer) {
        double elapsed=fmax(0,audio_time-v->start_time);
        v->age=elapsed;
        v->smppos+=elapsed*SR*v->smpstep*
            dsp_pitch_multiplier(&v->dsp.control,0,v->gate_end-v->start_time);
    }
    for (int i=0;i<N_PATTERN_SCOPES;i++) if (v->scope_mask&(1u<<i))
        pattern_scope_generation[i]=v->scope_generation;
    if (v->onset_room) publish_room(v->orbit,v->onset_ir,0);
    if (!v->dsp.control.enabled && v->delay_send>0) {
        if (v->onset_delay_time>=0) orbit_dt[v->orbit]=v->onset_delay_time;
        if (v->onset_delay_feedback>=0) orbit_fb[v->orbit]=v->onset_delay_feedback;
    }
    if (!v->dsp.control.enabled && v->onset_duck_mask && v->onset_duck_depth>0) {
        for (int o=0;o<N_ORBITS;o++) if (v->onset_duck_mask&(1u<<o)) {
            duck_depth[o]=fmin(1,v->onset_duck_depth);
            web_duck[o].enabled=0;
            duck_atk[o]=fmax(0,v->onset_duck_attack); duck_t[o]=0;
        }
    }
}

/* Web sample cut schedules gain automation when the source is prepared.
   Its future unity set also overrides an earlier pool-retirement ramp. */
static void prepare_source_cut(Voice *v) {
    double cut=v->dsp.control.enabled && v->sampled_source?v->dsp.control.p[DSP_CUT]:0;
    if (cut>0) for (int i=0;i<VOICE_SLOTS;i++) {
        Voice *old=&voices[i];
        if (old!=v && old->active && old->sampled_source && old->dsp.control.enabled && old->dsp.control.p[DSP_CUT]==cut &&
            (old->start_time<v->start_time ||
             (old->start_time==v->start_time && old->onset_serial<v->onset_serial)) && !old->cut_pending) {
            old->cut_pending=1;
            old->cut_time=v->start_time;
        }
    }
}

static void dispatch_voice_onsets(void) {
    dispatch_delays_locked();
    dispatch_ducks_locked();
    if (next_voice_onset>audio_time+1e-10) return;
    int due[VOICE_SLOTS], count=0;
    next_voice_onset=HUGE_VAL;
    for (int i=0;i<VOICE_SLOTS;i++) {
        Voice *v=&voices[i];
        if (!v->active || !v->onset_pending) continue;
        if (v->start_time>audio_time+1e-10) { next_voice_onset=fmin(next_voice_onset,v->start_time); continue; }
        int at=count++;
        while (at>0) {
            Voice *previous=&voices[due[at-1]];
            if (previous->start_time<v->start_time ||
                (previous->start_time==v->start_time && previous->onset_serial<v->onset_serial)) break;
            due[at]=due[at-1]; at--;
        }
        due[at]=i;
    }
    for (int i=0;i<count;i++) if (voices[due[i]].active) voice_onset(&voices[due[i]]);
}

static void trigger_voice(Voice *v, AudioEvent *ev) {
    memset(v, 0, sizeof(Voice));
    v->scope_mask = ev->scope_mask; v->scope_generation = ev->generation;
    dbg_token(v->token, sizeof(v->token), ev->token);
    if (ev->dsp.enabled) dsp_init(&v->dsp, &ev->dsp);
    else v->dsp.control.enabled=v->dsp_right.control.enabled=0;
    if (ev->dsp.enabled && (ev->inst == INST_SUPERSAW || ev->inst == INST_NATIVE || ev->stretch_buffer)) dsp_init(&v->dsp_right,&ev->dsp);
    v->dsp.noise_seed ^= (unsigned)fmod(audio_time*SR,4294967295.0) + (unsigned)(v-voices)*97u;
    v->active = 1;
    v->native_program=ev->native_synth;
    v->inst = ev->is_drum ? ev->inst : ev->inst;
    // if drum token, map
    if (ev->is_drum) {
        /* hh:1 is the same drum as hh; the number picks a sample. */
        char base[64];
        const char *tok = ev->token;
        const char *col = strchr(ev->token, ':');
        if (col && col > ev->token && (size_t)(col - ev->token) < sizeof(base)) {
            memcpy(base, ev->token, (size_t)(col - ev->token));
            base[col - ev->token] = 0;
            tok = base;
        }
        if (strcmp(tok, "bd")==0) v->inst = INST_KICK;
        else if (strcmp(tok, "sd")==0 || strcmp(tok, "sn")==0) v->inst = INST_SNARE;
        else if (strcmp(tok, "hh")==0 || strcmp(tok, "hat")==0) v->inst = INST_HH;
        else if (strcmp(tok, "cp")==0 || strcmp(tok, "clap")==0) v->inst = INST_CP;
        else if (strcmp(tok, "tom")==0) v->inst = INST_TOM;
        else v->inst = INST_HH;
    }
    v->freq = ev->freq > 0 ? ev->freq : 440.0;
    v->amp = ev->dsp.enabled ? ev->gain : (ev->gain > 0 ? ev->gain : 0.8);
    v->pan = ev->pan;
    v->has_pan=ev->has_pan;
    v->waveform_gain=ev->waveform_gain>0?ev->waveform_gain:1;
    v->start_time = fmax(audio_time,ev->time);
    v->note_off_time = v->start_time + (ev->duration > 0 ? ev->duration : 0.25);
    v->gate_end = v->note_off_time;
    v->phase = 0;
    v->age = 0;
    v->env = 0;
    v->released = 0;
    v->vatt = ev->att;
    v->vrel = ev->rel;
    v->graph_end=v->note_off_time+fmax(0,v->vrel);
    v->vdec = ev->dec;
    v->vsus = ev->sus;
    v->pw = ev->pw;
    v->lpf_hz = ev->dsp.enabled ? 0 : ev->lpf;
    v->lpf_z = 0;
    v->lpq = ev->lpq;
    v->lpenv = ev->lpenv;
    v->has_lpenv = ev->has_lpenv;
    v->lpa = ev->lpa;
    v->smp_end = (ev->smp_end > 0.0 && ev->smp_end <= 1.0) ? ev->smp_end : 1.0;
    if (ev->dsp.enabled) {
        v->smp_end = ev->dsp.p[DSP_END];
        v->smp_begin = ev->dsp.p[DSP_BEGIN];
    }
    v->delay_send = ev->delay_send;
    v->orbit = ev->orbit;
    if (v->orbit < 0 || v->orbit >= N_ORBITS) v->orbit = 0;
    orbits_ensure();
    v->onset_serial=++voice_onset_serial;
    v->sampled_source=ev->smp>=0 || ev->stretch_buffer!=NULL;
    v->onset_pending=1;
    v->onset_room=!ev->room_prepared && ev->dsp.enabled && ev->dsp.p[DSP_ROOM]>0;
    v->onset_ir=ev->ir_kernel;
    v->onset_delay_time=ev->delay_time; v->onset_delay_feedback=ev->delay_fb;
    v->onset_duck_mask=ev->duck_mask; v->onset_duck_depth=ev->duck_depth; v->onset_duck_attack=ev->duck_attack;
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
    v->smp_channels = 0;
    v->smplen = 0;
    v->smppos = 0;
    v->smpstep = 1.0;
    v->smp_gate = ev->smp_gate;
    v->stretch_factor = ev->stretch > 0 ? ev->stretch : 1;
    v->stretch_buffer=ev->stretch_buffer;
    stretch_retain(v->stretch_buffer);
    if (v->stretch_buffer) v->age=stretch_age_offset(v->stretch_buffer);
    if (ev->smp >= 0) {
        smp_get_stereo(ev->smp, &v->smpbuf, &v->smplen,&v->smp_channels);
        if (ev->dsp.enabled && v->smp_channels == 2)
            dsp_init(&v->dsp_right, &ev->dsp);
        if (v->smpbuf && v->smplen > 0 && ev->freq > 0) {
            double base = midi_to_freq(smp_base_midi(ev->smp));
            if (base > 0) v->smpstep = ev->freq / base;
        }
        /* 0 is natural speed. Negative speed walks the buffer backwards. */
        double sp = ev->dsp.enabled ? ev->smp_speed : ((ev->smp_speed != 0.0) ? ev->smp_speed : 1.0);
        v->smpstep *= sp;
        double mag = fabs(v->smpstep);
        if (ev->dsp.enabled) {
            if (mag > 0 && mag < .001) mag = .001;
        } else if (mag < 0.05) mag = 0.05;
        if (mag > (ev->dsp.enabled ? 4096 : 8)) mag = ev->dsp.enabled ? 4096 : 8;
        v->smpstep = (v->smpstep < 0.0) ? -mag : mag;
        if (ev->dsp.enabled && v->smpstep < 0) {
            double begin = v->smp_begin;
            v->smp_begin = 1-v->smp_end; v->smp_end = 1-begin;
            double loop_begin = v->dsp.control.p[DSP_LOOPBEGIN];
            v->dsp.control.p[DSP_LOOPBEGIN] = 1-v->dsp.control.p[DSP_LOOPEND];
            v->dsp.control.p[DSP_LOOPEND] = 1-loop_begin;
        }
        if (v->smpstep < 0.0 && v->smplen > 1)
            v->smppos = fmax(0,(ev->dsp.enabled ? v->smplen*v->smp_end : ceil(v->smplen*v->smp_end))-1);
        else v->smppos = v->smplen*v->smp_begin;
        if (v->smp_end <= v->smp_begin) v->active = 0;
        if (ev->dsp.enabled && !v->smp_gate && mag > 0) {
            v->gate_end = v->start_time+v->smplen*(v->smp_end-v->smp_begin)/(mag*SR);
            v->note_off_time = v->gate_end;
        }
    }
    if (v->start_time<=audio_time+1e-10) voice_onset(v);
    else next_voice_onset=fmin(next_voice_onset,v->start_time);
}

/* WebAudio exponentialRampToValueAtTime: equal ratio per unit time. */
static double exp_seg(double a, double b, double u) {
    if (u <= 0.0) return a;
    if (u >= 1.0) return b;
    if (a < 1.0) a = 1.0;
    if (b < 1.0) b = 1.0;
    return a * pow(b / a, u);
}

/* Value of the filter envelope at `age` seconds, before release.
   start is the frequency at the beginning of the attack, peak is the
   frequency at the end of it. The segment targets match createFilter;
   the path between them is exponential. */
static double filter_adsr(double age, double start, double peak,
                          double attack, double decay, double sus) {
    if (age < attack) {
        double u = attack > 0.0 ? age / attack : 1.0;
        return exp_seg(start, peak, u);
    }
    if (sus >= 1.0) return peak;
    double sustain_fc = start + sus * (peak - start);
    if (age < attack + decay) {
        double dlt = decay > 0.0 ? (age - attack) / decay : 1.0;
        return exp_seg(peak, sustain_fc, dlt);
    }
    return sustain_fc;
}

/* Strudel createFilter, anchor 0: the two ends are `fc` and
   `fc * 2^|lpenv|`. A negative lpenv starts at the high end, so
   lpenv(-4) at 800 Hz falls from 12800 Hz to 800 Hz. Setting lpa
   with no filter decay holds that end (sustain 1). Otherwise the
   default envelope is 5 ms attack, 140 ms decay, sustain 0. */
static double voice_fc(const Voice *V, double fc) {
    if (fc <= 0.0) return 0.0;
    if (V->has_lpenv) {
        double env_abs = fabs(V->lpenv);
        double start = fc;
        double peak = fc * pow(2.0, env_abs);
        double attack, decay, sus, rel, held, at;
        if (V->lpenv < 0.0) {
            double swap = start;
            start = peak;
            peak = swap;
        }
        if (V->lpa > 0.0) {
            attack = V->lpa;
            decay = 0.001;
            sus = 1.0;
            rel = 0.01;
        } else {
            attack = 0.005;
            decay = 0.14;
            sus = 0.0;
            rel = 0.1;
        }
        if (attack < 0.001) attack = 0.001;
        if (V->released) {
            at = V->release_time - V->start_time;
            if (at < 0.0) at = 0.0;
            held = filter_adsr(at, start, peak, attack, decay, sus);
            if (rel < 0.005) rel = 0.005;
            double u = (audio_time - V->release_time) / rel;
            if (u < 0.0) u = 0.0;
            if (u > 1.0) u = 1.0;
            fc = exp_seg(held, start, u);
        } else {
            fc = filter_adsr(V->age, start, peak, attack, decay, sus);
        }
    }
    if (fc < 20.0) fc = 20.0;
    if (fc > 18000.0) fc = 18000.0;
    return fc;
}

static double apply_lpf(Voice *V, double x, double fc) {
    if (fc <= 0.0) return x;
    if (V->lpq > 0.05) {
        double Q = V->lpq;
        if (Q > 24.0) Q = 24.0;
        double w0 = 2.0 * M_PI * fc / SR;
        double cw = cos(w0);
        double sw = sin(w0);
        double alpha = sw / (2.0 * Q);
        double a0 = 1.0 + alpha;
        double b0 = ((1.0 - cw) * 0.5) / a0;
        double b1 = (1.0 - cw) / a0;
        double b2 = b0;
        double a1 = (-2.0 * cw) / a0;
        double a2 = (1.0 - alpha) / a0;
        double y = b0 * x + b1 * V->bx1 + b2 * V->bx2 - a1 * V->by1 - a2 * V->by2;
        if (!isfinite(y)) y = 0.0;
        if (y > 1.5) y = 1.5;
        if (y < -1.5) y = -1.5;
        V->bx2 = V->bx1;
        V->bx1 = x;
        V->by2 = V->by1;
        V->by1 = y;
        return y;
    }
    double coef = 1.0 - exp(-2.0 * M_PI * fc / SR);
    V->lpf_z += coef * (x - V->lpf_z);
    return V->lpf_z;
}

/* Recent triggers. Short hats die before a live-voice poll. */
#define DBG_RING 48
typedef struct {
    char token[24];
    int slot, inst, smp, channels, smplen, orbit, active, released;
    double t, freq, gain, age, gate, att, dec, sus, rel;
    double begin, end, pos, step;
    double cut, q, ft, fc, hcut, hq, hfc, band, bq, bfc, delay, shape;
} DbgRow;

static DbgRow dbg_ring[DBG_RING];
static int dbg_pos;
static int dbg_count;

static void dbg_from_voice(DbgRow *r, const Voice *v, int slot) {
    double gate = v->gate_end - v->start_time;
    memset(r, 0, sizeof(*r));
    memcpy(r->token, v->token, sizeof(r->token));
    r->token[sizeof(r->token) - 1] = 0;
    if (!r->token[0]) r->token[0] = '-';
    r->slot = slot;
    r->inst = v->inst;
    r->smp = v->smp;
    r->channels = v->smp_channels;
    r->smplen = v->smplen;
    r->orbit = v->orbit;
    r->active = v->active;
    r->released = v->released;
    r->t = v->start_time;
    r->freq = v->freq;
    r->gain = v->amp;
    r->age = v->age;
    r->gate = isfinite(gate) && gate > 0 ? gate : 0;
    r->att = v->vatt;
    r->dec = v->vdec;
    r->sus = v->vsus;
    r->rel = v->vrel;
    r->begin = v->smp_begin;
    r->end = v->smp_end;
    r->pos = v->smppos;
    r->step = v->smpstep;
    r->delay = v->delay_send;
    if (v->dsp.control.enabled) {
        const DspParams *p = &v->dsp.control;
        r->cut = p->p[DSP_LPF];
        r->q = p->p[DSP_LPQ];
        r->ft = p->p[DSP_FTYPE];
        r->fc = dsp_filter_frequency(p, 0, v->age, r->gate);
        r->hcut = p->p[DSP_HPF];
        r->hq = p->p[DSP_HPQ];
        r->hfc = dsp_filter_frequency(p, 1, v->age, r->gate);
        r->band = p->p[DSP_BPF];
        r->bq = p->p[DSP_BPQ];
        r->bfc = dsp_filter_frequency(p, 2, v->age, r->gate);
        r->shape = p->p[DSP_SHAPE];
    } else {
        r->cut = v->lpf_hz;
        r->q = v->lpq;
        r->ft = -1;
        r->fc = voice_fc(v, v->lpf_hz);
    }
}

static void dbg_push(const Voice *v, int slot) {
    dbg_from_voice(&dbg_ring[dbg_pos], v, slot);
    dbg_pos = (dbg_pos + 1) % DBG_RING;
    if (dbg_count < DBG_RING) dbg_count++;
}

static size_t dbg_add(char *out, size_t cap, size_t pos, const char *fmt, ...) {
    va_list ap;
    int n;
    if (!out || cap == 0) return 0;
    if (pos >= cap) return cap - 1;
    va_start(ap, fmt);
    n = vsnprintf(out + pos, cap - pos, fmt, ap);
    va_end(ap);
    if (n < 0) {
        out[pos] = 0;
        return pos;
    }
    if ((size_t)n >= cap - pos) return cap - 1;
    return pos + (size_t)n;
}

static size_t dbg_row(char *out, size_t cap, size_t pos, const char *kind, const DbgRow *r) {
    return dbg_add(out, cap, pos,
        "%s%d inst=%s tok=%s freq=%.2f gain=%.3f age=%.3f gate=%.3f "
        "att=%.3f dec=%.3f sus=%.3f rel=%.3f off=%d "
        "smp=%d ch=%d begin=%.3f end=%.3f pos=%.2f len=%d step=%.3f "
        "cut=%.3f q=%.3f ft=%.0f fc=%.2f hcut=%.3f hq=%.3f hfc=%.2f "
        "band=%.3f bq=%.3f bfc=%.2f orb=%d dly=%.3f shape=%.3f at=%.3f\n",
        kind, r->slot, audio_inst_name(r->inst), r->token,
        r->freq, r->gain, r->age, r->gate,
        r->att, r->dec, r->sus, r->rel, r->released,
        r->smp, r->channels, r->begin, r->end, r->pos, r->smplen, r->step,
        r->cut, r->q, r->ft, r->fc, r->hcut, r->hq, r->hfc,
        r->band, r->bq, r->bfc, r->orbit, r->delay, r->shape, r->t);
}

int audio_debug_snapshot(char *out, size_t outlen, int limit) {
    DbgRow live[DBG_RING];
    DbgRow trig[DBG_RING];
    int nlive = 0, ntrig = 0, active = 0, retiring = 0, i;
    size_t pos = 0;
    if (!out || outlen < 2) return -1;
    if (limit < 1) limit = 1;
    if (limit > DBG_RING) limit = DBG_RING;
    out[0] = 0;
    pthread_mutex_lock(&voice_lock);
    for (i = 0; i < MAX_VOICES; i++) {
        if (!voices[i].active) continue;
        active++;
        if (nlive < limit) dbg_from_voice(&live[nlive++], &voices[i], i);
    }
    for (i=MAX_VOICES;i<VOICE_SLOTS;i++) retiring+=voices[i].active!=0;
    ntrig = dbg_count < limit ? dbg_count : limit;
    for (i = 0; i < ntrig; i++) {
        int idx = (dbg_pos - 1 - i + DBG_RING * 2) % DBG_RING;
        trig[i] = dbg_ring[idx];
    }
    pthread_mutex_unlock(&voice_lock);
    pos = dbg_add(out, outlen, pos,
        "t=%.3f playing=%s bpm=%.1f pool=%d/%d retiring=%d/%d\n"
        "fx drive=%.3f cutoff=%.3f delay=%.3f verb=%.3f\n"
        "voices %d showing %d\n",
        audio_time, playing ? "yes" : "no", bpm, active, MAX_VOICES, retiring, MAX_VOICES,
        fx_drive, fx_cutoff, fx_delay, fx_verb, active, nlive);
    for (i = 0; i < nlive; i++) pos = dbg_row(out, outlen, pos, "v", &live[i]);
    pos = dbg_add(out, outlen, pos, "triggers %d\n", ntrig);
    for (i = 0; i < ntrig; i++) pos = dbg_row(out, outlen, pos, "t", &trig[i]);
    return (int)pos;
}

typedef struct {
    NativeSynthState *left, *right;
    const StretchBuffer *stretch;
} VoiceResources;

static VoiceResources voice_resources(const Voice *voice) {
    return (VoiceResources){voice->native_state,voice->native_right,voice->stretch_buffer};
}

/* Match inserting a linear ramp into the source's existing gain automation.
   Oscillator gain is its ADSR; sampled sources have a separate unity cut gain.
   In particular, do not multiply an oscillator envelope by a second fade. */
static VoiceResources retire_voice_locked(int chosen, double incoming_time) {
    int slot=MAX_VOICES;
    for (;slot<VOICE_SLOTS;slot++) if (!voices[slot].active) break;
    if (slot==VOICE_SLOTS) {
        slot=MAX_VOICES;
        for (int i=MAX_VOICES+1;i<VOICE_SLOTS;i++)
            if (voices[i].retire_end<voices[slot].retire_end) slot=i;
        if (retirement_overflow<UINT_MAX) retirement_overflow++;
    }
    VoiceResources displaced=voice_resources(&voices[slot]);
    voices[slot]=voices[chosen];
    Voice *voice=&voices[slot];
    voice->retiring=1;
    voice->retire_end=incoming_time+.25;
    voice->retire_anchor=audio_time; voice->retire_level=1;
    if (!voice->sampled_source && !native_synth_enveloped(voice->native_program)) {
        double gate=fmax(0,voice->gate_end-voice->start_time);
        double attack=fmax(.001,voice->vatt),decay=fmax(0,voice->vdec);
        double sustain=fmax(0,fmin(1,voice->vsus)),release=fmax(.005,voice->vrel);
        double times[]={0,fmin(attack,gate),fmin(attack+decay,gate),gate,gate+release};
        voice->retire_anchor=voice->start_time; voice->retire_level=0;
        for (unsigned i=0;i<sizeof(times)/sizeof(times[0]);i++) {
            double at=voice->start_time+times[i];
            if (at<=voice->retire_end && at>=voice->retire_anchor) {
                voice->retire_anchor=at;
                voice->retire_level=dsp_envelope(times[i],gate,attack,decay,sustain,release);
            }
        }
    }
    return displaced;
}

static double retiring_envelope(const Voice *voice, double envelope) {
    if (!voice->retiring || audio_time<voice->retire_anchor) return envelope;
    double span=voice->retire_end-voice->retire_anchor;
    return span>0 ? voice->retire_level*fmax(0,(voice->retire_end-audio_time)/span) : 0;
}

static double source_cut_gain(const Voice *voice) {
    double cut=voice->cut_pending?fmax(0,fmin(1,1-(audio_time-voice->cut_time)/.01)):1;
    if (!voice->retiring) return cut;
    if (voice->cut_pending && voice->cut_time<=voice->retire_end) {
        if (voice->cut_time+.01<voice->retire_end) return cut;
        if (audio_time<voice->cut_time) return 1;
        return fmax(0,(voice->retire_end-audio_time)/(voice->retire_end-voice->cut_time));
    }
    return retiring_envelope(voice,1);
}

/* A rest or failed source removes an entry without inserting a replacement. */
static int oldest_full_pool_voice(void) {
    int chosen=0;
    for (int i=0;i<MAX_VOICES;i++) {
        if (!voices[i].active) return -1;
        if (voices[i].onset_serial<voices[chosen].onset_serial) chosen=i;
    }
    return chosen;
}

void audio_trigger_event(AudioEvent *ev) {
    if (ev->csound_only || ev->osc_only || ev->midi_only) return;
    if (!playing && !atomic_load(&reference_mode)) return;
    if (ev->eviction_only) {
        VoiceResources previous={0};
        pthread_mutex_lock(&voice_lock);
        if (silent_clock) audio_time=audio_time_now();
        int chosen=oldest_full_pool_voice();
        if (chosen>=0) {
            previous=retire_voice_locked(chosen,ev->time-(ev->stretch>0?.04:0));
            memset(&voices[chosen],0,sizeof(voices[chosen]));
        }
        pthread_mutex_unlock(&voice_lock);
        native_synth_state_free(previous.left); native_synth_state_free(previous.right);
        stretch_release(previous.stretch);
        return;
    }
    if (!ev->control_only) audio_register_orbit(ev->orbit);
    if (ev->dsp.enabled && ev->duck_mask) for (int o=0;o<N_ORBITS;o++) if (ev->duck_mask&(1u<<o)) {
        if (!audio_prepare_duck_timer(o,ev->time)) return;
        break;
    }
    AudioEvent prepared_event;
    if (!ev->delay_prepared || !ev->room_prepared) {
        prepared_event=*ev; ev=&prepared_event;
        if (!audio_prepare_delay(ev)) log_console("delay: invalid event or pending control capacity exceeded");
        audio_prepare_room(ev);
    }
    pthread_mutex_lock(&voice_lock);
    if (silent_clock) audio_time=audio_time_now();
    int controls_ready=schedule_duck_locked(ev);
    pthread_mutex_unlock(&voice_lock);
    if (!controls_ready) log_console("duck: pending control capacity exceeded");
    if (ev->control_only) return;
    NativeSynthState *prepared=ev->native_synth?native_synth_state_new(ev->native_synth):NULL;
    NativeSynthState *prepared_right=prepared && native_synth_effect(ev->native_synth)?native_synth_state_new(ev->native_synth):NULL;
    if (prepared && native_synth_effect(ev->native_synth) && !prepared_right) { native_synth_state_free(prepared); prepared=NULL; }
    if (ev->native_synth && !prepared) {
        NativeSynthState *retired[VOICE_SLOTS*2]; int count=0;
        pthread_mutex_lock(&voice_lock);
        for (int i=0;i<VOICE_SLOTS;i++) if (!voices[i].active && voices[i].native_state) {
            retired[count++]=voices[i].native_state; voices[i].native_state=NULL;
            if (voices[i].native_right) { retired[count++]=voices[i].native_right; voices[i].native_right=NULL; }
        }
        pthread_mutex_unlock(&voice_lock);
        for (int i=0;i<count;i++) native_synth_state_free(retired[i]);
        prepared=native_synth_state_new(ev->native_synth);
        prepared_right=prepared && native_synth_effect(ev->native_synth)?native_synth_state_new(ev->native_synth):NULL;
        if (prepared && native_synth_effect(ev->native_synth) && !prepared_right) { native_synth_state_free(prepared); prepared=NULL; }
    }
    if (ev->native_synth && !prepared) { log_console("native synth: state memory budget exceeded"); return; }
    pthread_mutex_lock(&voice_lock);    // find free voice
    int chosen = -1;
    for (int i = 0; i < MAX_VOICES; i++) {
        if (!voices[i].active) { chosen = i; break; }
    }
    if (chosen < 0) {
        /* Superdough's source Map evicts in insertion order, independently of
           gain and target time. Future notes can arrive out of time order. */
        chosen = 0;
        for (int i = 1; i < MAX_VOICES; i++) {
            if (voices[i].onset_serial < voices[chosen].onset_serial) {
                chosen = i;
            }
        }
    }
    /* Superdough applies the vocoder's 40 ms preroll before pool eviction. */
    double source_time=ev->time-(ev->stretch>0?.04:0);
    VoiceResources previous=voices[chosen].active?retire_voice_locked(chosen,source_time):voice_resources(&voices[chosen]);
    trigger_voice(&voices[chosen], ev);
    if (voices[chosen].active)
        prep_profile_event(PREP_RESERVED_SOURCE,ev->time,audio_time_now());
    prepare_source_cut(&voices[chosen]);
    voices[chosen].native_state=prepared;
    voices[chosen].native_right=prepared_right;
    dbg_push(&voices[chosen], chosen);
    pthread_mutex_unlock(&voice_lock);
    native_synth_state_free(previous.left);
    native_synth_state_free(previous.right);
    stretch_release(previous.stretch);
}

typedef struct {
    double left, right, delay_l, delay_r, room_l, room_r, room_size;
} VoiceMix;

static void route_voice(Voice *v, double l, double r, VoiceMix *mix) {
    if (native_synth_effect(v->native_program)) {
        double ignored, input_r=r;
        native_synth_process(v->native_state,v->age,v->gate_end-v->start_time,v->freq,v->start_time,
                             l,l,&l,&ignored);
        native_synth_process(v->native_right,v->age,v->gate_end-v->start_time,v->freq,v->start_time,
                             input_r,input_r,&ignored,&r);
    }
    double left = v->pan <= 0 ? 1 : 1-v->pan;
    double right = v->pan >= 0 ? 1 : 1+v->pan;
    if (v->dsp.control.enabled) {
        int stereo = v->smp_channels == 2 || v->inst == INST_SUPERSAW || native_synth_stereo(v->native_program);
        double pan = fmax(-1,fmin(1,v->pan));
        if (!v->has_pan) {
            /* With no pan control, WebAudio upmixes mono at unity. */
        } else if (!stereo) {
            l *= cos((pan+1)*M_PI/4); r *= sin((pan+1)*M_PI/4);
        } else if (pan <= 0) {
            l += r*cos((pan+1)*M_PI/2); r *= sin((pan+1)*M_PI/2);
        } else {
            r += l*sin(pan*M_PI/2); l *= cos(pan*M_PI/2);
        }
        left = right = 1;
    }
    double dry = v->dsp.control.enabled ? v->dsp.control.p[DSP_DRY] : 1;
    unsigned scope_mask = v->scope_mask;
    for (int i = 0; scope_mask; i++, scope_mask >>= 1) if ((scope_mask&1u) && v->scope_generation == pattern_scope_generation[i])
        pattern_scope_frame[i] += (l*left+r*right)*dry*.5;
    mix->left += l*left*dry; mix->right += r*right*dry;
    mix->delay_l += l*left*v->delay_send;
    mix->delay_r += r*right*v->delay_send;
    if (v->dsp.control.enabled) {
        double room = v->dsp.control.p[DSP_ROOM];
        mix->room_l += l*left*room; mix->room_r += r*right*room;
        if (room > 0) mix->room_size = v->dsp.control.p[DSP_SIZE];
    }
}

static void mix_voice(Voice *v, double sample, VoiceMix *mix) {
    sample = dsp_process(&v->dsp,sample,v->age,v->gate_end-v->start_time);
    route_voice(v,sample,sample,mix);
}

static void mix_supersaw(Voice *v, double amplitude, VoiceMix *mix) {
    double age = v->age, gate = v->gate_end-v->start_time;
    double left = dsp_process(&v->dsp,v->dsp.supersaw_l*amplitude,age,gate);
    double right = dsp_process(&v->dsp_right,v->dsp.supersaw_r*amplitude,age,gate);
    route_voice(v,left,right,mix);
}

static int voice_has_effect_tail(const Voice *v) {
    const double *p=v->dsp.control.p;
    return v->dsp.control.enabled && (native_synth_effect(v->native_program) ||
        p[DSP_LPF]>0 || p[DSP_HPF]>0 || p[DSP_BPF]>0 || p[DSP_VOWEL]>=0 ||
        p[DSP_PHASER]>0 || p[DSP_CHORUS]>0 || p[DSP_COARSE]>1);
}

static void native_reverb(int orbit, VoiceMix *mix) {
    static float comb[N_ORBITS][2][4][16384], allpass[N_ORBITS][2][2][2048];
    static unsigned pos[N_ORBITS], ap_pos[N_ORBITS];
    static double size[N_ORBITS];
    const int lengths[] = {1116,1188,1277,1356};
    if (orbit < 0 || orbit >= N_ORBITS || !mix) return;
    unsigned long long revision=atomic_load(&ir_revision[orbit]);
    const ConvKernel *kernel=atomic_load(&requested_ir[orbit]);
    if ((revision&1) || revision!=atomic_load(&ir_revision[orbit])) kernel=convolvers[orbit].kernel;
    else if (revision!=rendered_ir_revision[orbit]) {
        convolution_set(&convolvers[orbit],NULL);
        rendered_ir_revision[orbit]=revision;
    }
    convolution_set(&convolvers[orbit],kernel);
    if (kernel) {
        double left, right;
        convolution_process(&convolvers[orbit],mix->room_l,mix->room_r,&left,&right);
        mix->left += left; mix->right += right;
        return;
    }
    /* A WebAudio convolver with no completed buffer has no wet output. */
    if (atomic_load(&prepared_rooms)&(1u<<orbit)) return;
    if (mix->room_size >= 0) size[orbit] = mix->room_size;
    else if (size[orbit] == 0) size[orbit] = 1;
    for (int ch = 0; ch < 2; ch++) {
        double input = ch ? mix->room_r : mix->room_l, wet = 0;
        for (int k = 0; k < 4; k++) {
            int delay = (int)(lengths[k]*(.25+size[orbit]*.75)) + ch*23;
            if (delay > 16383) delay = 16383;
            unsigned read = (pos[orbit]-(unsigned)delay)&16383;
            double sample = comb[orbit][ch][k][read];
            comb[orbit][ch][k][pos[orbit]&16383] = (float)tanh(input+sample*.84);
            wet += sample*.25;
        }
        for (int k = 0; k < 2; k++) {
            unsigned read = (ap_pos[orbit]-(unsigned)(k ? 441 : 556))&2047;
            double sample = allpass[orbit][ch][k][read];
            allpass[orbit][ch][k][ap_pos[orbit]&2047] = (float)(wet+sample*.5);
            wet = sample-wet*.5;
        }
        if (ch) mix->right += wet*.4; else mix->left += wet*.4;
    }
    pos[orbit]++; ap_pos[orbit]++;
}

/* Event sends stay on their orbit. The TUI fx delay is a separate master line. */
static void orbit_delay(int orbit, VoiceMix *mix) {
    static float dbuf[N_ORBITS][2][ORBIT_DELAY];
    static int dpos[N_ORBITS];
    static float feedback_quantum[N_ORBITS][2][128];
    static unsigned feedback_pos[N_ORBITS];
    if (orbit < 0 || orbit >= N_ORBITS || !mix) return;
    double b = effect_bpm;
    double dt = orbit_dt[orbit] >= 0 ? orbit_dt[orbit] : (0.75 * 60.0 / b);
    double fb = orbit_fb[orbit] >= 0 ? orbit_fb[orbit] : 0.5;
    double maximum_fb=web_delay_created[orbit]?(float).98:.98;
    if (fb > maximum_fb) fb = maximum_fb;
    if (fb < 0) fb = 0;
    if (dt > 1.99) dt = 1.99;
    if (!web_delay_created[orbit] && dt < 1.0 / SR) dt = 1.0 / SR;
    double frames=web_delay_created[orbit]?(float)((float)dt*(float)SR):dt*SR;
    int dl = (int)frames;
    if (dl < (web_delay_created[orbit]?0:1)) dl = web_delay_created[orbit]?0:1;
    if (dl >= ORBIT_DELAY) dl = ORBIT_DELAY - 1;
    unsigned fp=feedback_pos[orbit];
    if (web_delay_created[orbit]) {
        dbuf[orbit][0][dpos[orbit]]=(float)(mix->delay_l+feedback_quantum[orbit][0][fp]);
        dbuf[orbit][1][dpos[orbit]]=(float)(mix->delay_r+feedback_quantum[orbit][1][fp]);
    }
    int rp = dpos[orbit] - dl;
    if (rp < 0) rp += ORBIT_DELAY;
    float de_l = dbuf[orbit][0][rp], de_r = dbuf[orbit][1][rp];
    if (web_delay_created[orbit]) {
        float fraction=(float)(frames-dl);
        int previous=rp?rp-1:ORBIT_DELAY-1;
        de_l=de_l*(1-fraction)+dbuf[orbit][0][previous]*fraction;
        de_r=de_r*(1-fraction)+dbuf[orbit][1][previous]*fraction;
    }
    feedback_quantum[orbit][0][fp]=web_delay_created[orbit]?(float)(de_l*fb):0;
    feedback_quantum[orbit][1][fp]=web_delay_created[orbit]?(float)(de_r*fb):0;
    feedback_pos[orbit]=(fp+1)&127;
    if (!web_delay_created[orbit]) {
        dbuf[orbit][0][dpos[orbit]] = (float)(mix->delay_l + de_l*fb);
        dbuf[orbit][1][dpos[orbit]] = (float)(mix->delay_r + de_r*fb);
    }
    if (++dpos[orbit] >= ORBIT_DELAY) dpos[orbit] = 0;
    mix->left += de_l;
    mix->right += de_r;
}

static double source_value(const Voice *v, double position, int channel) {
    if (v->dsp.control.p[DSP_LOOP] > 0) {
        double lo = v->smplen*v->dsp.control.p[DSP_LOOPBEGIN];
        double hi = v->smplen*v->dsp.control.p[DSP_LOOPEND];
        if (hi > lo && ((v->smpstep >= 0 && position >= hi) || (v->smpstep < 0 && position < lo))) {
            position = lo+fmod(position-lo,hi-lo);
            if (position < lo) position += hi-lo;
        }
    }
    /* Deployed reverseBuffer copies each reversed channel at an offset equal
       to its channel index. Preserve the right channel's leading zero and
       one-source-frame displacement for web sample playback. */
    int reverse_offset=v->dsp.control.enabled && v->smpstep<0 && v->smp_channels==2 && channel==1;
    if (reverse_offset) position+=1;
    if (position < 0 || position >= v->smplen || !isfinite(position)) return 0;
    int index = (int)position;
    double fraction = position-index;
    double a = v->smpbuf[(size_t)index*2+channel];
    double b = index+1 < v->smplen ? v->smpbuf[(size_t)(index+1)*2+channel] : reverse_offset?0:a;
    return a+(b-a)*fraction;
}

/* Audio callback */
static int pa_callback(const void *input, void *output, unsigned long frameCount,
                       const PaStreamCallbackTimeInfo *timeInfo, PaStreamCallbackFlags statusFlags, void *userData) {
    (void)input; (void)timeInfo; (void)statusFlags; (void)userData;
    float *out = (float*)output;
    for (unsigned long i = 0; i < frameCount; i++) {
        VoiceMix mixes[N_ORBITS];
        memset(pattern_scope_frame,0,sizeof(pattern_scope_frame));
        for (int o = 0; o < N_ORBITS; o++) {
            mixes[o] = (VoiceMix){0};
            mixes[o].room_size = -1;
        }
        double L = 0, R = 0;
        double cs_l = 0, cs_r = 0;
        double duck_g[N_ORBITS];
        orbits_ensure();
        if (playing || atomic_load(&reference_mode)) {
            pthread_mutex_lock(&voice_lock);
            double dt = 1.0/SR;
            dispatch_voice_onsets();
            for (int v = 0; v < VOICE_SLOTS; v++) {
                Voice *V = &voices[v];
                if (!V->active) {
                    if (V->stretch_buffer) { stretch_release(V->stretch_buffer); V->stretch_buffer=NULL; }
                    continue;
                }
                if (V->retiring && audio_time>=V->retire_end-1e-10) {
                    /* stop() silences the source, while its downstream nodes
                       remain connected until the original graph cleanup. */
                    if (voice_has_effect_tail(V) && audio_time<V->graph_end) {
                        double gate=V->gate_end-V->start_time;
                        /* The web's eviction gain sits downstream of the
                           phaser: a retired phaser goes silent at stop while
                           downstream biquads still ring. Bypass the swept
                           allpass cascade here so its frozen state cannot be
                           pumped back up; everything else flushes normally. */
                        double phaser=V->dsp.control.p[DSP_PHASER];
                        double phaser_right=V->dsp_right.control.p[DSP_PHASER];
                        V->dsp.control.p[DSP_PHASER]=0;
                        V->dsp_right.control.p[DSP_PHASER]=0;
                        double left=dsp_process(&V->dsp,0,V->age,gate),right=left;
                        if(V->smp_channels==2 || V->inst==INST_SUPERSAW || V->inst==INST_NATIVE)
                            right=dsp_process(&V->dsp_right,0,V->age,gate);
                        V->dsp.control.p[DSP_PHASER]=phaser;
                        V->dsp_right.control.p[DSP_PHASER]=phaser_right;
                        route_voice(V,left,right,&mixes[V->orbit]);
                        V->age+=dt;
                        continue;
                    }
                    V->active=0;
                    if (V->stretch_buffer) { stretch_release(V->stretch_buffer); V->stretch_buffer=NULL; }
                    continue;
                }
                if (V->start_time>audio_time+1e-10) continue;
                int orbit = V->orbit;
                if (orbit < 0 || orbit >= N_ORBITS) orbit = 0;
                VoiceMix *vm = &mixes[orbit];
                double age = V->age;
                /* Sample cut fades the source before its downstream effects;
                   retain the graph until its original envelope/lifetime ends. */
                double cut_gain=source_cut_gain(V);
                if (V->stretch_buffer) {
                    const float *pcm=stretch_pcm(V->stretch_buffer);
                    if (V->stretch_frame>=(unsigned)stretch_frames(V->stretch_buffer)) {
                        V->active=0; stretch_release(V->stretch_buffer); V->stretch_buffer=NULL; continue;
                    }
                    double gate=V->gate_end-V->start_time;
                    double left=pcm[(size_t)V->stretch_frame*2]*V->amp*cut_gain;
                    double right=pcm[(size_t)V->stretch_frame*2+1]*V->amp*cut_gain;
                    V->env=dsp_envelope(age,gate,V->vatt,V->vdec,V->vsus,V->vrel);
                    route_voice(V,dsp_process(&V->dsp,left,age,gate),dsp_process(&V->dsp_right,right,age,gate),vm);
                    V->stretch_frame++; V->smppos+=V->smpstep; V->age+=dt; continue;
                }
                if (V->inst==INST_NATIVE) {
                    double gate=V->gate_end-V->start_time, left=0,right=0;
                    int own_envelope=native_synth_enveloped(V->native_program);
                    double env=retiring_envelope(V,own_envelope?1:dsp_envelope(age,gate,V->vatt,V->vdec,V->vsus,V->vrel));
                    double tail=native_synth_tail(V->native_program);
                    if ((tail>=0 && age>=gate+fmax(V->vrel,tail)) ||
                        !native_synth_process(V->native_state,age,gate,V->freq,V->start_time,0,0,&left,&right)) {
                        V->active=0; continue;
                    }
                    V->env=env;
                    left=dsp_process(&V->dsp,left*env*V->amp,age,gate);
                    right=dsp_process(&V->dsp_right,right*env*V->amp,age,gate);
                    route_voice(V,left,right,vm); V->age+=dt; continue;
                }
                // envelope
                double env = 0;
                // ADSR (for synth). For drums use custom.
                int is_drum = (V->inst >= INST_KICK && V->inst <= INST_TOM);
                int smp_lim = V->smplen;
                double lower = 0;
                /* Legacy notes use a hard slice. WebAudio samples run to the
                   buffer boundary; end sets the default envelope gate, so
                   release can continue beyond the slice. Explicit clip/loop/
                   release instead use the hap duration set by trigger_voice. */
                if (!V->dsp.control.enabled) {
                    lower = V->smplen * V->smp_begin;
                    if (V->smp_end > 0.0 && V->smp_end < 1.0) {
                        smp_lim = (int)(V->smplen * V->smp_end);
                        if (smp_lim < 1) smp_lim = 1;
                    }
                }
                if (V->smpbuf && V->smppos >= 0.0 && (int)V->smppos < smp_lim) {
                    /* one-shot bank sample, mono center, linear interp.
                       Gated voices (melodic banks) stop at note end.
                       A negative step plays the buffer backwards. */
                    if (V->smp_gate && !V->released && audio_time >= V->note_off_time)
                        V->released = 1;
                    double g = 1.0;
                    if (V->dsp.control.enabled) {
                        double gate = V->gate_end-V->start_time;
                        if (age >= gate) V->released = 1;
                        g = dsp_envelope(age,gate,V->vatt,V->vdec,V->vsus,V->vrel);
                        if (age >= gate+V->vrel) { V->active = 0; continue; }
                    } else if (V->smp_gate && V->released) {
                        double r = V->vrel >= 0 ? V->vrel : 0.08;
                        if (r < 0.005) r = 0.005;
                        double rt = audio_time - V->note_off_time;
                        if (rt >= r) { V->active = 0; continue; }
                        g = 1.0 - rt / r;
                    }
                    double left = source_value(V,V->smppos,0), right = source_value(V,V->smppos,1);
                    double transport = V->smpstep*dsp_pitch_multiplier(&V->dsp.control,age,V->gate_end-V->start_time);
                    g*=cut_gain;
                    double s = left*V->amp*g;
                    if (V->smp_channels == 2 && (V->dsp.control.enabled || V->lpf_hz <= 0)) {
                        right *= V->amp*g;
                        double gate = V->gate_end-V->start_time;
                        route_voice(V,dsp_process(&V->dsp,s,age,gate),dsp_process(&V->dsp_right,right,age,gate),vm);
                    } else {
                        if (V->smp_channels == 2) s = (s+right*V->amp*g)*.5;
                        if (V->lpf_hz > 0.0) s = apply_lpf(V, s, voice_fc(V, V->lpf_hz));
                        mix_voice(V,s,vm);
                    }
                    V->smppos += transport;
                    V->age += dt;
                    if (V->dsp.control.enabled && V->dsp.control.p[DSP_LOOP] > 0) {
                        double lo = V->smplen*V->dsp.control.p[DSP_LOOPBEGIN];
                        double hi = V->smplen*V->dsp.control.p[DSP_LOOPEND];
                        if (hi > lo && ((V->smpstep >= 0 && V->smppos >= hi) ||
                                        (V->smpstep < 0 && V->smppos < lo))) {
                            V->smppos = lo + fmod(V->smppos-lo,hi-lo);
                            if (V->smppos < lo) V->smppos += hi-lo;
                        }
                    }
                    if (V->smppos < lower || V->smppos >= smp_lim) V->active = 0;
                    continue;
                } else if (V->smpbuf) { V->active = 0; continue; }
                if (is_drum) {
                    /* Stolen drums fade like every other evicted source; the
                       web ramps each evicted pool entry to zero at the
                       incoming target plus 250 ms regardless of type. */
                    double drum_fade = retiring_envelope(V,1.0);
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
                        mix_voice(V,s*V->amp*.9*drum_fade,vm);
                    } else if (V->inst == INST_SNARE) {
                        double t = age;
                        double dur = 0.25;
                        if (t >= dur) { V->active = 0; continue; }
                        double env = exp(-t*10.0);
                        // noise + tone
                        double noise = ((double)rand()/RAND_MAX)*2-1;
                        double tone = wt_sample(V->wt, 200.0*audio_time); // fixed 200
                        double s = (0.7*noise + 0.3*tone)*env;
                        mix_voice(V,s*V->amp*.7*drum_fade,vm);
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
                        mix_voice(V,s*V->amp*.5*drum_fade,vm);
                    } else if (V->inst == INST_CP) {
                        double t = age;
                        double dur = 0.2;
                        if (t >= dur) { V->active = 0; continue; }
                        double env = exp(-t*12.0);
                        double noise = ((double)rand()/RAND_MAX)*2-1;
                        double s = noise*env;
                        mix_voice(V,s*V->amp*.6*drum_fade,vm);
                    } else if (V->inst == INST_TOM) {
                        double t = age;
                        double dur = 0.3;
                        if (t >= dur) { V->active = 0; continue; }
                        double env = exp(-t*6.0);
                        double pitch = V->freq;
                        V->phase += (2*M_PI * pitch)/SR;
                        double s = wt_sample(V->wt, V->phase/(2*M_PI))*env;
                        mix_voice(V,s*V->amp*.8*drum_fade,vm);
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
                    if (V->dsp.control.enabled)
                        V->env = dsp_envelope(age,V->gate_end-V->start_time,a,d,sus,r);
                    else V->env += (envtarget - V->env) * 0.02;
                    env = retiring_envelope(V,V->env);
                    /* one LFO per voice: triangle, sine, saw, square */
                    double freq = V->freq;
                    double amp = V->amp;
                    double pwuse = V->pw;
                    if (V->dsp.control.enabled && V->dsp.control.p[DSP_PWRATE] > 0) {
                        const double *p = V->dsp.control.p;
                        pwuse += sin(2*M_PI*age*p[DSP_PWRATE])*p[DSP_PWSWEEP];
                    }
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
                    freq = dsp_frequency(&V->dsp,freq*(V->stretch_factor > 0 ? V->stretch_factor : 1),V->age,V->gate_end-V->start_time);
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
                        case INST_NOISE: sample = V->dsp.control.enabled ? dsp_noise(&V->dsp,0) : ((double)rand()/RAND_MAX)*2-1; break;
                        case INST_PINK: sample = dsp_noise(&V->dsp,1); break;
                        case INST_BROWN: sample = dsp_noise(&V->dsp,2); break;
                        case INST_SUPERSAW: sample = dsp_supersaw(&V->dsp,freq); break;
                        case INST_ADDITIVE: sample = dsp_additive(&V->dsp.control,V->phase,freq); break;
                        default: sample = sin(V->phase); break;
                    }
                    if (V->dsp.control.enabled) {
                        if (V->inst == INST_SAW) sample = dsp_oscillator(0,V->phase,freq,.5);
                        else if (V->inst == INST_SQUARE) sample = dsp_oscillator(1,V->phase,freq,pwuse >= 0 ? pwuse : .5);
                        else if (V->inst == INST_TRI) sample = dsp_oscillator(2,V->phase,freq,.5);
                    }
                    if (V->lpf_hz > 0.0 || V->lfo_tgt == 3) {
                        double base = (V->lfo_tgt == 3) ? fc : V->lpf_hz;
                        sample = apply_lpf(V, sample, voice_fc(V, base));
                    }
                    V->phase += phaseinc;
                    if (V->phase > 2*M_PI || V->phase < -2*M_PI) V->phase = fmod(V->phase,2*M_PI);
                    V->age += dt;
                    double smp = sample * env * amp * (V->waveform_gain>0?V->waveform_gain:1);
                    /* Superdough oscillator sources have a fixed 0.3 gain
                       before the amplitude envelope and effects. */
                    if (V->dsp.control.enabled) { smp *= .3; amp *= .3; }
                    if (V->inst == INST_SUPERSAW) mix_supersaw(V,env*amp,vm);
                    else mix_voice(V,smp,vm);
                    (void)age;
                    continue;
                }
                V->age += dt;
            }
            csound_program_sample(csound_program,&cs_l,&cs_r);
            for (int o = 0; o < N_ORBITS; o++) {
                duck_g[o] = orbit_duck_gain(o);
                duck_t[o] += dt;
            }
            pattern_scope_publish();
            pthread_mutex_unlock(&voice_lock);
        } else {
            pthread_mutex_lock(&voice_lock);
            double discard_l = 0, discard_r = 0;
            csound_program_sample(csound_program,&discard_l,&discard_r);
            for (int o = 0; o < N_ORBITS; o++) {
                duck_g[o] = orbit_duck_gain(o);
                duck_t[o] += 1.0 / SR;
            }
            pattern_scope_publish();
            pthread_mutex_unlock(&voice_lock);
        }
        for (int o = 0; o < N_ORBITS; o++) {
            native_reverb(o, &mixes[o]);
            orbit_delay(o, &mixes[o]);
            L += mixes[o].left * duck_g[o];
            R += mixes[o].right * duck_g[o];
        }
        L += cs_l;
        R += cs_r;
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
        /* TUI `fx delay` is its own master send. Pattern `.delay()` uses orbit_delay.
           Keep ticking after the knob returns to 0 so the tail can finish. */
        {
            static float dbuf[2][ORBIT_DELAY];
            static int dpos = 0;
            double b = effect_bpm;
            double dt = 0.75 * 60.0 / b;
            if (dt > 1.99) dt = 1.99;
            int dl = (int)(dt * SR);
            if (dl < 1) dl = 1;
            if (dl >= ORBIT_DELAY) dl = ORBIT_DELAY - 1;
            int rp = dpos - dl;
            if (rp < 0) rp += ORBIT_DELAY;
            float de_l = dbuf[0][rp], de_r = dbuf[1][rp];
            double send = fx_delay > 0.001 ? fx_delay : 0;
            dbuf[0][dpos] = (float)(L * send + de_l * 0.5);
            dbuf[1][dpos] = (float)(R * send + de_r * 0.5);
            if (++dpos >= ORBIT_DELAY) dpos = 0;
            if (send > 0 || de_l > 1e-5f || de_l < -1e-5f || de_r > 1e-5f || de_r < -1e-5f) {
                L += de_l;
                R += de_r;
            }
        }
        /* DC blocker + tanh soft-clip limiter (replaces hard clip) */
        if (atomic_load_explicit(&reference_mode,memory_order_relaxed)) {
            /* Superdough's default destination gain is unity. Do not
               color every native event with the legacy DC/drive stage. */
            L = fmax(-1,fmin(1,L)); R = fmax(-1,fmin(1,R));
        } else {
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

static void render_output(float *output, unsigned long frames) {
    (void)pa_callback(NULL,output,frames,NULL,0,NULL);
}

void audio_init(void) {
    wt_init();
#ifdef __linux__
    snd_lib_error_set_handler(alsa_silent);
#endif
    midi_init();
    memset(voices, 0, sizeof(voices));
    retirement_overflow=0;
    pending_duck_count=0; pending_duck_serial=0;
    duck_error_count=0; duck_error_overflow=0;
    pending_delay_count=0; pending_delay_serial=0;
    memset(web_delay_created,0,sizeof(web_delay_created));
    memset(room_created,0,sizeof(room_created));
    atomic_store(&prepared_rooms,0);
    room_failures=0;
    atomic_store(&created_orbits,0);
    memset(web_duck,0,sizeof(web_duck));
    playing = 1;
    silent_clock=!audio_output_start(render_output,audio_block_frames);
    if (silent_clock) silent_clock_origin=now_seconds()-audio_time;
}
void audio_shutdown(void) {
    stop_room_worker();
    midi_shutdown();
    audio_output_stop();
    silent_clock=0;
    for (int i=0;i<VOICE_SLOTS;i++) {
        native_synth_state_free(voices[i].native_state); voices[i].native_state=NULL;
        native_synth_state_free(voices[i].native_right); voices[i].native_right=NULL;
        stretch_release(voices[i].stretch_buffer); voices[i].stretch_buffer=NULL;
    }
    audio_set_csound(NULL);
}
void audio_toggle_play(int on) { playing = on ? 1 : 0; }
int audio_is_playing(void) { return playing; }
int audio_is_stream_active(void) {
    return audio_output_active();
}
const char *audio_backend_name(void) { return audio_output_name(); }

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
        case INST_SUPERSAW: return "supersaw";
        case INST_PINK: return "pink";
        case INST_BROWN: return "brown";
        case INST_ADDITIVE: return "additive";
        case INST_NATIVE: return "native";
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

int audio_retiring_voices(void) {
    int n = 0;
    pthread_mutex_lock(&voice_lock);
    for (int i = MAX_VOICES; i < VOICE_SLOTS; i++)
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
