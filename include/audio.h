#ifndef AUDIO_H
#define AUDIO_H
#include "common.h"
#include "event.h"

typedef struct Voice {
    int active;
    int inst;
    double freq;
    double phase;
    double amp;
    double pan;
    double start_time;
    double note_off_time;
    double env;        // current env level
    int released;
    double vatt;       // attack seconds, <0 = default
    double vrel;       // release seconds, <0 = default
    double vdec;       // decay seconds, <0 = default
    double vsus;       // sustain 0..1, <0 = default
    double pw;         // pulse width, <0 = plain square
    double lpf_hz;     // 0 = bypass
    double lpf_z;
    int lfo_tgt;
    int lfo_wave;
    double lfo_hz;
    double lfo_depth;
    double lfo_phase;
    double release_start_level;
    double release_time;
    double gate_end;
    // wavetable state (valid when inst == INST_WT)
    int wt;
    double wtpos; // table position in [0, WT_SIZE)
    // one-shot sample state (valid when smp >= 0)
    int smp;
    float *smpbuf;
    int smplen;
    double smppos;  // fractional position for pitch-shifted playback
    double smpstep; // advance per output sample (1 = original pitch)
    int smp_gate;   // respect note duration + release (melodic banks)
    // drum-specific
    double age;
} Voice;

void audio_init(void);
void audio_shutdown(void);
double audio_time_now(void);
void audio_toggle_play(int on);
int audio_is_playing(void);
void audio_set_bpm(double bpm);
double audio_get_bpm(void);
void audio_set_block(unsigned long n);
int audio_is_stream_active(void);
/* master fx chain params (drive/cutoff/delay/verb), each 0..1 */
void audio_fx_set(const char *name, double v);
double audio_sec_per_beat(void);
double audio_sec_per_cycle(void);
double audio_cycle_beats(void);

/* trigger a voice from an event; time is event time (may be <=now, trigger immediately) */
void audio_trigger_event(AudioEvent *ev);

/* nerd-pane stats (thread-safe snapshots) */
int audio_active_voices(void);
void audio_voice_stats(int *active_out, int by_inst[10]);
const char *audio_inst_name(int inst);
/* recent master output, oldest first (for the scope); returns count */
int audio_scope_copy(float *out, int n);

int note_name_to_midi(const char *name);
double midi_to_freq(int midi);

#endif