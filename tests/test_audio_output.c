/* Deterministic backend failures; no sound server/device is opened. */
#define Pa_Initialize fake_pa_initialize
#define Pa_Terminate fake_pa_terminate
#define Pa_HostApiTypeIdToHostApiIndex fake_pa_host_index
#define Pa_GetHostApiInfo fake_pa_host_info
#define Pa_GetDeviceInfo fake_pa_device_info
#define Pa_IsFormatSupported fake_pa_format
#define Pa_OpenStream fake_pa_open
#define Pa_StartStream fake_pa_start
#define Pa_AbortStream fake_pa_abort
#define Pa_CloseStream fake_pa_close
#define Pa_IsStreamActive fake_pa_active
#define Pa_GetErrorText fake_pa_error
#include "../src/audio_output.c"
#include <assert.h>

static int init_error, open_error, start_error, format_error, initialized_count, terminated_count, closed_count;
static int has_device=1, channels=2, last_host;
static double default_rate=48000, opened_rate;
static int stream_cookie;
PaError fake_pa_initialize(void) { initialized_count++; return init_error; }
PaError fake_pa_terminate(void) { terminated_count++; return paNoError; }
PaHostApiIndex fake_pa_host_index(PaHostApiTypeId type) { last_host=type; return 0; }
const PaHostApiInfo *fake_pa_host_info(PaHostApiIndex index) {
    (void)index; static PaHostApiInfo host;
    host.defaultOutputDevice=has_device ? 0 : paNoDevice; return &host;
}
const PaDeviceInfo *fake_pa_device_info(PaDeviceIndex index) {
    (void)index; static PaDeviceInfo device;
    device.maxOutputChannels=channels; device.defaultSampleRate=default_rate; return &device;
}
PaError fake_pa_format(const PaStreamParameters *input, const PaStreamParameters *output, double rate) {
    (void)input; assert(output->channelCount==2 && output->sampleFormat==paFloat32);
    return rate==SR ? format_error : paNoError;
}
PaError fake_pa_open(PaStream **stream, const PaStreamParameters *input, const PaStreamParameters *output,
                    double rate, unsigned long frames, PaStreamFlags flags, PaStreamCallback *callback, void *data) {
    (void)input; (void)output; (void)frames; (void)flags; (void)callback; (void)data;
    opened_rate=rate;
    if (!open_error) *stream=(PaStream *)&stream_cookie;
    return open_error;
}
PaError fake_pa_start(PaStream *stream) { assert(stream); return start_error; }
PaError fake_pa_abort(PaStream *stream) { assert(stream); return paNoError; }
PaError fake_pa_close(PaStream *stream) { assert(stream); closed_count++; return paNoError; }
PaError fake_pa_active(PaStream *stream) { return stream ? 1 : 0; }
const char *fake_pa_error(PaError error) { (void)error; return "test failure"; }

static int attempts[4], closes[4], succeeded=-1;
static int open_0(void) { attempts[0]++; return succeeded==0; }
static int open_1(void) { attempts[1]++; return succeeded==1; }
static int open_2(void) { attempts[2]++; return succeeded==2; }
static int open_3(void) { attempts[3]++; return succeeded==3; }
static void close_0(void) { closes[0]++; }
static void close_1(void) { closes[1]++; }
static void close_2(void) { closes[2]++; }
static void close_3(void) { closes[3]++; }
static int is_active(void) { return 1; }
static const OutputDriver fake_drivers[]={
    {"PipeWire",open_0,close_0,is_active}, {"PulseAudio",open_1,close_1,is_active},
    {"JACK",open_2,close_2,is_active}, {"ALSA",open_3,close_3,is_active}
};

static unsigned long rendered;
static void sine_render(float *out, unsigned long frames) {
    for (unsigned long i=0;i<frames;i++,rendered++) {
        float sample=(float)(.25*sin(2*3.141592653589793*440*rendered/SR));
        out[i*2]=sample; out[i*2+1]=-sample;
    }
}

int main(void) {
    size_t n=sizeof(output_drivers)/sizeof(*output_drivers);
    assert(!strcmp(output_drivers[n-2].name,"JACK") && !strcmp(output_drivers[n-1].name,"ALSA"));
#ifdef HAVE_PIPEWIRE
    assert(!strcmp(output_drivers[0].name,"PipeWire"));
#endif
    for (int winner=0;winner<4;winner++) {
        succeeded=winner; memset(attempts,0,sizeof(attempts)); memset(closes,0,sizeof(closes));
        assert(select_output(fake_drivers,4)); assert(!strcmp(audio_output_name(),fake_drivers[winner].name));
        for (int i=0;i<4;i++) { assert(attempts[i]==(i<=winner)); assert(closes[i]==(i<winner)); }
        assert(audio_output_active()); audio_output_stop(); assert(closes[winner]==1 && !audio_output_active());
    }
    succeeded=-1; memset(closes,0,sizeof(closes));
    assert(!select_output(fake_drivers,4) && !strcmp(audio_output_name(),"none"));
    for (int i=0;i<4;i++) assert(closes[i]==1);
    audio_output_stop(); audio_output_stop();

    block_frames=256; render_audio=sine_render;
    init_error=paUnanticipatedHostError; int before=terminated_count;
    assert(!jack_open()); portaudio_close(); assert(terminated_count==before);
    init_error=0; has_device=0; assert(!jack_open()); portaudio_close();
    has_device=1; channels=1; assert(!alsa_open()); portaudio_close();
    channels=2; open_error=paDeviceUnavailable; assert(!alsa_open()); portaudio_close();
    open_error=0; start_error=paDeviceUnavailable; before=closed_count;
    assert(!jack_open()); portaudio_close(); assert(closed_count==before+1 && last_host==paJACK);
    start_error=0; assert(alsa_open() && opened_rate==SR && last_host==paALSA); portaudio_close();
    assert(initialized_count==terminated_count+1); /* One initialization failed. */
#ifdef HAVE_SAMPLERATE
    format_error=paInvalidSampleRate;
    assert(jack_open() && opened_rate==48000 && rate_converter);
    float out[1024*2]; unsigned long total=0; double energy=0; int crossings=0; float previous=0;
    rendered=0;
    while (total<48000) {
        unsigned long frames=total%2 ? 17 : 511;
        if (frames>48000-total) frames=48000-total;
        output_render(out,frames);
        for (unsigned long i=0;i<frames;i++) {
            assert(isfinite(out[i*2]) && fabs(out[i*2]+out[i*2+1])<1e-6);
            if (total+i>=2048) {
                energy+=out[i*2]*out[i*2];
                if (previous<=0 && out[i*2]>0) crossings++;
            }
            previous=out[i*2];
        }
        total+=frames;
    }
    assert(fabs(sqrt(energy/(48000-2048))-.25/sqrt(2))<.002);
    assert(fabs(crossings/(1.0-2048.0/48000)-440)<2);
    assert(fabs(rendered/SR-1)<.02);
    portaudio_close(); assert(!rate_converter);
#else
    format_error=paInvalidSampleRate;
    assert(!jack_open()); portaudio_close();
#endif
    puts("audio output priority, failure cleanup, unavailable devices and sample-rate conversion ok");
    return 0;
}
