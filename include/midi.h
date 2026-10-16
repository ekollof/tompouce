#ifndef MIDI_H
#define MIDI_H

/* ALSA sequencer MIDI out. Init is best-effort: without a sequencer
   everything below is a silent no-op (logged once). */
void midi_init(void);
void midi_shutdown(void);
int midi_ready(void);
/* note on now, note off after dur_ms (worker thread). Thread-safe. */
void midi_note(int chan, int note, int vel, int dur_ms);

#endif
