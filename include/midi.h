#ifndef MIDI_H
#define MIDI_H

/* ALSA sequencer MIDI out. Init is best-effort: without a sequencer
   sending below is a silent no-op (logged once). */
void midi_init(void);
void midi_shutdown(void);
int midi_ready(void);
/* note on now, note off after dur_ms (worker thread). Thread-safe. */
void midi_note(int chan, int note, int vel, int dur_ms);
typedef struct MidiHeldKey { int note, chan; double vel, at; int consumed; } MidiHeldKey;
/* Controller input. Values are normalized 0..1 like the web input; channels
   are 1..16 with 0 selecting the merged stream, and anything else reads 0.
   Held keyboard notes carry arrival times for query windows to consume once.
   midi_open_input subscribes a sender by "client:port", numeric client id,
   name substring or ordinal index, returning 1 when events can arrive;
   midi_input_live reports a currently connected sender. */
double midi_cc_value(int cc, int chan);
unsigned midi_take_keys(MidiHeldKey *out, unsigned capacity, double horizon);
int midi_open_input(const char *name, int ordinal);
int midi_input_live(void);
void midi_test_inject(int type, int chan, int p1, int p2);
/* Bounded scheduling observation for tests (independent of hardware).
   Enable to start a fresh capture; read back up to capacity entries in
   send order. Returns the total captured (may exceed capacity). */
enum { MIDI_CAPTURE_CAPACITY = 4096 };
typedef struct MidiCapturedNote { int on, chan, note, vel; double at; } MidiCapturedNote;
void midi_capture_enable(int on);
unsigned midi_captured_notes(MidiCapturedNote *out, unsigned capacity);

#endif
