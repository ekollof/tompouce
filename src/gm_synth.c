#include "gm_synth.h"
#include "stretch.h"
#include "sampler.h"
#include "websamp.h"
#include "banks.h"
#include "common.h"
#include "dsp.h"
#define MINIMP3_FLOAT_OUTPUT
#include "minimp3.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
/* Pinned first-font pack per gm_* name, extracted from the Strudel web
   deployment (same webaudiofontdata repo as scripts/gm-webaudiofont.lock.json).
   The web selects fonts[0] for the default sample index; the native path
   plays that same pack. Piano aliases resolve through gm_piano. */
static const struct { const char *name; const char *font; } gm_fonts[]={
    {"gm_accordion","0210_JCLive_sf2_file"},
    {"gm_acoustic_bass","0320_JCLive_sf2_file"},
    {"gm_acoustic_guitar_nylon","0240_JCLive_sf2_file"},
    {"gm_acoustic_guitar_steel","0253_Acoustic_Guitar_sf2_file"},
    {"gm_agogo","1130_JCLive_sf2_file"},
    {"gm_alto_sax","0650_JCLive_sf2_file"},
    {"gm_applause","1260_JCLive_sf2_file"},
    {"gm_bagpipe","1090_Aspirin_sf2_file"},
    {"gm_bandoneon","0230_Aspirin_sf2_file"},
    {"gm_banjo","1050_FluidR3_GM_sf2_file"},
    {"gm_baritone_sax","0670_JCLive_sf2_file"},
    {"gm_bassoon","0700_JCLive_sf2_file"},
    {"gm_bird_tweet","1230_FluidR3_GM_sf2_file"},
    {"gm_blown_bottle","0760_FluidR3_GM_sf2_file"},
    {"gm_brass_section","0610_JCLive_sf2_file"},
    {"gm_breath_noise","1210_FluidR3_GM_sf2_file"},
    {"gm_celesta","0080_JCLive_sf2_file"},
    {"gm_cello","0420_Aspirin_sf2_file"},
    {"gm_choir_aahs","0520_Aspirin_sf2_file"},
    {"gm_church_organ","0190_JCLive_sf2_file"},
    {"gm_clarinet","0710_JCLive_sf2_file"},
    {"gm_clavinet","0070_JCLive_sf2_file"},
    {"gm_contrabass","0430_Aspirin_sf2_file"},
    {"gm_distortion_guitar","0300_FluidR3_GM_sf2_file"},
    {"gm_drawbar_organ","0160_JCLive_sf2_file"},
    {"gm_dulcimer","0150_Aspirin_sf2_file"},
    {"gm_electric_bass_finger","0330_JCLive_sf2_file"},
    {"gm_electric_bass_pick","0340_JCLive_sf2_file"},
    {"gm_electric_guitar_clean","0270_Aspirin_sf2_file"},
    {"gm_electric_guitar_jazz","0260_JCLive_sf2_file"},
    {"gm_electric_guitar_muted","0280_Aspirin_sf2_file"},
    {"gm_english_horn","0690_JCLive_sf2_file"},
    {"gm_epiano1","0040_JCLive_sf2_file"},
    {"gm_epiano2","0050_JCLive_sf2_file"},
    {"gm_fiddle","1100_JCLive_sf2_file"},
    {"gm_flute","0730_JCLive_sf2_file"},
    {"gm_french_horn","0600_Aspirin_sf2_file"},
    {"gm_fretless_bass","0350_Aspirin_sf2_file"},
    {"gm_fx_atmosphere","0990_JCLive_sf2_file"},
    {"gm_fx_brightness","1000_JCLive_sf2_file"},
    {"gm_fx_crystal","0980_Aspirin_sf2_file"},
    {"gm_fx_echoes","1020_FluidR3_GM_sf2_file"},
    {"gm_fx_goblins","1010_FluidR3_GM_sf2_file"},
    {"gm_fx_rain","0960_FluidR3_GM_sf2_file"},
    {"gm_fx_sci_fi","1030_FluidR3_GM_sf2_file"},
    {"gm_fx_soundtrack","0970_FluidR3_GM_sf2_file"},
    {"gm_glockenspiel","0090_JCLive_sf2_file"},
    {"gm_guitar_fret_noise","1200_JCLive_sf2_file"},
    {"gm_guitar_harmonics","0310_Aspirin_sf2_file"},
    {"gm_gunshot","1270_JCLive_sf2_file"},
    {"gm_harmonica","0220_FluidR3_GM_sf2_file"},
    {"gm_harpsichord","0060_JCLive_sf2_file"},
    {"gm_helicopter","1250_JCLive_sf2_file"},
    {"gm_kalimba","1080_JCLive_sf2_file"},
    {"gm_koto","1070_FluidR3_GM_sf2_file"},
    {"gm_lead_1_square","0800_Aspirin_sf2_file"},
    {"gm_lead_2_sawtooth","0810_JCLive_sf2_file"},
    {"gm_lead_3_calliope","0820_JCLive_sf2_file"},
    {"gm_lead_4_chiff","0830_JCLive_sf2_file"},
    {"gm_lead_5_charang","0840_JCLive_sf2_file"},
    {"gm_lead_6_voice","0850_JCLive_sf2_file"},
    {"gm_lead_7_fifths","0860_JCLive_sf2_file"},
    {"gm_lead_8_bass_lead","0870_JCLive_sf2_file"},
    {"gm_marimba","0120_JCLive_sf2_file"},
    {"gm_melodic_tom","1170_JCLive_sf2_file"},
    {"gm_music_box","0100_JCLive_sf2_file"},
    {"gm_muted_trumpet","0590_JCLive_sf2_file"},
    {"gm_oboe","0680_JCLive_sf2_file"},
    {"gm_ocarina","0790_FluidR3_GM_sf2_file"},
    {"gm_orchestra_hit","0550_Aspirin_sf2_file"},
    {"gm_orchestral_harp","0460_Aspirin_sf2_file"},
    {"gm_overdriven_guitar","0290_FluidR3_GM_sf2_file"},
    {"gm_pad_bowed","0920_JCLive_sf2_file"},
    {"gm_pad_choir","0910_FluidR3_GM_sf2_file"},
    {"gm_pad_halo","0940_Chaos_sf2_file"},
    {"gm_pad_metallic","0930_Aspirin_sf2_file"},
    {"gm_pad_new_age","0880_JCLive_sf2_file"},
    {"gm_pad_poly","0900_JCLive_sf2_file"},
    {"gm_pad_sweep","0950_Aspirin_sf2_file"},
    {"gm_pad_warm","0890_JCLive_sf2_file"},
    {"gm_pan_flute","0750_JCLive_sf2_file"},
    {"gm_percussive_organ","0170_JCLive_sf2_file"},
    {"gm_piano","0000_JCLive_sf2_file"},
    {"gm_piccolo","0720_JCLive_sf2_file"},
    {"gm_pizzicato_strings","0450_Aspirin_sf2_file"},
    {"gm_recorder","0740_JCLive_sf2_file"},
    {"gm_reed_organ","0200_JCLive_sf2_file"},
    {"gm_reverse_cymbal","1190_JCLive_sf2_file"},
    {"gm_rock_organ","0180_JCLive_sf2_file"},
    {"gm_seashore","1220_JCLive_sf2_file"},
    {"gm_shakuhachi","0770_JCLive_sf2_file"},
    {"gm_shamisen","1060_JCLive_sf2_file"},
    {"gm_shanai","1110_Aspirin_sf2_file"},
    {"gm_sitar","1040_Aspirin_sf2_file"},
    {"gm_slap_bass_1","0360_Aspirin_sf2_file"},
    {"gm_slap_bass_2","0370_Aspirin_sf2_file"},
    {"gm_soprano_sax","0640_JCLive_sf2_file"},
    {"gm_steel_drums","1140_FluidR3_GM_sf2_file"},
    {"gm_string_ensemble_1","0480_Aspirin_sf2_file"},
    {"gm_string_ensemble_2","0490_Aspirin_sf2_file"},
    {"gm_synth_bass_1","0380_Chaos_sf2_file"},
    {"gm_synth_bass_2","0390_Aspirin_sf2_file"},
    {"gm_synth_brass_1","0620_Aspirin_sf2_file"},
    {"gm_synth_brass_2","0630_Aspirin_sf2_file"},
    {"gm_synth_choir","0540_Aspirin_sf2_file"},
    {"gm_synth_drum","1180_JCLive_sf2_file"},
    {"gm_synth_strings_1","0500_Aspirin_sf2_file"},
    {"gm_synth_strings_2","0510_Aspirin_sf2_file"},
    {"gm_taiko_drum","1160_JCLive_sf2_file"},
    {"gm_telephone","1240_JCLive_sf2_file"},
    {"gm_tenor_sax","0660_JCLive_sf2_file"},
    {"gm_timpani","0470_Aspirin_sf2_file"},
    {"gm_tinkle_bell","1120_Aspirin_sf2_file"},
    {"gm_tremolo_strings","0440_Aspirin_sf2_file"},
    {"gm_trombone","0570_Aspirin_sf2_file"},
    {"gm_trumpet","0560_FluidR3_GM_sf2_file"},
    {"gm_tuba","0580_FluidR3_GM_sf2_file"},
    {"gm_tubular_bells","0140_JCLive_sf2_file"},
    {"gm_vibraphone","0110_JCLive_sf2_file"},
    {"gm_viola","0410_Aspirin_sf2_file"},
    {"gm_violin","0400_Aspirin_sf2_file"},
    {"gm_voice_oohs","0530_Aspirin_sf2_file"},
    {"gm_whistle","0780_FluidR3_GM_sf2_file"},
    {"gm_woodblock","1150_JCLive_sf2_file"},
    {"gm_xylophone","0130_JCLive_sf2_file"},
};

/* First-font pack file for a gm_* name, without extension. NULL when the name
   is not a pinned webaudiofont program (caller falls back to FluidSynth). */
static const char *gm_pack_font(const char *name) {
    if (!name) return NULL;
    for (unsigned i = 0; i < sizeof(gm_fonts)/sizeof(*gm_fonts); i++)
        if (!strcmp(name,gm_fonts[i].name)) return gm_fonts[i].font;
    /* Names outside the pinned web table (e.g. the gm_*_piano aliases) keep
       their exact-program FluidSynth fallback. */
    return NULL;
}

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

/* ---- WebAudioFont sample packs (Strudel web parity) --------------------
 * The web renders gm_* through superdough's soundfont path: per-instrument
 * sample packs (MIT-licensed webaudiofontdata, runtime-fetched, never
 * bundled) played as pitched samples with a fixed 0.3 voice gain. Native
 * FluidSynth rendering uses a different font at gain 0.2, so levels and
 * sustain shape diverge. This pipeline plays the same samples through the
 * native sample path: fetch the pack, parse zones with a confined C parser
 * (remote text is never evaluated as JS), decode the selected zone's MP3,
 * bake the 0.3 voice staging into a cached WAV, and acquire it as a
 * pitch-keyed sample with the zone's sustain-loop fractions. The exact web
 * playbackRate closes through smp_speed, keeping the WAV a pristine copy of
 * the trimmed decode so loop phase stays sample-exact.
 * FluidSynth above stays as the offline fallback. */
#define GM_WAF_PACK_MAX (2u*1024u*1024u)
#define GM_WAF_B64_MAX (4u*1024u*1024u)
#define GM_WAF_MP3_MAX (8u*1024u*1024u)
#define GM_WAF_DECODE_MAX (8u*1024u*1024u)
#define GM_WAF_VOICE_GAIN 0.3
#define GM_WAF_PACK_SLOTS 8
#define GM_WAF_CACHE_VERSION "v3"

static int gm_b64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c-'A';
    if (c >= 'a' && c <= 'z') return c-'a'+26;
    if (c >= '0' && c <= '9') return c-'0'+52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int gm_b64_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

int gm_waf_b64_decode(const char *text, size_t len, unsigned char *out, size_t capacity) {
    if (!text || !out) return -1;
    if (len > GM_WAF_B64_MAX || capacity == 0) return -1;
    size_t count = 0, pad = 0;
    int seen_pad = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (gm_b64_space(c)) continue;
        if (c == '=') { seen_pad = 1; pad++; continue; }
        if (c >= 128 || gm_b64_value((char)c) < 0) return -1;
        if (seen_pad) return -1;
        count++;
    }
    if (pad > 2) return -1;
    size_t total = count+pad;
    if (total == 0 || total%4 != 0) return -1;
    size_t outlen = total/4*3;
    if (pad == 1) outlen -= 1;
    else if (pad == 2) outlen -= 2;
    if (outlen == 0 || outlen > capacity || outlen > (size_t)INT_MAX) return -1;
    size_t quartets = total/4, q = 0, oi = 0;
    int v[4], n = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (gm_b64_space(c)) continue;
        v[n++] = (c == '=') ? 0 : gm_b64_value((char)c);
        if (n != 4) continue;
        int bytes = (q == quartets-1) ? 3-(int)pad : 3;
        unsigned bits = ((unsigned)v[0]<<18)|((unsigned)v[1]<<12)|((unsigned)v[2]<<6)|(unsigned)v[3];
        if (bytes > 0) out[oi++] = (unsigned char)(bits>>16);
        if (bytes > 1) out[oi++] = (unsigned char)(bits>>8);
        if (bytes > 2) out[oi++] = (unsigned char)bits;
        n = 0; q++;
    }
    return oi == outlen ? (int)outlen : -1;
}

static const char *gm_field_find(const char *lo, const char *hi, const char *key) {
    size_t n = strlen(key);
    if (n == 0 || hi-lo < (ptrdiff_t)n) return NULL;
    for (const char *p = lo; p+n <= hi; p++)
        if (!memcmp(p,key,n)) return p;
    return NULL;
}

static int gm_field_long(const char *lo, const char *hi, const char *key, long *out) {
    const char *p = gm_field_find(lo,hi,key);
    if (!p) return 0;
    p += strlen(key);
    while (p < hi && (*p == ' ' || *p == '\t')) p++;
    int neg = 0;
    if (p < hi && *p == '-') { neg = 1; p++; }
    if (p >= hi || *p < '0' || *p > '9') return 0;
    long v = 0;
    while (p < hi && *p >= '0' && *p <= '9') {
        int d = *p-'0';
        if (v > (LONG_MAX-d)/10) return 0;
        v = v*10+d; p++;
    }
    *out = neg ? -v : v;
    return 1;
}

int gm_waf_parse(const char *text, GmWafZone *zones, int capacity) {
    if (!text || !zones || capacity <= 0) return -1;
    size_t len = strlen(text);
    if (len == 0 || len > GM_WAF_PACK_MAX) return -1;
    const char *hi = text+len;
    const char *zp = gm_field_find(text,hi,"zones");
    if (!zp) return -1;
    const char *arr = zp+5;
    while (arr < hi && *arr != '[') {
        if (*arr == ']' || *arr == '{' || *arr == ';') return -1;
        arr++;
    }
    if (arr >= hi) return -1;
    const char *p = arr+1;
    int n = 0;
    while (p < hi && n < capacity && n < GM_WAF_MAX_ZONES) {
        const char *open = gm_field_find(p,hi,"{");
        if (!open) break;
        /* Base64 payloads never contain braces, so the first '}' closes the block. */
        const char *close = gm_field_find(open+1,hi,"}");
        if (!close || close-open > (ptrdiff_t)GM_WAF_B64_MAX+4096) return -1;
        const char *file_key = gm_field_find(open,close,"file:");
        if (!file_key) { p = close+1; continue; }
        long low, high, pitch, coarse, fine, rate;
        if (!gm_field_long(open,file_key,"keyRangeLow:",&low) ||
            !gm_field_long(open,file_key,"keyRangeHigh:",&high) ||
            !gm_field_long(open,file_key,"originalPitch:",&pitch) ||
            !gm_field_long(open,file_key,"coarseTune:",&coarse) ||
            !gm_field_long(open,file_key,"fineTune:",&fine) ||
            !gm_field_long(open,file_key,"sampleRate:",&rate)) { p = close+1; continue; }
        if (low < 0 || low > 127 || high < 0 || high > 127 || low > high ||
            pitch < 0 || pitch > 12700 || coarse < -128 || coarse > 128 ||
            fine < -500 || fine > 500 || rate < 1000 || rate > 384000) { p = close+1; continue; }
        /* Sustain loops are optional: a zone without a valid loop still plays,
           it just does not loop. */
        long loop_start = 0, loop_end = 0;
        long ls = 0, le = 0;
        if (gm_field_long(open,file_key,"loopStart:",&ls) &&
            gm_field_long(open,file_key,"loopEnd:",&le) &&
            ls >= 0 && le > ls && le <= (long)GM_WAF_DECODE_MAX) {
            loop_start = ls; loop_end = le;
        }
        const char *fs = file_key+5;
        while (fs < close && (*fs == ' ' || *fs == '\t')) fs++;
        if (fs >= close || (*fs != '\'' && *fs != '"')) { p = close+1; continue; }
        char quote = *fs++;
        const char *fe = fs;
        while (fe < close && *fe != quote) fe++;
        if (fe >= close || fe == fs || (size_t)(fe-fs) > GM_WAF_B64_MAX) { p = close+1; continue; }
        zones[n].low = (int)low; zones[n].high = (int)high;
        zones[n].original_pitch = (int)pitch;
        zones[n].coarse = (int)coarse; zones[n].fine = (int)fine;
        zones[n].sample_rate = (int)rate;
        zones[n].loop_start = loop_start; zones[n].loop_end = loop_end;
        zones[n].file = fs; zones[n].file_len = (size_t)(fe-fs);
        n++;
        p = close+1;
    }
    return n;
}

int gm_waf_pick(const GmWafZone *zones, int n, double note) {
    if (!zones || n <= 0 || !isfinite(note)) return -1;
    /* Verbatim web predicate (find returns the first match in pack order):
       keyRangeLow <= v && keyRangeHigh+1 >= v. The +1 slop and pack order
       decide overlaps; fractional notes keep their fraction here while the
       voice pitch still comes from ev->freq. */
    for (int i = 0; i < n; i++)
        if ((double)zones[i].low <= note && note <= (double)zones[i].high+1) return i;
    return -1;
}

/* Effective recorded root, verbatim from the web player (packages/superdough
   soundfont path): e = originalPitch - 100*coarseTune - fineTune, in MIDI
   cents/100. Note the minus signs: this is the player as shipped, and its
   playbackRate 2^((100*note - e)/1200) is what the gate compares against. */
static double gm_waf_effective(const GmWafZone *zone) {
    return ((double)zone->original_pitch-zone->coarse*100.0-zone->fine)/100.0;
}

int gm_waf_base_midi(const GmWafZone *zone) {
    if (!zone) return 60;
    double eff = gm_waf_effective(zone);
    long base = lround(eff);
    if (base < 0) base = 0;
    if (base > 127) base = 127;
    return (int)base;
}

/* Web playback rate for a note, verbatim: 2^((100*note - e)/1200) with
   e = originalPitch - 100*coarseTune - fineTune. The native voice plays the
   cached WAV at ev->freq/midi_to_freq(base) times smp_speed, so the WAV stays
   a pristine (gain-staged only) copy of the trimmed decode and this factor
   closes the remaining rate gap exactly. It is note-independent: the note
   cancels out, leaving 2^((100*base - e)/1200). */
double gm_waf_speed_correction(const GmWafZone *zone, int base) {
    if (!zone || base < 0 || base > 127) return 1;
    double e = (double)zone->original_pitch-zone->coarse*100.0-zone->fine;
    double correction = pow(2.0,(100.0*base-e)/1200.0);
    return isfinite(correction) && correction > 0 ? correction : 1;
}

int gm_waf_loop_fractions(const GmWafZone *zone, long decoded_frames, long decoded_rate,
    double *lo, double *hi) {
    if (!zone || !lo || !hi || decoded_frames <= 0) return 0;
    if (decoded_rate < 1000 || decoded_rate > 384000) return 0;
    if (zone->sample_rate < 1000 || zone->sample_rate > 384000) return 0;
    /* Verbatim web gate: loopStart > 1 && loopStart < loopEnd enables looping.
       The web sets loop seconds as loopStart/zoneRate on the resampled buffer,
       so pack indices convert through the zone rate, not the MP3 rate (packs
       like Aspirin declare e.g. 29999 Hz zones holding 32000 Hz MP3). With the
       final buffer in seconds (decodedFrames/decodedRate), fractions are
       (loopStart/zoneRate)/bufferSeconds. Zone and MP3 rates equal for JCLive,
       where this reduces to loopStart/decodedFrames. */
    if (zone->loop_start <= 1 || zone->loop_end <= zone->loop_start) return 0;
    double buffer_seconds = (double)decoded_frames/(double)decoded_rate;
    if (!(buffer_seconds > 0) || !isfinite(buffer_seconds)) return 0;
    double start = (double)zone->loop_start/(double)zone->sample_rate/buffer_seconds;
    double end = (double)zone->loop_end/(double)zone->sample_rate/buffer_seconds;
    /* start is non-negative (loop_start > 1 gate above). end may exceed 1:
       like the web (loopEnd past duration ends the source instead of
       wrapping), the voice then plays to the buffer end, so leave it. */
    if (!(end > start) || !(start < 1)) return 0;
    *lo = start;
    *hi = end;
    if (!isfinite(*lo) || !isfinite(*hi) || *lo < 0 || *hi <= *lo) return 0;
    return 1;
}

/* In-memory MP3 decode matching src/sampler.c (minimp3 with the same gapless
   convention), so zone PCM matches what the sample cache would decode. */
static uint32_t gm_mp3_u32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}

static int gm_mp3_gapless(const uint8_t *frame, size_t size, int frame_samples,
    size_t *skip, size_t *length) {
    if (size < 4) return 0;
    int mpeg1 = ((frame[1]>>3)&3) == 3;
    int mono = (frame[3]>>6) == 3;
    size_t offset = 4+(mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17));
    if (!(frame[1]&1)) offset += 2;
    if (offset > size || size-offset < 8) return 0;
    if (memcmp(frame+offset,"Xing",4) && memcmp(frame+offset,"Info",4)) return 0;
    uint32_t flags = gm_mp3_u32(frame+offset+4);
    offset += 8;
    if (!(flags&1) || size-offset < 4) return 1;
    uint64_t count = (uint64_t)gm_mp3_u32(frame+offset)*(unsigned)frame_samples;
    offset += 4;
    const unsigned masks[] = {2,4,8};
    const size_t sizes[] = {4,100,4};
    for (int i = 0; i < 3; i++) {
        if (!(flags&masks[i])) continue;
        if (size-offset < sizes[i]) return 1;
        offset += sizes[i];
    }
    if (size-offset < 36 || !frame[offset]) return 1;
    const uint8_t *tag = frame+offset+21;
    unsigned delay = ((unsigned)tag[0]<<4)|(tag[1]>>4);
    unsigned padding = ((unsigned)(tag[1]&15)<<8)|tag[2];
    if (count <= (uint64_t)delay+padding || count > GM_WAF_DECODE_MAX) return 1;
    *skip = delay+529;
    *length = (size_t)(count-delay-padding);
    return 1;
}

static float *gm_mp3_decode(const unsigned char *data, size_t len, uint32_t *have_out,
    int *rate_out, int *channels_out) {
    mp3dec_t dec;
    mp3dec_init(&dec);
    mp3dec_frame_info_t info;
    memset(&info,0,sizeof(info));
    mp3d_sample_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    size_t cap = 65536, n = 0;
    float *stereo = malloc(cap*sizeof(float)*2);
    if (!stereo) return NULL;
    int rate = 0, fail = 0, is_stereo = 0;
    int checked_tag = 0;
    size_t skip = 0, length = 0;
    const uint8_t *p = data;
    size_t left = len;
    for (;;) {
        int samples = mp3dec_decode_frame(&dec,p,(int)(left > 1<<30 ? 1<<30 : left),pcm,&info);
        if (info.frame_bytes <= 0 || (size_t)info.frame_bytes > left) break;
        if (!checked_tag && samples > 0) {
            checked_tag = 1;
            if (info.layer == 3 && info.frame_offset >= 0 && info.frame_offset < info.frame_bytes &&
                gm_mp3_gapless(p+(size_t)info.frame_offset,
                    (size_t)(info.frame_bytes-info.frame_offset),samples,&skip,&length)) {
                p += (size_t)info.frame_bytes;
                left -= (size_t)info.frame_bytes;
                mp3dec_init(&dec);
                continue;
            }
        }
        p += (size_t)info.frame_bytes;
        left -= (size_t)info.frame_bytes;
        if (samples <= 0 || info.channels < 1 || info.channels > 2) continue;
        if (!rate && info.hz > 0) rate = info.hz;
        if (info.hz != rate) { fail = 1; break; }
        if (info.channels == 2) is_stereo = 1;
        size_t drop = skip < (size_t)samples ? skip : (size_t)samples;
        skip -= drop;
        int first = (int)drop;
        samples -= first;
        if (length && n+(size_t)samples > length) samples = (int)(length-n);
        if (n+(size_t)samples > GM_WAF_DECODE_MAX) break;
        if (n+(size_t)samples > cap) {
            size_t nc = cap*2;
            if (nc > GM_WAF_DECODE_MAX) nc = GM_WAF_DECODE_MAX;
            if (nc > SIZE_MAX/(sizeof(float)*2)) { fail = 1; break; }
            float *nm = realloc(stereo,nc*sizeof(float)*2);
            if (!nm) { fail = 1; break; }
            stereo = nm;
            cap = nc;
        }
        for (int i = first; i < first+samples; i++) {
            stereo[n*2] = pcm[i*info.channels];
            stereo[n*2+1] = pcm[i*info.channels+(info.channels == 2 ? 1 : 0)];
            n++;
        }
        if (left == 0 || (length && n == length)) break;
    }
    /* Gapless trimming matches the browser decode length frame-for-frame
       (verified identical); loop points address the trimmed buffer absolutely. */
    if (fail || n == 0 || rate <= 0) { free(stereo); return NULL; }
    *have_out = (uint32_t)n;
    *rate_out = rate;
    *channels_out = is_stereo ? 2 : 1;
    return stereo;
}

/* Same location as the web-sample cache; file names below are generated from
   validated integers only, never from pack text. */
static int gm_cache_dir(char *out, int n) {
    const char *home = getenv("HOME");
    if (!home || !home[0] || strlen(home) > 400) return 0;
    char parent[512];
    snprintf(parent,sizeof(parent),"%s/.cache",home);
    mkdir(parent,0700);
    user_dir(parent,sizeof(parent),NULL,"",".cache/tompouce");
    if (!parent[0]) return 0;
    mkdir(parent,0700);
    int wr = snprintf(out,(size_t)n,"%s/web",parent);
    if (wr < 0 || wr >= n) return 0;
    if (mkdir(out,0700) != 0 && errno != EEXIST) return 0;
    return 1;
}

/* Pinned font file names contain only these characters (checked before any
   path join); the table above is trusted, the check is defense in depth. */
static int gm_font_ok(const char *s) {
    if (!s) return 0;
    size_t n = strlen(s);
    if (n < 12 || n > 48) return 0;
    if (strcmp(s+n-9,"_sf2_file") != 0) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!(c >= 'A' && c <= 'Z') && !(c >= 'a' && c <= 'z') &&
            !(c >= '0' && c <= '9') && c != '_' && c != '-') return 0;
    }
    return 1;
}

static int gm_pack_filename(const char *font, char *out, int n) {
    if (!gm_font_ok(font) || !out || n < 32) return 0;
    int wr = snprintf(out,(size_t)n,"%s.js",font);
    return wr > 0 && wr < n;
}

static char *gm_read_bounded_text(const char *path, size_t max) {
    struct stat st;
    if (!path || lstat(path,&st) != 0 || S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode)) return NULL;
    if (st.st_size <= 0 || (size_t)st.st_size > max) return NULL;
    FILE *f = fopen(path,"rb");
    if (!f) return NULL;
    size_t len = (size_t)st.st_size;
    char *buf = malloc(len+1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf,1,len,f);
    fclose(f);
    if (got != len || memchr(buf,0,len)) { free(buf); return NULL; }
    buf[len] = 0;
    return buf;
}

static int gm_write_text_cache(const char *path, const char *text) {
    if (!path || !text) return 0;
    size_t len = strlen(text);
    if (len == 0 || len > GM_WAF_PACK_MAX) return 0;
    char part[800];
    if (snprintf(part,sizeof(part),"%s.part",path) >= (int)sizeof(part)) return 0;
    FILE *f = fopen(part,"wb");
    if (!f) return 0;
    size_t wrote = fwrite(text,1,len,f);
    int closed = fclose(f);
    if (wrote != len || closed != 0) { unlink(part); return 0; }
    if (rename(part,path) != 0) { unlink(part); return 0; }
    return 1;
}

/* Owned pack text: installed banks first (installer prefetch, offline), then
   the per-user disk cache, then the existing confined github fetch (same
   public-URL and redirect policy as all web samples). */
static char *gm_pack_load(const char *font) {
    char file[64];
    if (!gm_pack_filename(font,file,sizeof(file))) return NULL;
    banks_ensure_default();
    char installed[1152];
    const char *root = banks_root_dir();
    if (root && root[0] && strlen(root) < 1000) {
        if (snprintf(installed,sizeof(installed),"%s/gm-webaudiofont/%s",root,file) <
            (int)sizeof(installed)) {
            char *pinned = gm_read_bounded_text(installed,GM_WAF_PACK_MAX);
            if (pinned) return pinned;
        }
    }
    char dir[512];
    if (!gm_cache_dir(dir,sizeof(dir))) return NULL;
    char path[640];
    if (snprintf(path,sizeof(path),"%s/gm_%s",dir,file) >= (int)sizeof(path)) return NULL;
    char *cached = gm_read_bounded_text(path,GM_WAF_PACK_MAX);
    if (cached) return cached;
    char spec[160];
    if (snprintf(spec,sizeof(spec),"github:felixroos/webaudiofontdata/master/sound/%s",file) >=
        (int)sizeof(spec)) return NULL;
    char *fetched = websamp_fetch_text(spec);
    if (!fetched || strlen(fetched) == 0 || strlen(fetched) > GM_WAF_PACK_MAX) {
        free(fetched);
        return NULL;
    }
    gm_write_text_cache(path,fetched); /* best effort; a missing disk cache refetches */
    return fetched;
}

typedef struct {
    int program; /* -1 = empty slot */
    char font[64]; /* pinned pack file stem, empty when unset */
    unsigned serial;
    int fetch_failed;
    char *text; /* owned pack JS; zones reference slices of it */
    GmWafZone zones[GM_WAF_MAX_ZONES];
    int nzones;
    int smp_ids[GM_WAF_MAX_ZONES]; /* sample id, -1 unknown, -2 failed */
    double loop_lo[GM_WAF_MAX_ZONES];
    double loop_hi[GM_WAF_MAX_ZONES];
} GmPack;

static GmPack gm_packs[GM_WAF_PACK_SLOTS];
static unsigned gm_waf_clock = 0;
static int gm_waf_inited = 0;
static pthread_mutex_t gm_waf_lock = PTHREAD_MUTEX_INITIALIZER;

static void gm_waf_init_locked(void) {
    if (gm_waf_inited) return;
    gm_waf_inited = 1;
    for (int i = 0; i < GM_WAF_PACK_SLOTS; i++) {
        gm_packs[i].program = -1;
        for (int z = 0; z < GM_WAF_MAX_ZONES; z++) gm_packs[i].smp_ids[z] = -1;
    }
}

/* Publish a fetched pack. The caller passes ownership of text. Slots are
   keyed by pinned font stem; the program number is kept for diagnostics. */
static GmPack *gm_pack_publish(int program, const char *font, char *text, GmWafZone *zones, int nzones) {
    pthread_mutex_lock(&gm_waf_lock);
    gm_waf_init_locked();
    GmPack *slot = NULL;
    for (int i = 0; i < GM_WAF_PACK_SLOTS; i++)
        if (gm_packs[i].text && !strcmp(gm_packs[i].font,font)) { slot = &gm_packs[i]; break; }
    if (!slot) {
        slot = NULL;
        for (int i = 0; i < GM_WAF_PACK_SLOTS; i++)
            if (!gm_packs[i].text && !gm_packs[i].fetch_failed) { slot = &gm_packs[i]; break; }
    }
    if (!slot) {
        slot = &gm_packs[0];
        for (int i = 1; i < GM_WAF_PACK_SLOTS; i++)
            if (gm_packs[i].serial < slot->serial) slot = &gm_packs[i];
        free(slot->text);
        slot->text = NULL;
    }
    free(slot->text);
    slot->text = text;
    slot->program = program;
    snprintf(slot->font,sizeof(slot->font),"%s",font);
    slot->fetch_failed = 0;
    slot->nzones = nzones < GM_WAF_MAX_ZONES ? nzones : GM_WAF_MAX_ZONES;
    memcpy(slot->zones,zones,(size_t)slot->nzones*sizeof(*zones));
    for (int z = 0; z < GM_WAF_MAX_ZONES; z++) {
        slot->smp_ids[z] = -1;
        slot->loop_lo[z] = 0;
        slot->loop_hi[z] = 1;
    }
    slot->serial = ++gm_waf_clock;
    pthread_mutex_unlock(&gm_waf_lock);
    return slot;
}

static void gm_pack_mark_failed(int program, const char *font) {
    pthread_mutex_lock(&gm_waf_lock);
    gm_waf_init_locked();
    for (int i = 0; i < GM_WAF_PACK_SLOTS; i++)
        if (!strcmp(gm_packs[i].font,font)) {
            pthread_mutex_unlock(&gm_waf_lock);
            return; /* another thread published or already marked */
        }
    GmPack *slot = NULL;
    for (int i = 0; i < GM_WAF_PACK_SLOTS; i++)
        if (!gm_packs[i].text && !gm_packs[i].fetch_failed) { slot = &gm_packs[i]; break; }
    if (!slot) {
        slot = &gm_packs[0];
        for (int i = 1; i < GM_WAF_PACK_SLOTS; i++)
            if (gm_packs[i].serial < slot->serial) slot = &gm_packs[i];
        free(slot->text);
        slot->text = NULL;
    }
    slot->program = program;
    snprintf(slot->font,sizeof(slot->font),"%s",font);
    slot->fetch_failed = 1;
    slot->serial = ++gm_waf_clock;
    pthread_mutex_unlock(&gm_waf_lock);
}

static GmPack *gm_pack_find(const char *font) {
    pthread_mutex_lock(&gm_waf_lock);
    gm_waf_init_locked();
    GmPack *found = NULL;
    for (int i = 0; i < GM_WAF_PACK_SLOTS; i++)
        if (gm_packs[i].text && !strcmp(gm_packs[i].font,font)) {
            gm_packs[i].serial = ++gm_waf_clock;
            found = &gm_packs[i];
            break;
        }
    pthread_mutex_unlock(&gm_waf_lock);
    return found;
}

static int gm_pack_failed(const char *font) {
    pthread_mutex_lock(&gm_waf_lock);
    gm_waf_init_locked();
    int failed = 0;
    for (int i = 0; i < GM_WAF_PACK_SLOTS; i++)
        if (gm_packs[i].fetch_failed && !strcmp(gm_packs[i].font,font)) { failed = 1; break; }
    pthread_mutex_unlock(&gm_waf_lock);
    return failed;
}

static GmPack *gm_pack_ensure(int program, const char *font) {
    if (!gm_font_ok(font)) return NULL;
    GmPack *found = gm_pack_find(font);
    if (found) return found;
    if (gm_pack_failed(font)) return NULL;
    /* Fetch and parse outside the pack lock; concurrent workers may duplicate
       the work once, publish is double-checked. */
    char *text = gm_pack_load(font);
    if (!text) { gm_pack_mark_failed(program,font); return NULL; }
    GmWafZone zones[GM_WAF_MAX_ZONES];
    int nzones = gm_waf_parse(text,zones,GM_WAF_MAX_ZONES);
    if (nzones <= 0) {
        log_console("GM: cannot parse webaudiofont pack for program %d",program);
        free(text);
        gm_pack_mark_failed(program,font);
        return NULL;
    }
    return gm_pack_publish(program,font,text,zones,nzones);
}

/* 16-bit PCM WAV writer. The sampler's WAV decoder accepts format 1/16-bit,
   so zones round-trip through smp_acquire_abs exactly. */
static int gm_write_wav(const char *path, const float *pcm, uint32_t frames, int channels, int rate) {
    if (!path || !pcm || frames == 0 || (channels != 1 && channels != 2)) return 0;
    if (rate < 1000 || rate > 384000) return 0;
    if ((uint64_t)frames*(uint64_t)channels > (uint64_t)GM_WAF_DECODE_MAX) return 0;
    uint64_t data_bytes = (uint64_t)frames*(uint64_t)channels*2;
    if (data_bytes > 0x7fffffff) return 0;
    FILE *f = fopen(path,"wb");
    if (!f) return 0;
    uint8_t hdr[44];
    uint32_t total = (uint32_t)(36+data_bytes);
    memcpy(hdr,"RIFF",4);
    hdr[4] = (uint8_t)total; hdr[5] = (uint8_t)(total>>8);
    hdr[6] = (uint8_t)(total>>16); hdr[7] = (uint8_t)(total>>24);
    memcpy(hdr+8,"WAVEfmt ",8);
    hdr[16] = 16; hdr[17] = 0; hdr[18] = 0; hdr[19] = 0;
    hdr[20] = 1; hdr[21] = 0;
    hdr[22] = (uint8_t)channels; hdr[23] = 0;
    hdr[24] = (uint8_t)rate; hdr[25] = (uint8_t)(rate>>8);
    hdr[26] = (uint8_t)(rate>>16); hdr[27] = (uint8_t)(rate>>24);
    uint32_t byterate = (uint32_t)rate*(uint32_t)channels*2;
    hdr[28] = (uint8_t)byterate; hdr[29] = (uint8_t)(byterate>>8);
    hdr[30] = (uint8_t)(byterate>>16); hdr[31] = (uint8_t)(byterate>>24);
    hdr[32] = (uint8_t)(channels*2); hdr[33] = 0;
    hdr[34] = 16; hdr[35] = 0;
    memcpy(hdr+36,"data",4);
    uint32_t db = (uint32_t)data_bytes;
    hdr[40] = (uint8_t)db; hdr[41] = (uint8_t)(db>>8);
    hdr[42] = (uint8_t)(db>>16); hdr[43] = (uint8_t)(db>>24);
    int ok = fwrite(hdr,1,sizeof(hdr),f) == sizeof(hdr);
    for (uint32_t i = 0; ok && i < frames; i++) {
        for (int c = 0; c < channels; c++) {
            double v = pcm[(size_t)i*2+c];
            if (!isfinite(v)) v = 0;
            if (v > 1) v = 1;
            if (v < -1) v = -1;
            long s = lround(v*32767);
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            uint8_t b[2] = {(uint8_t)(s&0xff),(uint8_t)((s>>8)&0xff)};
            if (fwrite(b,1,2,f) != 2) { ok = 0; break; }
        }
    }
    if (fclose(f) != 0) ok = 0;
    if (!ok) unlink(path);
    return ok;
}

/* Bake one zone: base64 MP3 -> trimmed PCM -> 0.3-staged WAV -> sample id.
   The WAV is intentionally not detune-resampled: the web plays the raw
   decode at playbackRate 2^((100*note - e)/1200), and any resample grid
   would quantize the sustain loop phase. The exact rate match comes from
   gm_waf_speed_correction through ev->smp_speed instead. Loop fractions come
   from the trimmed decode. */
static int gm_zone_build(const GmWafZone *zone, const char *wav_final,
    int *smp_out, double *lo_out, double *hi_out) {
    if (!zone || !wav_final || !smp_out || !lo_out || !hi_out) return 0;
    if (zone->file_len == 0 || zone->file_len > GM_WAF_B64_MAX) return 0;
    size_t mp3cap = zone->file_len/4*3+3;
    if (mp3cap == 0 || mp3cap > GM_WAF_MP3_MAX) return 0;
    unsigned char *mp3 = malloc(mp3cap);
    if (!mp3) return 0;
    int mp3len = gm_waf_b64_decode(zone->file,zone->file_len,mp3,mp3cap);
    if (mp3len <= 0) { free(mp3); return 0; }
    uint32_t have = 0;
    int rate = 0, channels = 0;
    float *pcm = gm_mp3_decode(mp3,(size_t)mp3len,&have,&rate,&channels);
    free(mp3);
    if (!pcm || have == 0) { free(pcm); return 0; }
    double lo = 0, hi = 1;
    int loop = gm_waf_loop_fractions(zone,(long)have,(long)rate,&lo,&hi);
    if (!loop) { lo = 0; hi = 1; }
    int base = gm_waf_base_midi(zone);
    if (have > (uint32_t)(44100*120)) { free(pcm); return 0; }
    for (uint32_t i = 0; i < have; i++)
        for (int c = 0; c < 2; c++)
            pcm[(size_t)i*2+c] = (float)(pcm[(size_t)i*2+c]*GM_WAF_VOICE_GAIN);
    struct stat st;
    int have_wav = stat(wav_final,&st) == 0 && S_ISREG(st.st_mode) && st.st_size > 44;
    if (!have_wav) {
        char part[800];
        int built = snprintf(part,sizeof(part),"%s.part",wav_final) < (int)sizeof(part) &&
            gm_write_wav(part,pcm,have,channels,rate) &&
            rename(part,wav_final) == 0;
        if (!built) {
            char retry[800];
            if (snprintf(retry,sizeof(retry),"%s.part",wav_final) < (int)sizeof(retry))
                unlink(retry);
            /* Another worker may have won the rename race; fall through to acquire. */
        }
    }
    free(pcm);
    int id = smp_acquire_abs(wav_final,base);
    if (id < 0) return 0;
    *smp_out = id;
    *lo_out = lo;
    *hi_out = hi;
    return 1;
}

static int gm_zone_wav_path(const char *font, const GmWafZone *zone, char *out, int n) {
    if (!gm_font_ok(font) || !zone || !out || n < 64) return 0;
    int base = gm_waf_base_midi(zone);
    char dir[512];
    if (!gm_cache_dir(dir,sizeof(dir))) return 0;
    int wr = snprintf(out,(size_t)n,"%s/gm_%s_z%d-%d_b%d_"GM_WAF_CACHE_VERSION".wav",
        dir,font,zone->low,zone->high,base);
    return wr > 0 && wr < n;
}

/* Lazily materialize one zone. The pack lock is only held for table checks;
   fetch, decode and file I/O run outside it. */
static int gm_zone_materialize(GmPack *pack, const char *font, int zi,
    int *smp_out, double *lo_out, double *hi_out) {
    if (!pack || !gm_font_ok(font) || zi < 0 || zi >= pack->nzones || !smp_out || !lo_out || !hi_out) return 0;
    pthread_mutex_lock(&gm_waf_lock);
    /* The slot may have been evicted for another pack; never build from a
       zone the caller did not select. */
    if (strcmp(pack->font,font) || !pack->text || zi >= pack->nzones) {
        pthread_mutex_unlock(&gm_waf_lock);
        return 0;
    }
    int known = pack->smp_ids[zi];
    double klo = pack->loop_lo[zi], khi = pack->loop_hi[zi];
    GmWafZone zone = pack->zones[zi];
    /* The file slice points into pack text that eviction may free; duplicate
       it here so the build below never touches pack memory unlocked. */
    char *file_copy = NULL;
    if (known < 0 && known != -2 && zone.file_len > 0 && zone.file_len <= GM_WAF_B64_MAX) {
        file_copy = malloc(zone.file_len);
        if (file_copy) memcpy(file_copy,zone.file,zone.file_len);
    }
    pthread_mutex_unlock(&gm_waf_lock);
    if (known >= 0) { *smp_out = known; *lo_out = klo; *hi_out = khi; return 1; }
    if (known == -2 || !file_copy) { free(file_copy); return 0; }
    zone.file = file_copy;
    char wav[640];
    if (!gm_zone_wav_path(font,&zone,wav,sizeof(wav))) { free(file_copy); return 0; }
    int smp = -1;
    double lo = 0, hi = 1;
    int built = gm_zone_build(&zone,wav,&smp,&lo,&hi);
    free(file_copy);
    if (!built || smp < 0) {
        pthread_mutex_lock(&gm_waf_lock);
        if (!strcmp(pack->font,font) && pack->smp_ids[zi] == -1) pack->smp_ids[zi] = -2;
        pthread_mutex_unlock(&gm_waf_lock);
        return 0;
    }
    pthread_mutex_lock(&gm_waf_lock);
    /* The slot may have been evicted and reused for another pack meanwhile. */
    if (!strcmp(pack->font,font) && pack->smp_ids[zi] < 0) {
        pack->smp_ids[zi] = smp;
        pack->loop_lo[zi] = lo;
        pack->loop_hi[zi] = hi;
    }
    pthread_mutex_unlock(&gm_waf_lock);
    *smp_out = smp;
    *lo_out = lo;
    *hi_out = hi;
    return 1;
}

int gm_synth_prepare_sample(AudioEvent *event, const cJSON *value, double midi) {
    (void)value;
    if (!event || !isfinite(midi)) return 0;
    int program = gm_synth_program(event->token);
    if (program < 0) return 0;
    /* First-font pack from the pinned web table; unknown names keep the
       FluidSynth fallback. */
    const char *font = gm_pack_font(event->token);
    if (!gm_font_ok(font)) return 0;
    /* The web finds no zone outside 0..127 and throws; fall back instead. The
       fractional value goes to zone selection (first range match, high+1 slop)
       while the voice pitch below still comes from ev->freq. */
    if (midi < 0 || midi > 127) return 0;
    GmPack *pack = gm_pack_ensure(program,font);
    if (!pack) return 0;
    /* Pick and copy the zone under one lock hold so eviction cannot swap the
       pack between selection and use. Scalar fields only; file slices are
       duplicated separately inside materialize. */
    int zi = -1;
    GmWafZone zone;
    memset(&zone,0,sizeof(zone));
    pthread_mutex_lock(&gm_waf_lock);
    if (!strcmp(pack->font,font) && pack->text) {
        zi = gm_waf_pick(pack->zones,pack->nzones,midi);
        if (zi >= 0 && zi < pack->nzones) zone = pack->zones[zi];
        else zi = -1;
    }
    pthread_mutex_unlock(&gm_waf_lock);
    if (zi < 0) return 0;
    int smp = -1;
    double lo = 0, hi = 1;
    if (!gm_zone_materialize(pack,font,zi,&smp,&lo,&hi) || smp < 0) return 0;
    event->smp = smp;
    if (hi > lo) {
        event->dsp.p[DSP_LOOP] = 1;
        event->dsp.p[DSP_LOOPBEGIN] = lo;
        event->dsp.p[DSP_LOOPEND] = hi;
    }
    /* The cached WAV keeps the zone's recorded pitch (integer base); the exact
       web playbackRate 2^((100*note - e)/1200) is closed through smp_speed.
       The factor is note-independent and multiplies (never replaces) the
       pattern speed, so explicit speed/detune controls keep working. This runs
       before the shared sample epilogue, which only multiplies smp_speed
       further for wavetable/cycle units. */
    int base = gm_waf_base_midi(&zone);
    double correction = gm_waf_speed_correction(&zone,base);
    if (!isfinite(correction) || correction <= 0) return 0;
    event->smp_speed *= correction;
    if (!isfinite(event->smp_speed) || event->smp_speed == 0) return 0;
    event->is_drum = 0;
    return 1;
}
