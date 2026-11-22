#define _GNU_SOURCE
#include "live_lv2.h"
#include "lv2_worker.h"
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define LIVE_BLOCK 256
#define LIVE_RING 2048
#define LIVE_TARGET 768
#define LIVE_EVENTS 128
#define LIVE_URI_MAX 256

typedef struct {
    double at;
    unsigned char size;
    unsigned char data[3];
} LiveEvent;

typedef struct {
    char uri[LIVE_URI_MAX+1];
    Lv2Worker *worker;
    pthread_t thread;
    atomic_int stop, active, failed;
    float ring[LIVE_RING*2];
    _Atomic uint32_t read_index, write_index;
    atomic_ullong underruns;
    double rate, start_time;
    unsigned long long rendered;
    LiveEvent events[LIVE_EVENTS];
    int event_count;
} LiveSlot;

static LiveSlot slots[LIVE_LV2_SLOTS];
static char rejected[LIVE_LV2_SLOTS][LIVE_URI_MAX+1];
static int rejected_count;
static pthread_mutex_t queue_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t registry_lock=PTHREAD_MUTEX_INITIALIZER;

static void sleep_ms(long milliseconds) {
    struct timespec pause={0,milliseconds*1000000L};
    while (nanosleep(&pause,&pause)!=0) {}
}

static void event_insert_sorted(LiveEvent *events, int count, LiveEvent event) {
    int at=count;
    while (at>0 && events[at-1].at>event.at) {
        events[at]=events[at-1];
        at--;
    }
    events[at]=event;
}

static void *live_thread(void *argument) {
    LiveSlot *slot=argument;
    float input[LIVE_BLOCK*LV2_WORKER_CHANNELS]={0};
    float output[LIVE_BLOCK*LV2_WORKER_CHANNELS];
    char error[256];
    const double block_seconds=LIVE_BLOCK/slot->rate;
    while (!atomic_load(&slot->stop)) {
        uint32_t write=atomic_load_explicit(&slot->write_index,memory_order_acquire);
        uint32_t read=atomic_load_explicit(&slot->read_index,memory_order_acquire);
        uint32_t used=write-read;
        if (used>LIVE_RING-LIVE_BLOCK || used>=LIVE_TARGET) {
            sleep_ms(1);
            continue;
        }
        double now=slot->start_time+(double)slot->rendered/slot->rate;
        LiveEvent due[LV2_WORKER_MIDI_EVENTS];
        int due_count=0, keep=0;
        pthread_mutex_lock(&queue_lock);
        for (int i=0;i<slot->event_count;i++) {
            LiveEvent event=slot->events[i];
            if (event.at<=now+block_seconds && due_count<LV2_WORKER_MIDI_EVENTS)
                due[due_count++]=event;
            else slot->events[keep++]=event;
        }
        slot->event_count=keep;
        pthread_mutex_unlock(&queue_lock);
        for (int i=1;i<due_count;i++)
            event_insert_sorted(due,i,due[i]);
        int sent=0;
        for (int i=0;i<due_count;i++) {
            if (!lv2_worker_send_midi_at(slot->worker,0,due[i].data,due[i].size,
                error,sizeof(error))) break;
            sent++;
        }
        int ok=sent==due_count &&
            lv2_worker_process(slot->worker,input,output,LIVE_BLOCK,error,sizeof(error));
        if (!ok) {
            atomic_store(&slot->failed,1);
            break;
        }
        for (int i=0;i<LIVE_BLOCK;i++) {
            size_t at=((write+(uint32_t)i)%LIVE_RING)*2;
            slot->ring[at]=output[i*2];
            slot->ring[at+1]=output[i*2+1];
        }
        atomic_store_explicit(&slot->write_index,write+LIVE_BLOCK,memory_order_release);
        slot->rendered+=LIVE_BLOCK;
    }
    atomic_store(&slot->active,0);
    return NULL;
}

int live_lv2_acquire(const char *uri, double sample_rate, double now,
                     char *error, size_t error_size) {
    if (error && error_size) error[0]=0;
    if (!uri || !*uri || strlen(uri)>LIVE_URI_MAX || !isfinite(sample_rate) ||
        sample_rate<=0 || !isfinite(now)) {
        if (error && error_size) snprintf(error,error_size,"invalid live LV2 request");
        return -1;
    }
    pthread_mutex_lock(&registry_lock);
    for (int i=0;i<LIVE_LV2_SLOTS;i++)
        if (slots[i].worker && !strcmp(slots[i].uri,uri)) {
            pthread_mutex_unlock(&registry_lock);
            return i;
        }
    for (int i=0;i<rejected_count;i++)
        if (!strcmp(rejected[i],uri)) {
            pthread_mutex_unlock(&registry_lock);
            if (error && error_size) snprintf(error,error_size,"plugin is not a MIDI instrument");
            return -1;
        }
    int slot=-1;
    for (int i=0;i<LIVE_LV2_SLOTS;i++) if (!slots[i].worker) { slot=i; break; }
    if (slot<0) {
        pthread_mutex_unlock(&registry_lock);
        if (error && error_size) snprintf(error,error_size,"live LV2 slots are full");
        return -1;
    }
    char worker_error[256]="";
    Lv2Worker *worker=lv2_worker_start(uri,sample_rate,worker_error,sizeof(worker_error));
    if (!worker) {
        pthread_mutex_unlock(&registry_lock);
        if (error && error_size) snprintf(error,error_size,"%s",worker_error);
        return -1;
    }
    if (!lv2_worker_is_instrument(worker)) {
        lv2_worker_stop(worker);
        if (rejected_count<LIVE_LV2_SLOTS)
            snprintf(rejected[rejected_count++],LIVE_URI_MAX+1,"%s",uri);
        pthread_mutex_unlock(&registry_lock);
        if (error && error_size) snprintf(error,error_size,"plugin is not a MIDI instrument");
        return -1;
    }
    LiveSlot *target=&slots[slot];
    memset(target->ring,0,sizeof(target->ring));
    atomic_store(&target->stop,0);
    atomic_store(&target->failed,0);
    atomic_store(&target->active,0);
    atomic_store(&target->underruns,0);
    atomic_store(&target->read_index,0);
    atomic_store(&target->write_index,0);
    target->event_count=0;
    target->rendered=0;
    target->rate=sample_rate;
    target->start_time=now;
    target->worker=worker;
    snprintf(target->uri,sizeof(target->uri),"%s",uri);
    if (pthread_create(&target->thread,NULL,live_thread,target)!=0) {
        target->worker=NULL;
        lv2_worker_stop(worker);
        pthread_mutex_unlock(&registry_lock);
        if (error && error_size) snprintf(error,error_size,"live LV2 thread failed");
        return -1;
    }
    atomic_store(&target->active,1);
    pthread_mutex_unlock(&registry_lock);
    return slot;
}

int live_lv2_note(int slot, double at, double duration, int channel, int note,
                  int velocity, int cc, int cc_value) {
    if (slot<0 || slot>=LIVE_LV2_SLOTS || !isfinite(at) || !isfinite(duration) ||
        duration<0 || channel<0 || channel>15 || note<0 || note>127 ||
        velocity<0 || velocity>127 || cc<-1 || cc>127 ||
        (cc>=0 && (cc_value<0 || cc_value>127))) return 0;
    LiveEvent pending[3];
    int count=0;
    unsigned status=(unsigned)(channel&15);
    if (cc>=0) {
        pending[count].at=at; pending[count].size=3;
        pending[count].data[0]=(unsigned char)(0xB0|status);
        pending[count].data[1]=(unsigned char)cc;
        pending[count].data[2]=(unsigned char)cc_value;
        count++;
    }
    pending[count].at=at; pending[count].size=3;
    pending[count].data[0]=(unsigned char)(0x90|status);
    pending[count].data[1]=(unsigned char)note;
    pending[count].data[2]=(unsigned char)(velocity>0?velocity:1);
    count++;
    if (duration>0) {
        pending[count].at=at+duration; pending[count].size=3;
        pending[count].data[0]=(unsigned char)(0x80|status);
        pending[count].data[1]=(unsigned char)note;
        pending[count].data[2]=0;
        count++;
    }
    pthread_mutex_lock(&queue_lock);
    LiveSlot *target=&slots[slot];
    if (!target->worker || target->event_count+count>LIVE_EVENTS) {
        pthread_mutex_unlock(&queue_lock);
        return 0;
    }
    for (int i=0;i<count;i++) target->events[target->event_count++]=pending[i];
    pthread_mutex_unlock(&queue_lock);
    return 1;
}

int live_lv2_cc(int slot, double at, int channel, int control, int value) {
    if (slot<0 || slot>=LIVE_LV2_SLOTS || !isfinite(at) || channel<0 ||
        channel>15 || control<0 || control>127 || value<0 || value>127) return 0;
    LiveEvent event={.at=at,.size=3,
        .data={(unsigned char)(0xB0|(channel&15)),(unsigned char)control,
               (unsigned char)value}};
    pthread_mutex_lock(&queue_lock);
    LiveSlot *target=&slots[slot];
    if (!target->worker || target->event_count>=LIVE_EVENTS) {
        pthread_mutex_unlock(&queue_lock);
        return 0;
    }
    target->events[target->event_count++]=event;
    pthread_mutex_unlock(&queue_lock);
    return 1;
}

void live_lv2_all_notes_off(void) {
    pthread_mutex_lock(&queue_lock);
    for (int i=0;i<LIVE_LV2_SLOTS;i++) {
        LiveSlot *slot=&slots[i];
        if (!slot->worker) continue;
        for (int channel=0;channel<16 && slot->event_count<LIVE_EVENTS;channel++) {
            LiveEvent event={.at=0,.size=3,
                .data={(unsigned char)(0xB0|channel),123,0}};
            slot->events[slot->event_count++]=event;
        }
    }
    pthread_mutex_unlock(&queue_lock);
}

void live_lv2_mix(float *left, float *right, unsigned frames) {
    if (!left || !right || frames==0) return;
    for (int i=0;i<LIVE_LV2_SLOTS;i++) {
        LiveSlot *slot=&slots[i];
        if (!atomic_load_explicit(&slot->active,memory_order_acquire) || !slot->worker)
            continue;
        uint32_t read=atomic_load_explicit(&slot->read_index,memory_order_acquire);
        uint32_t write=atomic_load_explicit(&slot->write_index,memory_order_acquire);
        uint32_t available=write-read;
        unsigned take=available<frames?available:frames;
        for (unsigned j=0;j<take;j++) {
            size_t at=((read+(uint32_t)j)%LIVE_RING)*2;
            left[j]+=slot->ring[at];
            right[j]+=slot->ring[at+1];
        }
        atomic_store_explicit(&slot->read_index,read+take,memory_order_release);
        if (take<frames) atomic_fetch_add(&slot->underruns,1);
    }
}

unsigned long long live_lv2_underruns(void) {
    unsigned long long total=0;
    for (int i=0;i<LIVE_LV2_SLOTS;i++)
        total+=atomic_load(&slots[i].underruns);
    return total;
}

void live_lv2_shutdown(void) {
    pthread_mutex_lock(&registry_lock);
    for (int i=0;i<LIVE_LV2_SLOTS;i++) {
        LiveSlot *slot=&slots[i];
        if (!slot->worker) continue;
        atomic_store(&slot->stop,1);
        pid_t pid=lv2_worker_pid(slot->worker);
        if (pid>0) kill(pid,SIGKILL);
        pthread_join(slot->thread,NULL);
        atomic_store(&slot->active,0);
        lv2_worker_stop(slot->worker);
        slot->worker=NULL;
        slot->event_count=0;
        slot->uri[0]=0;
    }
    pthread_mutex_unlock(&registry_lock);
}
