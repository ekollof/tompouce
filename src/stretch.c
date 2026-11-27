/* Native adaptation of superdough 1.2.6's PhaseVocoderProcessor/OLAProcessor
   (Strudel contributors, AGPL-3.0-or-later), originally olvb/phaze (Unlicense).
   Spectrum peak regions, phase correction and Float32 OLA follow that source.
   All transforms run during generator preparation, never in the callback. */
#include "stretch.h"
#include "vocoder.h"
#include "stretch_fft_simd.h"
#include "sampler.h"
#include "prep_profile.h"
#include <stdatomic.h>
#include <float.h>

#define CACHE_SIZE 128
#define CACHE_BYTES (128u*1024u*1024u)
typedef struct {
    char source_key[768];
    int sample, phase, frames;
    double fraction,step,gate,factor,begin,end,attack,decay,sustain,release;
    DspParams params;
} StretchKey;
struct StretchBuffer { atomic_uint refs; int frames; size_t bytes; double age_offset; float pcm[]; };
static struct {
    StretchKey key;
    StretchBuffer *buffer;
    unsigned long used;
    size_t reserved_bytes;
    int reserved;
    unsigned waiters;
} cache[CACHE_SIZE];
static size_t cache_bytes; /* published buffers plus in-flight reservations */
static unsigned long serial;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready=PTHREAD_COND_INITIALIZER;
static void stretch_lock(void) { prep_lock(&lock, PREP_LOCK_STRETCH); }
static void stretch_unlock(void) { prep_unlock(&lock, PREP_LOCK_STRETCH); }

void stretch_retain(const StretchBuffer *buffer) { if (buffer) atomic_fetch_add(&((StretchBuffer *)buffer)->refs,1); }
void stretch_release(const StretchBuffer *buffer) { if (buffer) atomic_fetch_sub(&((StretchBuffer *)buffer)->refs,1); }
int stretch_frames(const StretchBuffer *buffer) { return buffer?buffer->frames:0; }
const float *stretch_pcm(const StretchBuffer *buffer) { return buffer?buffer->pcm:NULL; }
double stretch_age_offset(const StretchBuffer *buffer) { return buffer?buffer->age_offset:0; }

const StretchBuffer *stretch_pcm_lookup(const char *key) {
    if (!key || !*key || strlen(key)>=sizeof(cache[0].key.source_key)) return NULL;
    stretch_lock();
    for (int i=0;i<CACHE_SIZE;i++) if (cache[i].buffer && !strcmp(cache[i].key.source_key,key)) {
        cache[i].used=++serial; stretch_retain(cache[i].buffer); stretch_unlock(); return cache[i].buffer;
    }
    stretch_unlock(); return NULL;
}

/* Caller holds the cache lock. In-flight reservations count toward the byte
   bound, so parallel preparation cannot exceed the cache budget. */
static int cache_reserve_locked(size_t bytes) {
    if (bytes>CACHE_BYTES) return -1;
    for (;;) {
        int slot=-1;
        for (int i=0;i<CACHE_SIZE;i++) if (!cache[i].buffer && !cache[i].reserved && !cache[i].waiters) { slot=i; break; }
        if (slot>=0 && cache_bytes+bytes<=CACHE_BYTES) {
            memset(&cache[slot].key,0,sizeof(cache[slot].key));
            cache[slot].reserved=1; cache[slot].reserved_bytes=bytes; cache_bytes+=bytes;
            return slot;
        }
        int oldest=-1;
        for (int i=0;i<CACHE_SIZE;i++) if (cache[i].buffer && !cache[i].waiters && !atomic_load(&cache[i].buffer->refs) &&
            (oldest<0 || cache[i].used<cache[oldest].used)) oldest=i;
        if (oldest<0) return -1;
        cache_bytes-=cache[oldest].buffer->bytes;
        free(cache[oldest].buffer); cache[oldest].buffer=NULL;
    }
}

static void cache_release_reservation_locked(int slot) {
    if (slot<0 || slot>=CACHE_SIZE) return;
    cache_bytes-=cache[slot].reserved_bytes;
    cache[slot].reserved_bytes=0; cache[slot].reserved=0;
    pthread_cond_broadcast(&ready);
}

/* The compute runs outside the lock. A duplicate that completed first wins;
   the discarded buffer was never published or visible to another thread. */
static const StretchBuffer *cache_publish(int slot, const StretchKey *key, StretchBuffer *buffer) {
    stretch_lock();
    for (int i=0;i<CACHE_SIZE;i++) if (cache[i].buffer && !memcmp(&cache[i].key,key,sizeof(*key))) {
        StretchBuffer *existing=cache[i].buffer;
        cache_release_reservation_locked(slot);
        cache[i].used=++serial; stretch_retain(existing);
        stretch_unlock(); free(buffer); return existing;
    }
    cache[slot].key=*key; cache[slot].buffer=buffer; cache[slot].used=++serial;
    cache[slot].reserved=0; cache[slot].reserved_bytes=0;
    pthread_cond_broadcast(&ready);
    stretch_unlock(); return buffer;
}

const StretchBuffer *stretch_pcm_store(const char *key, float *pcm, int frames) {
    if (!key || !*key || strlen(key)>=sizeof(cache[0].key.source_key) || !pcm || frames<=0 || frames>90*44100) {
        free(pcm); return NULL;
    }
    size_t bytes=sizeof(StretchBuffer)+(size_t)frames*2*sizeof(float);
    stretch_lock();
    for (int i=0;i<CACHE_SIZE;i++) if (cache[i].buffer && !strcmp(cache[i].key.source_key,key)) {
        cache[i].used=++serial; stretch_retain(cache[i].buffer); stretch_unlock(); free(pcm); return cache[i].buffer;
    }
    int slot=cache_reserve_locked(bytes);
    stretch_unlock();
    if (slot<0) { free(pcm); return NULL; }
    StretchBuffer *buffer=malloc(bytes);
    if (!buffer) {
        stretch_lock(); cache_release_reservation_locked(slot); stretch_unlock();
        free(pcm); return NULL;
    }
    atomic_init(&buffer->refs,1); buffer->frames=frames; buffer->bytes=bytes; buffer->age_offset=0;
    memcpy(buffer->pcm,pcm,(size_t)frames*2*sizeof(float)); free(pcm);
    StretchKey keyed={0};
    snprintf(keyed.source_key,sizeof(keyed.source_key),"%s",key);
    return cache_publish(slot,&keyed,buffer);
}

const StretchBuffer *stretch_prepare(const AudioEvent *event) {
    if (!event || !isfinite(event->stretch) || event->stretch<=0 || event->stretch>17 || event->smp<0) return NULL;
    float *source; int length,channels;
    smp_get_stereo(event->smp,&source,&length,&channels);
    if (!source || length<1) return NULL;
    StretchKey key={0};
    key.sample=event->smp; key.factor=event->stretch;
    dsp_params_init(&key.params); key.params.enabled=event->dsp.enabled;
    static const int controls[]={DSP_PENV,DSP_PATT,DSP_PDEC,DSP_PSUS,DSP_PREL,DSP_PANCHOR,DSP_PCURVE,
        DSP_VIB,DSP_VIBMOD,DSP_LOOP,DSP_LOOPBEGIN,DSP_LOOPEND};
    for (unsigned i=0;i<sizeof(controls)/sizeof(*controls);i++) key.params.p[controls[i]]=event->dsp.p[controls[i]];
    key.attack=event->att; key.decay=event->dec; key.sustain=event->sus; key.release=event->rel;
    key.begin=event->dsp.p[DSP_BEGIN]; key.end=event->dsp.p[DSP_END];
    for (int i=0;i<DSP_COUNT;i++) if (!isfinite(key.params.p[i])) return NULL;
    if (!isfinite(key.attack) || !isfinite(key.decay) || !isfinite(key.sustain) || key.attack<0 ||
        key.decay<0 || !isfinite(key.begin) || !isfinite(key.end) || key.begin<0 || key.end>1 || key.end<=key.begin) return NULL;
    key.step=event->smp_speed;
    if (event->freq>0) key.step*=event->freq/(440*pow(2,(smp_base_midi(event->smp)-69)/12.0));
    if (!isfinite(key.step) || !isfinite(event->time) || !isfinite(event->duration) || key.step==0 ||
        !isfinite(key.release) || key.release<0) return NULL;
    key.step=(key.step<0?-1:1)*fmin(4096,fmax(.001,fabs(key.step)));
    /* BufferSource playbackRate is a float32 AudioParam. */
    key.step=(double)(float)key.step;
    key.gate=event->smp_gate?event->duration:length*(key.end-key.begin)/(fabs(key.step)*SR);
    if (key.gate<=0 || key.gate+key.release>90) return NULL;
    double onset=event->time*SR;
    if (!isfinite(onset)) return NULL;
    /* Playback starts on the first frame at/after the musical onset. Anchor
       the prepared PCM there too; using floor delayed fractional onsets by a
       frame when the callback waited until ceil(onset). Snap arithmetic noise
       around integer frames without removing intentional sub-frame timing. */
    double nearest=round(onset);
    if (fabs(onset-nearest)<=10*DBL_EPSILON*fmax(1,fabs(onset))) onset=nearest;
    double first_frame=ceil(onset);
    key.phase=(int)fmod(first_frame,VOCODE_FFT); if (key.phase<0) key.phase+=VOCODE_FFT;
    key.fraction=onset-first_frame;
    /* Source onended disconnects the entire browser graph on the next quantum;
       retaining the vocoder's trailing window would extend its release. */
    key.frames=(int)(ceil((key.phase+key.fraction-.04*SR+(key.gate+key.release+.01)*SR)/VOCODE_HOP)*VOCODE_HOP-key.phase);
    if (key.frames<0) key.frames=0;
    size_t bytes=sizeof(StretchBuffer)+(size_t)key.frames*2*sizeof(float);
    vocoder_tables_init();
    stretch_lock();
    for (int i=0;i<CACHE_SIZE;i++) if (cache[i].buffer && !memcmp(&cache[i].key,&key,sizeof(key))) {
        cache[i].used=++serial; stretch_retain(cache[i].buffer); stretch_unlock(); return cache[i].buffer;
    }
    for (int i=0;i<CACHE_SIZE;i++) if (cache[i].reserved && !memcmp(&cache[i].key,&key,sizeof(key))) {
        /* Pin the slot until each waiter retains the result (or observes
           failure). Only preparation threads wait; the callback never enters
           this API. Keep condition sleep out of cache-lock hold statistics. */
        if (cache[i].waiters>=CACHE_SIZE) { stretch_unlock(); return NULL; }
        cache[i].waiters++;
        while (cache[i].reserved) if (prep_cond_wait(&ready,&lock,PREP_LOCK_STRETCH)) {
            cache[i].waiters--; stretch_unlock(); return NULL;
        }
        StretchBuffer *result=cache[i].buffer;
        if (result) { cache[i].used=++serial; stretch_retain(result); }
        cache[i].waiters--;
        stretch_unlock();
        return result;
    }
    int slot=cache_reserve_locked(bytes);
    if (slot>=0) cache[slot].key=key;
    stretch_unlock();
    if (slot<0) return NULL;
    StretchBuffer *buffer=calloc(1,bytes);
    typedef struct { float input[2][VOCODE_FFT],sum[2][VOCODE_FFT]; VocoderComplex freq[VOCODE_FFT],shifted[VOCODE_FFT]; } Work;
    Work *work=calloc(1,sizeof(*work));
    if (!buffer || !work) {
        free(buffer); free(work);
        stretch_lock(); cache_release_reservation_locked(slot); stretch_unlock();
        return NULL;
    }
    buffer->frames=key.frames; buffer->bytes=bytes; atomic_init(&buffer->refs,1);
    buffer->age_offset=.04;
    /* Match the reference's 40ms source anticipation and global 128-frame
       OLA grid. PCM is indexed from the musical onset, including its tail. */
    double source_start=key.phase+key.fraction-.04*SR;
    long first=(long)floor(source_start/VOCODE_HOP)*VOCODE_HOP;
    double position=key.step<0?length-1-length*key.begin:length*key.begin;
    double loop_lo=length*key.params.p[DSP_LOOPBEGIN],loop_hi=length*key.params.p[DSP_LOOPEND];
    if (key.step<0) { double swap=loop_lo; loop_lo=length-loop_hi; loop_hi=length-swap; }
    int started=0;
    prep_work_begin(PREP_STRETCH);
    for (long block=first;block<key.phase+buffer->frames;block+=VOCODE_HOP) {
        for (int channel=0;channel<2;channel++) {
            memmove(work->input[channel],work->input[channel]+VOCODE_HOP,(VOCODE_FFT-VOCODE_HOP)*sizeof(float));
            memset(work->input[channel]+VOCODE_FFT-VOCODE_HOP,0,VOCODE_HOP*sizeof(float));
        }
        for (int i=0;i<VOCODE_HOP;i++) {
            double age=(block+i-source_start)/SR;
            if (age<0) continue;
            if (!started) { position+=age*SR*key.step; started=1; }
            if (key.params.p[DSP_LOOP]<=0 && (position<0 || position>=length)) {
                int end=(int)(ceil((block+i)/(double)VOCODE_HOP)*VOCODE_HOP-key.phase);
                if (end<buffer->frames) buffer->frames=end>0?end:0;
            }
            double env=dsp_envelope(age,key.gate,key.attack,key.decay,key.sustain,key.release);
            if (age<key.gate+key.release+.01 && position>=0 && position<length) {
                int index=(int)position; double fraction=position-index;
                for (int channel=0;channel<2;channel++) {
                    /* Match reverseBuffer's per-channel copy offset before
                       the source enters the vocoder, including its zero pad. */
                    int offset=key.step<0 && channels==2?channel:0;
                    int read_index=index+offset;
                    double a=read_index<length?source[(size_t)read_index*2+channel]:0;
                    double b=read_index+1<length?source[(size_t)(read_index+1)*2+channel]:offset?0:a;
                    work->input[channel][VOCODE_FFT-VOCODE_HOP+i]=(float)((a+(b-a)*fraction)*(float)env);
                }
            }
            position+=key.step*dsp_pitch_multiplier(&key.params,age,key.gate);
            if (key.params.p[DSP_LOOP]>0 && loop_hi>loop_lo &&
                ((key.step>=0 && position>=loop_hi) || (key.step<0 && position<loop_lo))) {
                position=loop_lo+fmod(position-loop_lo,loop_hi-loop_lo);
                if (position<loop_lo) position+=loop_hi-loop_lo;
            }
        }
        unsigned cursor=(unsigned)((block%VOCODE_FFT+VOCODE_FFT)%VOCODE_FFT);
        for (int channel=0;channel<(channels==1?1:2);channel++) {
            vocoder_transform(work->input[channel],work->sum[channel],key.factor,cursor,work->freq,work->shifted);
            for (int i=0;i<VOCODE_HOP;i++) {
                long at=block+i-key.phase;
                if (at>=0 && at<key.frames) {
                    buffer->pcm[(size_t)at*2+channel]=work->sum[channel][i];
                    /* Mono source channels have identical input, envelope,
                       pitch and OLA state; preserve stereo PCM bit for bit. */
                    if (channels==1) buffer->pcm[(size_t)at*2+1]=work->sum[channel][i];
                }
            }
            memmove(work->sum[channel],work->sum[channel]+VOCODE_HOP,(VOCODE_FFT-VOCODE_HOP)*sizeof(float));
            memset(work->sum[channel]+VOCODE_FFT-VOCODE_HOP,0,VOCODE_HOP*sizeof(float));
        }
    }
    prep_work_end(PREP_STRETCH);
    free(work);
    return cache_publish(slot,&key,buffer);
}

/* Live-streaming scalars for a stretched-sample voice. Mirrors the key
   derivation in stretch_prepare line for line (mapping, clamps, Float32
   cast, onset snap and frame count) but stops before buffer allocation.
   The caller renders the same input timeline streaming instead. */
int stretch_live_key(const AudioEvent *event, StretchLiveKey *out) {
    if (!event || !out || !isfinite(event->stretch) || event->stretch<=0 || event->stretch>17 || event->smp<0) return 0;
    float *source; int length,channels;
    smp_get_stereo(event->smp,&source,&length,&channels);
    if (!source || length<1) return 0;
    double step=event->smp_speed;
    if (event->freq>0) step*=event->freq/(440*pow(2,(smp_base_midi(event->smp)-69)/12.0));
    if (!isfinite(step) || !isfinite(event->time) || !isfinite(event->duration) || step==0) return 0;
    step=(step<0?-1:1)*fmin(4096,fmax(.001,fabs(step)));
    step=(double)(float)step;
    double begin=event->dsp.p[DSP_BEGIN], end=event->dsp.p[DSP_END];
    double gate=event->smp_gate?event->duration:length*(end-begin)/(fabs(step)*SR);
    if (!isfinite(event->rel) || event->rel<0) return 0;
    if (gate<=0 || gate+event->rel>90) return 0;
    double onset=event->time*SR;
    if (!isfinite(onset)) return 0;
    double nearest=round(onset);
    if (fabs(onset-nearest)<=10*DBL_EPSILON*fmax(1,fabs(onset))) onset=nearest;
    double first_frame=ceil(onset);
    double phase=fmod(first_frame,VOCODE_FFT); if (phase<0) phase+=VOCODE_FFT;
    double fraction=onset-first_frame;
    out->step=step; out->gate=gate; out->begin=begin;
    /* The baked grid counts from an M-shifted origin (multiples of 2048
       removed); in true absolute samples the source starts at the onset
       minus the 40 ms preroll. Cursor arithmetic is unaffected mod 2048. */
    out->origin=event->time-.04;
    out->frames=(int)(ceil((phase+fraction-.04*SR+(gate+event->rel+.01)*SR)/VOCODE_HOP)*VOCODE_HOP-phase);
    if (out->frames<0) out->frames=0;
    return 1;
}
