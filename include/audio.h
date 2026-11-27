#ifndef AUDIO_H
#define AUDIO_H
#include "csound_host.h"
extern float *out_target_buffers[]; /* set by the output backend per render */
void audio_set_csound(CsoundProgram *program);
void audio_set_reference_mode(int on);
/* Reseed live wavetable phase randomization from the current time and PID.
   Reference renders call audio_set_reference_mode(1) instead for a fixed seed. */
void audio_live_randomize(void);
#include "common.h"
#include "event.h"

#define WT_SAMPLE_VOICES 16

typedef struct Voice {
    NativeSynthState *native_state;
    NativeSynthState *native_right;
    const NativeSynth *native_program;
    DspState dsp;
    DspState dsp_right;
    int stereo_filter_type; /* -1 or single filter type for persistent lanes */
    DspBiquad2 stereo_filter;
    int fx_stage_count;
    int route_fx_stereo;
    int fx_room_present;
    double fx_chain_gain;
    int fx_chain_folded;
    AudioFxStage fx_stages[AUDIO_FX_STAGES_MAX];
    DspBiquad fx_filters[2][AUDIO_FX_STAGES_MAX];
    int active;
    int out_target; /* 0 default, 1..4 named output streams */
    int analyser_id, analyser_fft; /* per-pattern analyser tap */
    int soa_lane, soa_active, soa_pending; /* from-allocation SoA bank */
    unsigned scope_mask, scope_generation;
    int inst;
    double freq;
    double phase;
    double amp;
    double pan;
    int has_pan;
    /* Precomputed routing/mix coefficients. Recomputed only at note-on from
       the immutable event, never inside the audio callback. The multiplication
       order in route_voice is unchanged, so PCM hashes stay identical. */
    double route_ll, route_lr, route_rl, route_rr, route_left, route_right;
    double route_dry, route_delay, route_room, route_room_size;
    int route_dsp, route_cached, route_side_one;
    double waveform_gain;
    double start_time;
    double source_begin; /* source worklet's first active render quantum */
    unsigned long long onset_serial;
    int onset_pending;
    int sampled_source, cut_pending;
    double cut_time;
    int retiring;
    double retire_end, retire_anchor, retire_level;
    double onset_delay_time, onset_delay_feedback;
    unsigned onset_duck_mask;
    double onset_duck_depth, onset_duck_attack;
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
    double lpq;        // <=0 one-pole
    double lpenv;
    int has_lpenv;
    double lpa;
    double bx1, bx2, by1, by2;
    double smp_end;    // fraction of the buffer to play
    double smp_begin;
    double delay_send;
    int orbit;
    LfoSlot vlfo[LFO_SLOTS];
    double lfo_phase[LFO_SLOTS]; /* advancing phase in radians */
    double lfo_hold; /* onset quantum span with unmodulated output (seconds) */
    int has_orbit_lfo; /* voice drives the shared orbit delay line */
    double orbit_lfo_end; /* absolute time through which its shared-line LFO renders */
    double release_start_level;
    double release_time;
    double gate_end;
    double graph_end;  /* original cleanup deadline, independent of source stop */
    double graph_gate_end, graph_fade_start; /* immutable original hap anchors */
    double fx_release;
    int source_finished; /* drain downstream effects with zero input */
    // wavetable state (valid when inst == INST_WT)
    int wt;
    double wtpos; // table position in [0, WT_SIZE)
    /* Multi-frame sample wavetable (Superdough wt_* samples). */
    int wt_sample_frames, wt_sample_frame_len;
    double wt_sample_position, wt_sample_warp, wt_sample_lfo_hz, wt_sample_lfo_depth;
    double wt_sample_detune, wt_sample_spread, wt_sample_lfo_skew, wt_sample_lfo_dc;
    double wt_sample_env_amount, wt_sample_env[4], wt_sample_warp_env_amount, wt_sample_warp_env[4];
    double wt_sample_warp_lfo_hz, wt_sample_warp_lfo_depth, wt_sample_warp_lfo_skew, wt_sample_warp_lfo_dc;
    double wt_sample_phase_rand;
    int wt_sample_warp_mode, wt_sample_voices, wt_sample_lfo_shape;
    int wt_sample_warp_lfo_shape;
    double wt_sample_phase[WT_SAMPLE_VOICES];
    double wt_sample_lfo_phase, wt_sample_warp_lfo_phase;
    // one-shot sample state (valid when smp >= 0)
    int smp;
    float *smpbuf;
    int smp_channels;
    int smplen;
    double smppos;  // fractional position for pitch-shifted playback
    double smpstep; // advance per output sample (1 = original pitch)
    double sample_pitch_quantum, sample_pitch_ratio; /* BufferSource k-rate detune */
    double sample_viblfo_quantum, sample_viblfo_ratio; /* k-rate detune with integrated rate LFO */
    int smp_gate;   // respect note duration + release (melodic banks)
    double stretch_factor;
    const StretchBuffer *stretch_buffer;
    unsigned stretch_frame;
    /* Streaming phase-vocoder render (plain osc + stretch): pool slot +1,
       or 0 for none (zero survives pool wipes). The source runs on the
       absolute 128-sample grid so the transform matches the deployed
       per-quantum processing. */
    int live_voc;
    long long live_first; /* absolute onset sample of the first render */
    long long live_next;  /* absolute start of the next block to refill */
    long long live_frame; /* absolute sample the next drain renders */
    char token[24]; /* printable label for music_debug; not a path */
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
/* Session-level final stereo bus attenuation, after all voices/effects.
   Finite 0..1 only; lock-free for the callback. Persists across evaluations. */
int audio_set_output_gain(double value);
double audio_get_output_gain(void);
void audio_set_block(unsigned long n);
int audio_is_stream_active(void);
const char *audio_backend_name(void);
/* master fx chain params (drive/cutoff/delay/verb), each 0..1 */
void audio_fx_set(const char *name, double v);
double audio_sec_per_beat(void);
double audio_sec_per_cycle(void);
double audio_cycle_beats(void);

/* trigger a voice from an event; time is event time (may be <=now, trigger immediately) */
void audio_trigger_event(AudioEvent *ev);
/* Drain callback diagnostics from a non-audio thread, including while stopped. */
void audio_report_errors(void);
/* Producer-only validation before committing source/effect preparation. */
int audio_prepare_duck_timer(int orbit,double time);
int audio_prepare_delay(AudioEvent *ev);
void audio_prepare_room(AudioEvent *ev);
/* Offline verification only; never call from the live scheduler or callback. */
void audio_wait_room_preparation(void);
unsigned audio_register_orbit(int orbit);
/* Bits 0..5 specify explicit size/fade/low-pass/dimension/speed/offset controls. */
void audio_inherit_room(AudioEvent *ev, unsigned explicit_fields);

/* Read-only text snapshot for music_debug. limit is rows per section (clamped 1..48).
   Returns bytes written, excluding the NUL, or -1. No paths or sample URLs. */
int audio_debug_snapshot(char *out, size_t outlen, int limit);

/* nerd-pane stats (thread-safe snapshots) */
int audio_active_voices(void);
/* Highest absolute output sample since the last reset/play start. */
void audio_peak_reset(void);
double audio_peak_level(void);
/* Priority of the thread currently rendering audio; 0 when not real-time,
   -1 before the first render block. */
int audio_render_realtime(void);
int audio_reference_mode(void);
#ifdef TOMPOUCE_RENDER_PROFILE
void audio_section_profile_stats(unsigned long long *voice,
    unsigned long long *fx, unsigned long long *master);
void audio_hot_profile_stats(unsigned long long *source, unsigned long long *route,
    unsigned long long *scalls, unsigned long long *rcalls);
#endif
int audio_retiring_voices(void);
void audio_voice_stats(int *active_out, int by_inst[10]);
const char *audio_inst_name(int inst);
/* recent master output, oldest first (for the scope); returns count */
int audio_scope_copy(float *out, int n);
int audio_pattern_scope_copy(unsigned mask, unsigned generation, float *out, int n);
/* Per-pattern analyze(id) taps; id is 1..N_ANALYSERS. */
int audio_analyser_copy(int analyser, unsigned generation, float *out, int n);
int audio_analyser_fft_size(int analyser);

int note_name_to_midi(const char *name);
double midi_to_freq(int midi);

#endif
