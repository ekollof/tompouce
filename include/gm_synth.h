#ifndef GM_SYNTH_H
#define GM_SYNTH_H
#include "event.h"
#include <cjson/cJSON.h>
#include <stddef.h>
int gm_synth_count(void);
const char *gm_synth_name(int index);
int gm_synth_program(const char *name);
int gm_synth_available(void);
/* Generator only. Assigns owned immutable PCM; audio never calls FluidSynth. */
int gm_synth_prepare(AudioEvent *event);
/* WebAudioFont pack path: resolve event->token and numeric n to the same
   per-instrument sample pack the Strudel 1.2.6 web REPL plays, acquire the
   key-range zone as a pitch-keyed sample, and set its sustain-loop fractions.
   Returns 1 with ev->smp set, 0 when the caller should fall back (FluidSynth). */
int gm_synth_prepare_sample(AudioEvent *event, const cJSON *value, double midi);
/* Pack stem for name at Strudel's getSoundIndex(n). NULL when name is outside
   the pinned table or the selected stem is not a pack file. Non-finite n
   selects index 0. */
const char *gm_synth_font(const char *name, double n);
/* 1 when every pinned name's index 0 equals the historical first-font table. */
int gm_synth_default_fonts_match(void);

/* Confined webaudiofont pack helpers (no network, no filesystem). The pack
   text is the fetched pack JS; zones reference slices of it, so the text must
   outlive the zones. */
#define GM_WAF_MAX_ZONES 128
typedef struct {
    int low, high;      /* key range, inclusive, 0..127 */
    int original_pitch; /* recorded pitch, MIDI cents/100 */
    int coarse, fine;   /* tuning, semitones and cents */
    int sample_rate;
    long loop_start, loop_end; /* sustain loop, decoded-frame units; disabled when loop_end <= loop_start */
    const char *file;   /* base64 MP3 slice of the pack text, not NUL-terminated */
    size_t file_len;
} GmWafZone;
/* Strict base64 into a caller buffer. Returns decoded length, -1 on any
   violation (non-alphabet bytes, misplaced padding) or capacity overflow. */
int gm_waf_b64_decode(const char *text, size_t len, unsigned char *out, size_t capacity);
/* Parse pack zones. Returns the zone count (0 when none found), -1 when the
   text itself is invalid or over bounds. Unparseable zones are skipped. */
int gm_waf_parse(const char *text, GmWafZone *zones, int capacity);
/* Range-based zone selection (the web selects by key ranges, not by nearest
   root). Returns the zone index, or -1 when no range covers note. */
int gm_waf_pick(const GmWafZone *zones, int n, double note);
/* Effective recorded root, verbatim from the web player: originalPitch minus
   100*coarseTune minus fineTune, over 100, rounded to the integer base MIDI
   the cached WAV is acquired at. */
int gm_waf_base_midi(const GmWafZone *zone);
/* Web playback rate for a note, verbatim: 2^((100*note - e)/1200) with
   e = originalPitch - 100*coarseTune - fineTune. The native voice plays the
   cached WAV at ev->freq/midi_to_freq(base) times smp_speed, so the WAV stays
   a pristine (gain-staged only) copy of the trimmed decode and this factor
   closes the remaining rate gap exactly. It is note-independent: the note
   cancels out, leaving 2^((100*base - e)/1200). */
double gm_waf_speed_correction(const GmWafZone *zone, int base);
/* Sustain-loop fractions of the final buffer. The web sets loop seconds as
   loopStart/zoneRate on the resampled buffer, so pack indices convert through
   the zone rate, not the MP3 rate (packs like Aspirin declare e.g. 29999 Hz
   zones holding 32000 Hz MP3). With the buffer in seconds
   (decodedFrames/decodedRate), fractions are (loopStart/zoneRate)/seconds.
   Zone and MP3 rates are equal for JCLive, where this reduces to absolute
   indices. end may exceed 1: like the web, the voice then plays to the buffer
   end instead of wrapping. Returns 1, or 0 when looping is disabled. */
int gm_waf_loop_fractions(const GmWafZone *zone, long decoded_frames, long decoded_rate,
    double *lo, double *hi);
#endif
