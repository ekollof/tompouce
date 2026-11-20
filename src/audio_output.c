#define _POSIX_C_SOURCE 200809L
#include "audio_output.h"
#include "audio.h"
#include "common.h"
#include <portaudio.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdint.h>
#include <errno.h>
#ifdef HAVE_PIPEWIRE
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#endif
#ifdef HAVE_PULSE
#include <pulse/pulseaudio.h>
#endif
#ifdef HAVE_SAMPLERATE
#include <samplerate.h>
#endif

#define OUTPUT_CHANNELS 2
#define OUTPUT_STRIDE (OUTPUT_CHANNELS*sizeof(float))
#define OUTPUT_FRAMES 4096
#define CONNECT_SECONDS 2

/* Exact-match named targets. The list is fixed at construction from a bounded
   allowlist so an arbitrary pattern string can never open a new stream or
   connect to an unbounded node name. */
static char named_targets[AUDIO_OUTPUT_NAMED_MAX][64];
static int named_count=-1;
static char warned_targets[AUDIO_OUTPUT_NAMED_MAX+1];

static void init_named_targets(void) {
    if (named_count>=0) return;
    named_count=0;
    const char *list=getenv("TOMPOUCE_OUTPUT_NAMES");
    if (!list || !*list) return;
    const char *cursor=list;
    while (*cursor && named_count<AUDIO_OUTPUT_NAMED_MAX) {
        const char *end=strchr(cursor,',');
        size_t length=end?(size_t)(end-cursor):strlen(cursor);
        if (length && length<sizeof(named_targets[0])) {
            memcpy(named_targets[named_count],cursor,length);
            named_targets[named_count][length]=0;
            named_count++;
        }
        if (!end) break;
        cursor=end+1;
    }
    char names[512]=""; size_t used=0;
    for (int i=0;i<named_count;i++) {
        int n=snprintf(names+used,sizeof(names)-used,"%s%s",i?",":"",named_targets[i]);
        if (n<=0 || (size_t)n>=sizeof(names)-used) break;
        used+=(size_t)n;
    }
    log_console("audio: named output candidates: %s",used?names:"(none)");
}

int audio_output_target(const char *name) {
    init_named_targets();
    if (!name || !*name) return 0;
    for (int i=0;i<named_count;i++) if (!strcmp(name,named_targets[i])) return i+1;
    if (!warned_targets[AUDIO_OUTPUT_NAMED_MAX]) {
        warned_targets[AUDIO_OUTPUT_NAMED_MAX]=1;
        log_console("audio: unknown output target %.64s; using the default stream",name);
    }
    return 0;
}

const char *audio_output_target_name(int index) {
    init_named_targets();
    return index>=1 && index<=named_count ? named_targets[index-1] : NULL;
}

int audio_output_named_count(void) { init_named_targets(); return named_count; }

/* Missed device cycles observed on the output thread. A gap between process
   callbacks beyond 1.5x the delivered quantum means the server cycled
   without this stream, i.e. an underrun of this stream's output. Quantum
   changes never mask a gap: benign resizes land near the new period, while
   an underrun gap exceeds both the old and the new quantum (servers often
   resize while recovering). Empty callbacks are reported separately. All
   counters are lock-free; timing state is output-thread local and never
   touched by DSP. */
static atomic_ullong xrun_calls, xrun_gaps, xrun_max_gap_ns, xrun_empty;
static struct timespec xrun_last;
static unsigned long xrun_last_frames;
static int xrun_seen, xrun_injected;
static long xrun_inject_ms;

static uint64_t timespec_delta_ns(const struct timespec *from, const struct timespec *to) {
    int64_t seconds = (int64_t)to->tv_sec-(int64_t)from->tv_sec;
    int64_t nsec = (int64_t)to->tv_nsec-(int64_t)from->tv_nsec;
    int64_t total = seconds*1000000000ll+nsec;
    return total > 0 ? (uint64_t)total : 0;
}

static void xrun_observe(unsigned long frames) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC,&now) != 0) return;
    atomic_fetch_add(&xrun_calls,1);
    if (xrun_seen && frames) {
        uint64_t gap = timespec_delta_ns(&xrun_last,&now);
        /* Count against the larger quantum: a benign quantum upshift lands
           near the new period, while an underrun exceeds both, even when
           the server resizes buffers while recovering. */
        uint64_t old_expected = (uint64_t)xrun_last_frames*1000000000ull/(uint64_t)SR;
        uint64_t new_expected = (uint64_t)frames*1000000000ull/(uint64_t)SR;
        uint64_t bound = old_expected > new_expected ? old_expected : new_expected;
        if (gap > bound+bound/2) {
            atomic_fetch_add(&xrun_gaps,1);
            uint64_t prev = atomic_load(&xrun_max_gap_ns);
            while (gap > prev && !atomic_compare_exchange_weak(&xrun_max_gap_ns,&prev,gap))
                ;
        }
    }
    xrun_last = now; xrun_last_frames = frames; xrun_seen = 1;
}

void audio_output_xrun_stats(unsigned long long *calls, unsigned long long *gaps,
    unsigned long long *max_gap_ns, unsigned long long *empty) {
    if (calls) *calls = atomic_load(&xrun_calls);
    if (gaps) *gaps = atomic_load(&xrun_gaps);
    if (max_gap_ns) *max_gap_ns = atomic_load(&xrun_max_gap_ns);
    if (empty) *empty = atomic_load(&xrun_empty);
}

void audio_output_xrun_reset(void) {
    atomic_store(&xrun_calls,0); atomic_store(&xrun_gaps,0);
    atomic_store(&xrun_max_gap_ns,0); atomic_store(&xrun_empty,0);
    xrun_seen = 0; xrun_last_frames = 0; xrun_injected = 0;
}

typedef struct {
    const char *name;
    int (*open)(void);
    void (*close)(void);
    int (*active)(void);
} OutputDriver;
static const OutputDriver *selected;
static AudioOutputRender render_audio;
static unsigned long block_frames;
static char output_error[192];

#ifdef HAVE_PIPEWIRE
/* Named-target streams share the main loop. The default stream's render fills
   shadow buffers; named streams copy a snapshot so neither re-renders DSP. */
#define TARGET_FRAMES OUTPUT_FRAMES
static float target_shadow[AUDIO_OUTPUT_NAMED_MAX+1][TARGET_FRAMES*2];
static float target_ready[AUDIO_OUTPUT_NAMED_MAX+1][TARGET_FRAMES*2];
static pthread_mutex_t target_lock=PTHREAD_MUTEX_INITIALIZER;

static struct {
    struct pw_thread_loop *loop;
    struct pw_stream *stream;
    struct pw_stream *named[AUDIO_OUTPUT_NAMED_MAX+1];
    int initialized, started, failed;
    atomic_int active;
} native_pw;

/* Explicit PipeWire links for named targets. WirePlumber's policy ignores
   target.object for JACK-bridged DSP nodes (no media.class), so declarative
   targeting can silently link a named stream to the default sink. Enumerate
   the registry once the loop is running and create the links ourselves. */
#define PW_MAX_PORTS 64
typedef struct {
    uint32_t node_id;             /* target node, 0 until found */
    uint32_t in_port[PW_MAX_PORTS];
    int n_in;
    uint32_t out_port[PW_MAX_PORTS];
    int n_out;
    int done;
    int seq, target_seq;
} PwLinkScan;

static const char *pw_prop(const struct spa_dict *dict, const char *key) {
    if (!dict) return NULL;
    const struct spa_dict_item *item;
    spa_dict_for_each(item, dict)
        if (!strcmp(item->key,key)) return item->value;
    return NULL;
}

static void pw_scan_global(void *data, uint32_t id, uint32_t permissions,
    const char *type, uint32_t version, const struct spa_dict *props) {
    (void)permissions; (void)version;
    PwLinkScan *scan=data;
    const char *node_name=pw_prop(props,PW_KEY_NODE_NAME);
    const char *port_name=pw_prop(props,"port.name");
    if (node_name && !strcmp(node_name,"tompouce-named") &&
        !strcmp(type,PW_TYPE_INTERFACE_Node)) {
        /* Named stream node: its output ports are gathered below by node id. */
        scan->out_port[0]=id; /* placeholder replaced via port globals */
        scan->out_port[0]=0;
    }
    if (!strcmp(type,PW_TYPE_INTERFACE_Node) && node_name &&
        audio_output_named_count()>0) {
        for (int t=1;t<=audio_output_named_count();t++)
            if (!strcmp(node_name,audio_output_target_name(t))) { scan->node_id=id; break; }
    }
    if (!strcmp(type,PW_TYPE_INTERFACE_Port) && port_name) {
        const char *direction=pw_prop(props,"port.direction");
        const char *dsp=pw_prop(props,"format.dsp");
        int is_audio=dsp && strstr(dsp,"audio");
        const char *node_id_text=pw_prop(props,"node.id");
        uint32_t owner=node_id_text?(uint32_t)strtoul(node_id_text,NULL,10):0;
        if (is_audio && owner==scan->node_id && direction && !strcmp(direction,"in") &&
            scan->n_in<PW_MAX_PORTS) scan->in_port[scan->n_in++]=id;
        if (is_audio && owner && direction && !strcmp(direction,"out")) {
            /* Ports of the named stream: identify by its port alias prefix. */
            const char *alias=pw_prop(props,"port.alias");
            if (alias && !strncmp(alias,"tompouce-named:",15) && scan->n_out<PW_MAX_PORTS)
                scan->out_port[scan->n_out++]=id;
        }
    }
}

static void pw_scan_remove(void *data, uint32_t id) { (void)data; (void)id; }

static void pw_scan_done(void *data, uint32_t id, int seq) {
    PwLinkScan *scan=data;
    if (id==PW_ID_CORE && seq==scan->target_seq) scan->done=1;
}

static void pw_link_named_targets(void) {
    if (audio_output_named_count()<1 || !native_pw.named[1]) return;
    struct pw_core *core=pw_stream_get_core(native_pw.stream);
    struct pw_registry *registry=pw_core_get_registry(core,PW_VERSION_REGISTRY,0);
    if (!registry) return;
    PwLinkScan scan={0};
    static const struct pw_registry_events registry_events={
        .version=PW_VERSION_REGISTRY_EVENTS,.global=pw_scan_global,.global_remove=pw_scan_remove};
    struct spa_hook registry_hook={0};
    static const struct pw_core_events core_events={
        .version=PW_VERSION_CORE_EVENTS,.done=pw_scan_done};
    struct spa_hook core_hook={0};
    pw_registry_add_listener(registry,&registry_hook,&registry_events,&scan);
    pw_core_add_listener(core,&core_hook,&core_events,&scan);
    scan.seq=pw_core_sync(core,PW_ID_CORE,0);
    scan.target_seq=scan.seq;
    struct timespec deadline;
    if (pw_thread_loop_get_time(native_pw.loop,&deadline,2*1000000000LL)>=0)
        while (!scan.done)
            if (pw_thread_loop_timed_wait_full(native_pw.loop,&deadline)<0) break;
    spa_hook_remove(&registry_hook); spa_hook_remove(&core_hook);
    if (!scan.node_id || !scan.n_in || !scan.n_out) {
        log_console("audio: named target ports not found; default stream used");
        pw_core_destroy(core,registry);
        return;
    }
    int pairs=scan.n_in<scan.n_out?scan.n_in:scan.n_out;
    for (int i=0;i<pairs;i++) {
        char out_node[32], in_node[32], out_port[32], in_port[32];
        snprintf(out_node,sizeof(out_node),"%u",PW_ID_ANY);
        snprintf(in_node,sizeof(in_node),"%u",scan.node_id);
        snprintf(out_port,sizeof(out_port),"%u",scan.out_port[i]);
        snprintf(in_port,sizeof(in_port),"%u",scan.in_port[i]);
        struct spa_dict_item items[]={
            {PW_KEY_LINK_OUTPUT_NODE,out_node},{PW_KEY_LINK_OUTPUT_PORT,out_port},
            {PW_KEY_LINK_INPUT_NODE,in_node},{PW_KEY_LINK_INPUT_PORT,in_port},
            {PW_KEY_LINK_PASSIVE,"true"}};
        struct spa_dict dict=SPA_DICT_INIT_ARRAY(items);
        pw_core_create_object(core,"link-factory",PW_TYPE_INTERFACE_Link,PW_VERSION_LINK,&dict,0);
    }
    log_console("audio: linked named output to target node %u (%d ports)",scan.node_id,pairs);
    pw_core_destroy(core,registry);
}

static void pw_state_changed(void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error) {
    (void)data; (void)old;
    atomic_store(&native_pw.active,state==PW_STREAM_STATE_STREAMING);
    if (state==PW_STREAM_STATE_ERROR) {
        native_pw.failed=1;
        snprintf(output_error,sizeof(output_error),"%s",error ? error : "stream failed");
    }
    pw_thread_loop_signal(native_pw.loop,false);
}

static void pw_process(void *data) {
    (void)data;
    /* Test-only stall injection (owned null-sink runs): sleep once inside
       the output thread so the next gap accounting fires for certain. */
    if (!xrun_injected && xrun_inject_ms > 0 && atomic_load(&xrun_calls) >= 10) {
        xrun_injected = 1;
        struct timespec stall = {.tv_sec = xrun_inject_ms/1000,
            .tv_nsec = (xrun_inject_ms%1000)*1000000L};
        while (nanosleep(&stall, &stall) != 0 && errno == EINTR)
            ;
    }
    struct pw_buffer *buffer=pw_stream_dequeue_buffer(native_pw.stream);
    if (!buffer) { atomic_fetch_add(&xrun_empty,1); return; }
    struct spa_buffer *spa=buffer->buffer;
    if (spa && spa->n_datas && spa->datas[0].data && spa->datas[0].chunk) {
        struct spa_data *out=&spa->datas[0];
        uint32_t frames=out->maxsize/OUTPUT_STRIDE;
#if PW_CHECK_VERSION(0,3,50)
        if (buffer->requested && buffer->requested<frames) frames=(uint32_t)buffer->requested;
#endif
        xrun_observe(frames);
        int named=audio_output_named_count();
        for (int t=1;t<=named;t++) out_target_buffers[t]=target_shadow[t];
        /* Cap individual DSP calls, including unusually large server buffers. */
        for (uint32_t at=0;at<frames;) {
            unsigned long count=frames-at;
            if (count>block_frames) count=block_frames;
            render_audio((float *)out->data+at*OUTPUT_CHANNELS,count); at+=(uint32_t)count;
        }
        if (named>0) {
            pthread_mutex_lock(&target_lock);
            for (int t=1;t<=named;t++)
                memcpy(target_ready[t],target_shadow[t],sizeof(target_ready[t]));
            pthread_mutex_unlock(&target_lock);
        }
        out->chunk->offset=0; out->chunk->stride=OUTPUT_STRIDE; out->chunk->size=frames*OUTPUT_STRIDE;
        buffer->size=frames;
    }
    pw_stream_queue_buffer(native_pw.stream,buffer);
}

static const struct pw_stream_events pw_events={
    .version=PW_VERSION_STREAM_EVENTS, .state_changed=pw_state_changed, .process=pw_process
};

/* Named streams never render: they replay the last snapshot produced by the
   default stream's single render pass. */
static void pw_named_process(void *data) {
    int target=(int)(intptr_t)data;
    struct pw_stream *stream=target>=0 && target<=AUDIO_OUTPUT_NAMED_MAX ? native_pw.named[target] : NULL;
    if (!stream) return;
    struct pw_buffer *buffer=pw_stream_dequeue_buffer(stream);
    if (!buffer) return;
    struct spa_buffer *spa=buffer->buffer;
    if (spa && spa->n_datas && spa->datas[0].data && spa->datas[0].chunk) {
        struct spa_data *data_out=&spa->datas[0];
        uint32_t frames=data_out->maxsize/OUTPUT_STRIDE;
        if (buffer->requested && buffer->requested<frames) frames=buffer->requested;
        if (frames>TARGET_FRAMES) frames=TARGET_FRAMES;
        pthread_mutex_lock(&target_lock);
        memcpy(data_out->data,target_ready[target],(size_t)frames*OUTPUT_STRIDE);
        pthread_mutex_unlock(&target_lock);
        data_out->chunk->offset=0; data_out->chunk->stride=OUTPUT_STRIDE;
        data_out->chunk->size=frames*OUTPUT_STRIDE; buffer->size=frames;
    }
    pw_stream_queue_buffer(stream,buffer);
}

static const struct pw_stream_events pw_named_events={
    .version=PW_VERSION_STREAM_EVENTS, .process=pw_named_process
};

static int pipewire_open(void) {
    pw_init(NULL,NULL); native_pw.initialized=1;
    native_pw.loop=pw_thread_loop_new("tompouce-pipewire",NULL);
    if (!native_pw.loop) return 0;
    char latency[48]; snprintf(latency,sizeof(latency),"%lu/%.0f",block_frames,SR);
    struct pw_properties *props=pw_properties_new(PW_KEY_MEDIA_TYPE,"Audio",PW_KEY_MEDIA_CATEGORY,"Playback",
        PW_KEY_MEDIA_ROLE,"Music",PW_KEY_APP_NAME,"Tompouce",PW_KEY_NODE_NAME,"tompouce",
        PW_KEY_NODE_LATENCY,latency,NULL);
    if (!props) return 0;
    /* Owned-test routing only. When set, the stream links to the named target
       sink instead of the default sink; default routing is never changed. */
    const char *target=getenv("TOMPOUCE_OUTPUT_TARGET");
    if (target && *target) pw_properties_set(props,PW_KEY_TARGET_OBJECT,target);
    xrun_inject_ms=0;
    const char *inject=getenv("TOMPOUCE_XRUN_INJECT_MS");
    if (inject && *inject) {
        char *end=NULL;
        long value=strtol(inject,&end,10);
        if (end && !*end && value>0 && value<=5000) xrun_inject_ms=value;
    }
    native_pw.stream=pw_stream_new_simple(pw_thread_loop_get_loop(native_pw.loop),"Tompouce",props,&pw_events,NULL);
    if (!native_pw.stream) return 0;
    uint8_t storage[1024]; struct spa_pod_builder builder=SPA_POD_BUILDER_INIT(storage,sizeof(storage));
    struct spa_audio_info_raw info={.format=SPA_AUDIO_FORMAT_F32,.rate=(uint32_t)SR,.channels=OUTPUT_CHANNELS,
        .position={SPA_AUDIO_CHANNEL_FL,SPA_AUDIO_CHANNEL_FR}};
    const struct spa_pod *format=spa_format_audio_raw_build(&builder,SPA_PARAM_EnumFormat,&info);
    if (!format) return 0;
    pw_thread_loop_lock(native_pw.loop);
    int result=pw_stream_connect(native_pw.stream,PW_DIRECTION_OUTPUT,PW_ID_ANY,
        PW_STREAM_FLAG_AUTOCONNECT|PW_STREAM_FLAG_MAP_BUFFERS|PW_STREAM_FLAG_RT_PROCESS,&format,1);
    /* One extra stream per allowlisted name, with exact target-object matching.
       A stream that cannot connect is destroyed; its voices stay on default. */
    for (int t=1;t<=audio_output_named_count();t++) {
        const char *name=audio_output_target_name(t);
        struct pw_properties *named_props=pw_properties_new(PW_KEY_MEDIA_TYPE,"Audio",
            PW_KEY_MEDIA_CATEGORY,"Playback",PW_KEY_MEDIA_ROLE,"Music",PW_KEY_APP_NAME,"Tompouce",
            PW_KEY_NODE_NAME,"tompouce-named",PW_KEY_TARGET_OBJECT,name,PW_KEY_NODE_LATENCY,latency,NULL);
        if (!named_props) continue;
        native_pw.named[t]=pw_stream_new_simple(pw_thread_loop_get_loop(native_pw.loop),
            "Tompouce",named_props,&pw_named_events,(void *)(intptr_t)t);
        if (!native_pw.named[t]) continue;
        /* No AUTOCONNECT: WirePlumber would attach this stream to the default
           sink before the explicit target links below are created. */
        if (pw_stream_connect(native_pw.named[t],PW_DIRECTION_OUTPUT,PW_ID_ANY,
                PW_STREAM_FLAG_MAP_BUFFERS|PW_STREAM_FLAG_RT_PROCESS,
                &format,1)<0) {
            pw_stream_destroy(native_pw.named[t]); native_pw.named[t]=NULL;
            log_console("audio: named target %s unavailable; voices use the default stream",name);
        }
    }
    if (result>=0) {
        result=pw_thread_loop_start(native_pw.loop);
        if (result>=0) {
            native_pw.started=1;
            struct timespec deadline;
            result=pw_thread_loop_get_time(native_pw.loop,&deadline,CONNECT_SECONDS*1000000000LL);
            while (result>=0 && !native_pw.failed && !atomic_load(&native_pw.active))
                if (pw_thread_loop_timed_wait_full(native_pw.loop,&deadline)<0) break;
            if (atomic_load(&native_pw.active)) pw_link_named_targets();
        }
    }
    if (result<0) snprintf(output_error,sizeof(output_error),"%s",strerror(-result));
    else if (!native_pw.failed && !atomic_load(&native_pw.active)) snprintf(output_error,sizeof(output_error),"no streaming output within %ds",CONNECT_SECONDS);
    pw_thread_loop_unlock(native_pw.loop);
    return atomic_load(&native_pw.active);
}

static void pipewire_close(void) {
    if (native_pw.started) pw_thread_loop_stop(native_pw.loop);
    if (native_pw.loop) pw_thread_loop_lock(native_pw.loop);
    for (int t=1;t<=AUDIO_OUTPUT_NAMED_MAX;t++) {
        if (native_pw.named[t]) pw_stream_destroy(native_pw.named[t]);
        native_pw.named[t]=NULL;
        out_target_buffers[t]=NULL;
    }
    if (native_pw.stream) pw_stream_destroy(native_pw.stream);
    if (native_pw.loop) pw_thread_loop_unlock(native_pw.loop);
    if (native_pw.loop) pw_thread_loop_destroy(native_pw.loop);
    if (native_pw.initialized) pw_deinit();
    native_pw.stream=NULL; native_pw.loop=NULL;
    native_pw.initialized=native_pw.started=native_pw.failed=0; atomic_store(&native_pw.active,0);
}
static int pipewire_active(void) { return atomic_load(&native_pw.active); }
#endif

#ifdef HAVE_PULSE
static struct {
    pa_threaded_mainloop *loop;
    pa_context *context;
    pa_stream *stream;
    int started, timed_out;
    atomic_int active;
} native_pulse;

static void pulse_context_state(pa_context *context, void *data) {
    (void)data;
    if (!PA_CONTEXT_IS_GOOD(pa_context_get_state(context))) atomic_store(&native_pulse.active,0);
    pa_threaded_mainloop_signal(native_pulse.loop,0);
}
static void pulse_stream_state(pa_stream *stream, void *data) {
    (void)data;
    if (!PA_STREAM_IS_GOOD(pa_stream_get_state(stream))) atomic_store(&native_pulse.active,0);
    pa_threaded_mainloop_signal(native_pulse.loop,0);
}
static void pulse_timeout(pa_mainloop_api *api, pa_time_event *event, const struct timeval *time, void *data) {
    (void)api; (void)event; (void)time; (void)data;
    native_pulse.timed_out=1; pa_threaded_mainloop_signal(native_pulse.loop,0);
}
static void pulse_write(pa_stream *stream, size_t bytes, void *data) {
    (void)data;
    for (int burst=0;bytes>=OUTPUT_STRIDE && burst<8;burst++) {
        size_t count=bytes, maximum=block_frames*OUTPUT_STRIDE;
        if (count>maximum) count=maximum;
        void *buffer=NULL;
        if (pa_stream_begin_write(stream,&buffer,&count)<0) break;
        count-=count%OUTPUT_STRIDE;
        if (!buffer || !count) { pa_stream_cancel_write(stream); break; }
        render_audio(buffer,count/OUTPUT_STRIDE);
        if (pa_stream_write(stream,buffer,count,NULL,0,PA_SEEK_RELATIVE)<0) {
            atomic_store(&native_pulse.active,0); break;
        }
        bytes-=count; atomic_store(&native_pulse.active,1);
    }
    pa_threaded_mainloop_signal(native_pulse.loop,0);
}

static int pulse_open(void) {
    native_pulse.loop=pa_threaded_mainloop_new();
    if (!native_pulse.loop) return 0;
    pa_mainloop_api *api=pa_threaded_mainloop_get_api(native_pulse.loop);
    native_pulse.context=pa_context_new(api,"Tompouce");
    if (!native_pulse.context) return 0;
    pa_context_set_state_callback(native_pulse.context,pulse_context_state,NULL);
    if (pa_context_connect(native_pulse.context,NULL,PA_CONTEXT_NOAUTOSPAWN,NULL)<0) {
        snprintf(output_error,sizeof(output_error),"%s",pa_strerror(pa_context_errno(native_pulse.context))); return 0;
    }
    if (pa_threaded_mainloop_start(native_pulse.loop)<0) return 0;
    native_pulse.started=1; pa_threaded_mainloop_lock(native_pulse.loop);
    struct timeval time; pa_gettimeofday(&time); pa_timeval_add(&time,CONNECT_SECONDS*PA_USEC_PER_SEC);
    pa_time_event *timer=api->time_new(api,&time,pulse_timeout,NULL);
    if (!timer) { pa_threaded_mainloop_unlock(native_pulse.loop); return 0; }
    while (!native_pulse.timed_out && PA_CONTEXT_IS_GOOD(pa_context_get_state(native_pulse.context)) &&
           pa_context_get_state(native_pulse.context)!=PA_CONTEXT_READY) pa_threaded_mainloop_wait(native_pulse.loop);
    if (!native_pulse.timed_out && pa_context_get_state(native_pulse.context)==PA_CONTEXT_READY) {
        pa_sample_spec spec={.format=PA_SAMPLE_FLOAT32NE,.rate=(uint32_t)SR,.channels=OUTPUT_CHANNELS};
        native_pulse.stream=pa_stream_new(native_pulse.context,"Music",&spec,NULL);
        if (native_pulse.stream) {
            pa_stream_set_state_callback(native_pulse.stream,pulse_stream_state,NULL);
            pa_stream_set_write_callback(native_pulse.stream,pulse_write,NULL);
            uint32_t block=(uint32_t)(block_frames*OUTPUT_STRIDE);
            pa_buffer_attr attr={.maxlength=UINT32_MAX,.tlength=block*3,.prebuf=0,.minreq=block,.fragsize=UINT32_MAX};
            if (pa_stream_connect_playback(native_pulse.stream,NULL,&attr,PA_STREAM_ADJUST_LATENCY,NULL,NULL)>=0)
                while (!native_pulse.timed_out && PA_CONTEXT_IS_GOOD(pa_context_get_state(native_pulse.context)) &&
                       PA_STREAM_IS_GOOD(pa_stream_get_state(native_pulse.stream)) && !atomic_load(&native_pulse.active))
                    pa_threaded_mainloop_wait(native_pulse.loop);
        }
    }
    api->time_free(timer);
    if (!atomic_load(&native_pulse.active)) snprintf(output_error,sizeof(output_error),"%s",
        native_pulse.timed_out ? "connection timed out" : pa_strerror(pa_context_errno(native_pulse.context)));
    pa_threaded_mainloop_unlock(native_pulse.loop);
    return atomic_load(&native_pulse.active);
}

static void pulse_close(void) {
    if (native_pulse.started) pa_threaded_mainloop_stop(native_pulse.loop);
    if (native_pulse.stream) {
        pa_stream_set_state_callback(native_pulse.stream,NULL,NULL); pa_stream_set_write_callback(native_pulse.stream,NULL,NULL);
        pa_stream_disconnect(native_pulse.stream); pa_stream_unref(native_pulse.stream);
    }
    if (native_pulse.context) {
        pa_context_set_state_callback(native_pulse.context,NULL,NULL); pa_context_disconnect(native_pulse.context); pa_context_unref(native_pulse.context);
    }
    if (native_pulse.loop) pa_threaded_mainloop_free(native_pulse.loop);
    native_pulse.stream=NULL; native_pulse.context=NULL; native_pulse.loop=NULL;
    native_pulse.started=native_pulse.timed_out=0; atomic_store(&native_pulse.active,0);
}
static int pulse_active(void) { return atomic_load(&native_pulse.active); }
#endif

static PaStream *pa_output;
static int pa_initialized;
#ifdef HAVE_SAMPLERATE
static SRC_STATE *rate_converter;
static float rate_input[OUTPUT_FRAMES*OUTPUT_CHANNELS];
static long rate_at, rate_count;
static double output_ratio;
#endif

static void output_render(float *out, unsigned long frames) {
#ifdef HAVE_SAMPLERATE
    if (rate_converter) {
        while (frames) {
            if (rate_at==rate_count) { render_audio(rate_input,block_frames); rate_at=0; rate_count=(long)block_frames; }
            SRC_DATA data={.data_in=rate_input+rate_at*OUTPUT_CHANNELS,.data_out=out,
                .input_frames=rate_count-rate_at,.output_frames=(long)(frames>OUTPUT_FRAMES ? OUTPUT_FRAMES : frames),.src_ratio=output_ratio};
            if (src_process(rate_converter,&data) || (!data.input_frames_used && !data.output_frames_gen)) {
                memset(out,0,frames*OUTPUT_STRIDE); return;
            }
            rate_at+=data.input_frames_used;
            out+=data.output_frames_gen*OUTPUT_CHANNELS; frames-=(unsigned long)data.output_frames_gen;
        }
        return;
    }
#endif
    while (frames) {
        unsigned long count=frames>block_frames ? block_frames : frames;
        render_audio(out,count); frames-=count; out+=count*OUTPUT_CHANNELS;
    }
}
static int output_pa_callback(const void *input, void *output, unsigned long frames,
                              const PaStreamCallbackTimeInfo *time, PaStreamCallbackFlags flags, void *data) {
    (void)input; (void)time; (void)flags; (void)data;
    output_render(output,frames); return paContinue;
}
static int portaudio_open(PaHostApiTypeId type) {
    PaError error=Pa_Initialize();
    if (error!=paNoError) { snprintf(output_error,sizeof(output_error),"%s",Pa_GetErrorText(error)); return 0; }
    pa_initialized=1;
    PaHostApiIndex index=Pa_HostApiTypeIdToHostApiIndex(type);
    const PaHostApiInfo *host=index>=0 ? Pa_GetHostApiInfo(index) : NULL;
    const PaDeviceInfo *device=host && host->defaultOutputDevice!=paNoDevice ? Pa_GetDeviceInfo(host->defaultOutputDevice) : NULL;
    if (!device || device->maxOutputChannels<OUTPUT_CHANNELS) { snprintf(output_error,sizeof(output_error),"no stereo output device"); return 0; }
    PaStreamParameters output={.device=host->defaultOutputDevice,.channelCount=OUTPUT_CHANNELS,
        .sampleFormat=paFloat32,.suggestedLatency=.02};
    double rate=SR;
    error=Pa_IsFormatSupported(NULL,&output,rate);
#ifdef HAVE_SAMPLERATE
    if (error==paInvalidSampleRate && isfinite(device->defaultSampleRate) && device->defaultSampleRate>=8000 && device->defaultSampleRate<=384000) {
        rate=device->defaultSampleRate;
        error=Pa_IsFormatSupported(NULL,&output,rate);
        if (error==paNoError) {
            int conversion_error=0;
            rate_converter=src_new(SRC_SINC_FASTEST,OUTPUT_CHANNELS,&conversion_error);
            if (!rate_converter) { snprintf(output_error,sizeof(output_error),"resampler: %s",src_strerror(conversion_error)); return 0; }
            output_ratio=rate/SR; rate_at=rate_count=0;
            if (src_set_ratio(rate_converter,output_ratio)) return 0;
        }
    }
#endif
    if (error==paNoError) error=Pa_OpenStream(&pa_output,NULL,&output,rate,block_frames,paClipOff,output_pa_callback,NULL);
    if (error==paNoError) error=Pa_StartStream(pa_output);
    if (error==paNoError && Pa_IsStreamActive(pa_output)==1) return 1;
    snprintf(output_error,sizeof(output_error),"%s",error==paNoError ? "stream did not start" : Pa_GetErrorText(error)); return 0;
}
static int jack_open(void) { return portaudio_open(paJACK); }
static int alsa_open(void) { return portaudio_open(paALSA); }
static void portaudio_close(void) {
    if (pa_output) { Pa_AbortStream(pa_output); Pa_CloseStream(pa_output); pa_output=NULL; }
    if (pa_initialized) { Pa_Terminate(); pa_initialized=0; }
#ifdef HAVE_SAMPLERATE
    if (rate_converter) rate_converter=src_delete(rate_converter);
    rate_at=rate_count=0;
#endif
}
static int portaudio_active(void) { return pa_output && Pa_IsStreamActive(pa_output)==1; }

static const OutputDriver output_drivers[]={
#ifdef HAVE_PIPEWIRE
    {"PipeWire",pipewire_open,pipewire_close,pipewire_active},
#endif
#ifdef HAVE_PULSE
    {"PulseAudio",pulse_open,pulse_close,pulse_active},
#endif
    {"JACK",jack_open,portaudio_close,portaudio_active},
    {"ALSA",alsa_open,portaudio_close,portaudio_active}
};

static int select_output(const OutputDriver *drivers, size_t count) {
    for (size_t i=0;i<count;i++) {
        snprintf(output_error,sizeof(output_error),"initialization failed");
        if (drivers[i].open()) {
            selected=&drivers[i]; log_console("audio: %s output ready (%.0f Hz stereo engine)",selected->name,SR); return 1;
        }
        drivers[i].close(); log_console("audio: %s unavailable: %s",drivers[i].name,output_error);
    }
    log_console("audio: no output backend available"); return 0;
}

int audio_output_start(AudioOutputRender render, unsigned long frames) {
    if (selected) return selected->active();
    if (!render || frames<64 || frames>OUTPUT_FRAMES) return 0;
    render_audio=render; block_frames=frames;
    return select_output(output_drivers,sizeof(output_drivers)/sizeof(*output_drivers));
}
void audio_output_stop(void) {
    if (selected) selected->close();
    selected=NULL; render_audio=NULL;
}
int audio_output_active(void) { return selected ? selected->active() : 0; }
const char *audio_output_name(void) { return selected ? selected->name : "none"; }
