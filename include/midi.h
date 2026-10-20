#ifndef MIDI_H
#define MIDI_H

/* ALSA sequencer MIDI out. Init is best-effort: without a sequencer
   everything below is a silent no-op (logged once). */
void midi_init(void);
void midi_shutdown(void);
int midi_ready(void);
/* note on now, note off after dur_ms (worker thread). Thread-safe. */
void midi_note(int chan, int note, int vel, int dur_ms);
/* Bounded scheduling observation for tests (independent of hardware).
   Enable to start a fresh capture; read back up to capacity entries in
   send order. Returns the total captured (may exceed capacity). */
enum { MIDI_CAPTURE_CAPACITY = 4096 };
typedef struct MidiCapturedNote { int on, chan, note, vel; double at; } MidiCapturedNote;
void midi_capture_enable(int on);
unsigned midi_captured_notes(MidiCapturedNote *out, unsigned capacity);

#endif
