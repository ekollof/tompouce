#include "gm_synth.h"
#include "stretch.h"
#include <sys/stat.h>
#include <unistd.h>
#ifdef HAVE_FLUIDSYNTH
#include <fluidsynth.h>
#endif

/* Strudel 1.2.6 gm_* names mapped to standard zero-based GM programs.
 * Its piano family combines programs 0..3 under gm_piano. Native playback
 * uses one installed SF2 voice per name, rather than browser font variants. */
static const char *names[]={
    "gm_piano","gm_epiano1","gm_epiano2","gm_harpsichord","gm_clavinet",
    "gm_celesta","gm_glockenspiel","gm_music_box","gm_vibraphone","gm_marimba",
    "gm_xylophone","gm_tubular_bells","gm_dulcimer","gm_drawbar_organ","gm_percussive_organ",
    "gm_rock_organ","gm_church_organ","gm_reed_organ","gm_accordion","gm_harmonica","gm_bandoneon",
    "gm_acoustic_guitar_nylon","gm_acoustic_guitar_steel","gm_electric_guitar_jazz","gm_electric_guitar_clean",
    "gm_electric_guitar_muted","gm_overdriven_guitar","gm_distortion_guitar","gm_guitar_harmonics",
    "gm_acoustic_bass","gm_electric_bass_finger","gm_electric_bass_pick","gm_fretless_bass",
    "gm_slap_bass_1","gm_slap_bass_2","gm_synth_bass_1","gm_synth_bass_2",
    "gm_violin","gm_viola","gm_cello","gm_contrabass","gm_tremolo_strings","gm_pizzicato_strings",
    "gm_orchestral_harp","gm_timpani","gm_string_ensemble_1","gm_string_ensemble_2",
    "gm_synth_strings_1","gm_synth_strings_2","gm_choir_aahs","gm_voice_oohs","gm_synth_choir","gm_orchestra_hit",
    "gm_trumpet","gm_trombone","gm_tuba","gm_muted_trumpet","gm_french_horn","gm_brass_section",
    "gm_synth_brass_1","gm_synth_brass_2","gm_soprano_sax","gm_alto_sax","gm_tenor_sax","gm_baritone_sax",
    "gm_oboe","gm_english_horn","gm_bassoon","gm_clarinet","gm_piccolo","gm_flute","gm_recorder",
    "gm_pan_flute","gm_blown_bottle","gm_shakuhachi","gm_whistle","gm_ocarina",
    "gm_lead_1_square","gm_lead_2_sawtooth","gm_lead_3_calliope","gm_lead_4_chiff","gm_lead_5_charang",
    "gm_lead_6_voice","gm_lead_7_fifths","gm_lead_8_bass_lead","gm_pad_new_age","gm_pad_warm",
    "gm_pad_poly","gm_pad_choir","gm_pad_bowed","gm_pad_metallic","gm_pad_halo","gm_pad_sweep",
    "gm_fx_rain","gm_fx_soundtrack","gm_fx_crystal","gm_fx_atmosphere","gm_fx_brightness","gm_fx_goblins",
    "gm_fx_echoes","gm_fx_sci_fi","gm_sitar","gm_banjo","gm_shamisen","gm_koto","gm_kalimba",
    "gm_bagpipe","gm_fiddle","gm_shanai","gm_tinkle_bell","gm_agogo","gm_steel_drums","gm_woodblock",
    "gm_taiko_drum","gm_melodic_tom","gm_synth_drum","gm_reverse_cymbal","gm_guitar_fret_noise",
    "gm_breath_noise","gm_seashore","gm_bird_tweet","gm_telephone","gm_helicopter","gm_applause","gm_gunshot"
};
int gm_synth_count(void) { return (int)(sizeof(names)/sizeof(*names)); }
const char *gm_synth_name(int index) { return index>=0 && index<gm_synth_count()?names[index]:NULL; }
int gm_synth_program(const char *name) {
    if (!name) return -1;
    for (int i=0;i<gm_synth_count();i++) if (!strcmp(name,names[i])) return i?i+3:0;
    if (!strcmp(name,"gm_acoustic_grand_piano")) return 0;
    if (!strcmp(name,"gm_bright_acoustic_piano")) return 1;
    if (!strcmp(name,"gm_electric_grand_piano")) return 2;
    if (!strcmp(name,"gm_honky_tonk_piano")) return 3;
    return -1;
}

#ifdef HAVE_FLUIDSYNTH
static char soundfont[4096];
static pthread_once_t font_once=PTHREAD_ONCE_INIT;
static pthread_mutex_t gm_lock=PTHREAD_MUTEX_INITIALIZER;
static fluid_synth_t *synth;
static fluid_settings_t *settings;
static int font_id=-1, attempted;
static void gm_log(int level, const char *message, void *data) {
    (void)level; (void)data;
    if (message) log_console("GM: %.200s",message);
}
static void find_font(void) {
    const char *explicit=getenv("TOMPOUCE_SOUNDFONT");
    const char *paths[]={explicit,"/usr/share/soundfonts/FluidR3_GM.sf2","/usr/share/sounds/sf2/FluidR3_GM.sf2",
        "/usr/share/soundfonts/default.sf2","/usr/share/sounds/sf2/default-GM.sf2","/usr/share/soundfonts/FluidR3_GM.sf3"};
    for (unsigned i=0;i<sizeof(paths)/sizeof(*paths);i++) {
        if (!paths[i] || !*paths[i]) continue;
        struct stat st;
        if (strlen(paths[i])<sizeof(soundfont) && !stat(paths[i],&st) && S_ISREG(st.st_mode) &&
            st.st_size>0 && st.st_size<=512*1024*1024 && !access(paths[i],R_OK)) {
            snprintf(soundfont,sizeof(soundfont),"%s",paths[i]); return;
        }
        if (i==0 && explicit && *explicit) return;
    }
}
static void gm_cleanup(void) {
    if (synth) delete_fluid_synth(synth);
    if (settings) delete_fluid_settings(settings);
    synth=NULL; settings=NULL;
}
static int gm_start(void) {
    if (attempted) return synth!=NULL;
    attempted=1;
    /* We render PCM directly and use Tompouce's output backends. Disable
       FluidSynth device probes before its first settings/synth instance:
       its SDL3 probe leaks a mutex when SDL audio has not been initialized.
       Registration is process-global; gm_lock serializes all our FluidSynth
       calls, and attempted keeps it ahead of instance creation. */
    const char *drivers[]={NULL};
    if (fluid_audio_driver_register(drivers)!=FLUID_OK) {
        log_console("GM: cannot disable unused FluidSynth audio drivers"); return 0;
    }
    fluid_set_log_function(FLUID_PANIC,gm_log,NULL);
    fluid_set_log_function(FLUID_ERR,gm_log,NULL);
    fluid_set_log_function(FLUID_WARN,gm_log,NULL);
    settings=new_fluid_settings(); if (!settings) return 0;
    fluid_settings_setnum(settings,"synth.sample-rate",SR);
    fluid_settings_setnum(settings,"synth.gain",.2);
    fluid_settings_setint(settings,"synth.cpu-cores",1);
    fluid_settings_setint(settings,"synth.threadsafe-api",0);
    fluid_settings_setint(settings,"synth.reverb.active",0);
    fluid_settings_setint(settings,"synth.chorus.active",0);
    synth=new_fluid_synth(settings);
    if (!synth || (font_id=fluid_synth_sfload(synth,soundfont,0))<0 || atexit(gm_cleanup)) {
        gm_cleanup(); return 0;
    }
    fluid_synth_pitch_wheel_sens(synth,0,96);
    return 1;
}
#endif

int gm_synth_available(void) {
#ifdef HAVE_FLUIDSYNTH
    pthread_once(&font_once,find_font); return soundfont[0]!=0;
#else
    return 0;
#endif
}

int gm_synth_prepare(AudioEvent *event) {
    if (!event || gm_synth_program(event->token)<0) return 0;
#ifdef HAVE_FLUIDSYNTH
    if (!gm_synth_available()) { log_console("GM: no readable soundfont (TOMPOUCE_SOUNDFONT)"); return 0; }
    double freq=event->freq>0?event->freq:440*pow(2,(36-69)/12.0);
    freq*=event->stretch>0?event->stretch:1;
    double speed=event->smp_speed;
    if (!isfinite(event->att) || event->att<0 || !isfinite(event->dec) || event->dec<0 ||
        !isfinite(event->sus) || event->sus<0 || event->sus>1 ||
        !isfinite(speed) || !speed || !isfinite(freq) || freq<=0 || !isfinite(event->duration) || event->duration<=0 ||
        !isfinite(event->rel) || event->rel<0 || event->duration+event->rel>60) return 0;
    freq*=fabs(speed);
    if (event->freq<=0) event->freq=440*pow(2,(36-69)/12.0);
    int program=gm_synth_program(event->token);
    char key[768]; int used=snprintf(key,sizeof(key),"gm:%d:%a:%a:%a:%a:%a:%a:%a",program,freq,event->duration,
        event->att,event->dec,event->sus,event->rel,speed);
    const int controls[]={DSP_PENV,DSP_PATT,DSP_PDEC,DSP_PSUS,DSP_PREL,DSP_PANCHOR,DSP_PCURVE,DSP_VIB,DSP_VIBMOD};
    for (unsigned i=0;i<sizeof(controls)/sizeof(*controls);i++) {
        if (!isfinite(event->dsp.p[controls[i]])) return 0;
        if (used<0 || (size_t)used>=sizeof(key)) return 0;
        int n=snprintf(key+used,sizeof(key)-(size_t)used,":%a",event->dsp.p[controls[i]]);
        if (n<0) return 0;
        used+=n;
    }
    if ((size_t)used>=sizeof(key)) return 0;
    const StretchBuffer *cached=stretch_pcm_lookup(key);
    if (cached) { event->stretch_buffer=cached; event->is_drum=0; return 1; }
    int frames=(int)ceil((event->duration+event->rel)*SR);
    int rounded=(frames+63)&~63;
    float *pcm=calloc((size_t)rounded*2,sizeof(float)); if (!pcm) return 0;
    pthread_mutex_lock(&gm_lock);
    if (!gm_start() || fluid_synth_program_select(synth,0,font_id,0,program)!=FLUID_OK) {
        pthread_mutex_unlock(&gm_lock); free(pcm); log_console("GM: cannot load the selected soundfont/program"); return 0;
    }
    fluid_synth_all_sounds_off(synth,0);
    float silence[128]; fluid_synth_write_float(synth,64,silence,0,2,silence,1,2);
    double midi=69+12*log2(freq/440);
    int note=(int)fmax(0,fmin(127,round(midi)));
    int bend=(int)fmax(0,fmin(16383,8192+(midi-note)*8192/96));
    fluid_synth_pitch_bend(synth,0,bend);
    if (fluid_synth_noteon(synth,0,note,100)!=FLUID_OK) { pthread_mutex_unlock(&gm_lock); free(pcm); return 0; }
    int released=0, ok=1;
    for (int at=0;at<rounded;at+=64) {
        double age=at/SR;
        if (!released && age>=event->duration) { fluid_synth_noteoff(synth,0,note); released=1; }
        double mod=dsp_pitch_multiplier(&event->dsp,age,event->duration);
        double pitch=midi+12*log2(fmax(1e-6,mod));
        bend=(int)fmax(0,fmin(16383,8192+(pitch-note)*8192/96));
        fluid_synth_pitch_bend(synth,0,bend);
        if (fluid_synth_write_float(synth,64,pcm+at*2,0,2,pcm+at*2,1,2)!=FLUID_OK) { ok=0; break; }
    }
    fluid_synth_all_sounds_off(synth,0);
    pthread_mutex_unlock(&gm_lock);
    if (!ok) { free(pcm); return 0; }
    if (speed<0) for (int i=0;i<frames/2;i++) for (int c=0;c<2;c++) {
        float swap=pcm[i*2+c]; pcm[i*2+c]=pcm[(frames-1-i)*2+c]; pcm[(frames-1-i)*2+c]=swap;
    }
    for (int i=0;i<frames;i++) {
        double env=dsp_envelope(i/SR,event->duration,event->att,event->dec,event->sus,event->rel);
        pcm[i*2]*=(float)env; pcm[i*2+1]*=(float)env;
    }
    event->stretch_buffer=stretch_pcm_store(key,pcm,frames); event->is_drum=0;
    if (!event->stretch_buffer) log_console("GM: prepared PCM cache limit exceeded");
    return event->stretch_buffer!=NULL;
#else
    log_console("GM: rebuild with FluidSynth support"); return 0;
#endif
}
