#ifndef MIDI_H
#define MIDI_H

/* ALSA sequencer MIDI out. Init is best-effort: without a sequencer
   sending below is a silent no-op (logged once). */
void midi_init(void);
void midi_shutdown(void);
int midi_ready(void);
/* note on now, note off after dur_ms (worker thread). Thread-safe. */
void midi_note(int chan, int note, int vel, int dur_ms);
/* Dominant MIDI extras, matching the deployed web output's message set.
   Channel is 0-15; sends are best-effort like midi_note and always recorded
   in the scheduling capture when enabled. */
void midi_cc(int chan, int cc, int value);
void midi_program(int chan, int prog);
void midi_bend(int chan, int value); /* -8192..8191, zero-centered */
void midi_touch(int chan, int value);
void midi_sysex(const unsigned char *id, int id_len, const unsigned char *data, int len);
void midi_transport(int cmd); /* 1 clock, 2 start, 3 continue, 4 stop */
typedef struct MidiHeldKey { int note, chan; double vel, at; int consumed; } MidiHeldKey;
/* Controller input. Values are normalized 0..1 like the web input; channels
   are 1..16 with 0 selecting the merged stream, and anything else reads 0.
   Held keyboard notes carry arrival times for query windows to consume once.
   midi_open_input subscribes a sender by "client:port", numeric client id,
   name substring or ordinal index, returning 1 when events can arrive;
   midi_input_live reports a currently connected sender. */
double midi_cc_value(int cc, int chan);
unsigned midi_take_keys(MidiHeldKey *out, unsigned capacity, double horizon);
/* Inspect pending notes without consuming them (foreground visual queries). */
unsigned midi_peek_keys(MidiHeldKey *out, unsigned capacity);
int midi_open_input(const char *name, int ordinal);
int midi_input_live(void);
void midi_test_inject(int type, int chan, int p1, int p2);
/* Persisted CC state, mirroring the web input's per-device storage: values
   persist per string sender name (numeric/ordinal opens never persist) and
   load as reader defaults on open, even without hardware. Returns loaded
   entries, or -1 without a usable name. */
int midi_state_load(const char *name);
/* Record a received CC for the sender at client:port (no-op when the sender
   is unknown or unnamed). Thread-safe. */
void midi_state_saved_cc(int client, int port, int chan, int cc, double value);
/* Resend a sender's persisted CC values to its output port, restoring
   motorized encoders and sliders like the web input on connect. Senders
   are keyed by stable client name. Best effort; returns sent count,
   0 without hardware. */
int midi_restore_sender(const char *client_name);
/* Reconnect matching without hardware: mark senders dead, or report the
   client-name match that gates a revive (resubscribe needs hardware).
   Returns 1 on a dead-mark or a match. */
int midi_test_sender_alive(int client, int port);
int midi_test_announce(int added, int client, int port, const char *client_name);
/* Test hooks: fake a subscription record; inject an event as if from the
   sender at client:port. */
void midi_test_add_sender(int client, int port, const char *name, int named);
void midi_test_inject_from(int client, int port, int type, int chan, int p1, int p2);
/* Bounded scheduling observation for tests (independent of hardware).
   Enable to start a fresh capture; read back up to capacity entries in
   send order. Returns the total captured (may exceed capacity). */
enum { MIDI_CAPTURE_CAPACITY = 4096 };
enum { MIDI_CAP_NOTE_ON = 0, MIDI_CAP_NOTE_OFF, MIDI_CAP_CC, MIDI_CAP_PROG,
    MIDI_CAP_BEND, MIDI_CAP_TOUCH, MIDI_CAP_SYSEX, MIDI_CAP_TRANSPORT };
typedef struct MidiCapturedNote { int on, chan, note, vel; double at; } MidiCapturedNote;
/* Generic record: CC uses d1=cc d2=value; program uses d1=prog; bend uses
   d1=value (-8192..8191); touch uses d1=value; transport uses d1=cmd
   (1 clock, 2 start, 3 continue, 4 stop); sysex carries the framed
   F0..F7 bytes in sysex. */
typedef struct MidiCaptured { int kind, chan, d1, d2; unsigned char sysex[264]; int sysex_len; double at; } MidiCaptured;
void midi_capture_enable(int on);
unsigned midi_captured_notes(MidiCapturedNote *out, unsigned capacity);
unsigned midi_captured_all(MidiCaptured *out, unsigned capacity);

#endif
