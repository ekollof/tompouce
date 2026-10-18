#define _POSIX_C_SOURCE 200809L
#include "audio_output.h"
#include "common.h"
#include <portaudio.h>
#include <stdatomic.h>
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
static struct {
    struct pw_thread_loop *loop;
    struct pw_stream *stream;
    int initialized, started, failed;
    atomic_int active;
} native_pw;

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
    struct pw_buffer *buffer=pw_stream_dequeue_buffer(native_pw.stream);
    if (!buffer) return;
    struct spa_buffer *spa=buffer->buffer;
    if (spa && spa->n_datas && spa->datas[0].data && spa->datas[0].chunk) {
        struct spa_data *out=&spa->datas[0];
        uint32_t frames=out->maxsize/OUTPUT_STRIDE;
#if PW_CHECK_VERSION(0,3,50)
        if (buffer->requested && buffer->requested<frames) frames=(uint32_t)buffer->requested;
#endif
        /* Cap individual DSP calls, including unusually large server buffers. */
        for (uint32_t at=0;at<frames;) {
            unsigned long count=frames-at;
            if (count>block_frames) count=block_frames;
            render_audio((float *)out->data+at*OUTPUT_CHANNELS,count); at+=(uint32_t)count;
        }
        out->chunk->offset=0; out->chunk->stride=OUTPUT_STRIDE; out->chunk->size=frames*OUTPUT_STRIDE;
        buffer->size=frames;
    }
    pw_stream_queue_buffer(native_pw.stream,buffer);
}

static const struct pw_stream_events pw_events={
    .version=PW_VERSION_STREAM_EVENTS, .state_changed=pw_state_changed, .process=pw_process
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
    if (result>=0) {
        result=pw_thread_loop_start(native_pw.loop);
        if (result>=0) {
            native_pw.started=1;
            struct timespec deadline;
            result=pw_thread_loop_get_time(native_pw.loop,&deadline,CONNECT_SECONDS*1000000000LL);
            while (result>=0 && !native_pw.failed && !atomic_load(&native_pw.active))
                if (pw_thread_loop_timed_wait_full(native_pw.loop,&deadline)<0) break;
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
