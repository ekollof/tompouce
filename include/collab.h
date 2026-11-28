#ifndef COLLAB_H
#define COLLAB_H
#include <stddef.h>
#include <stdint.h>
/* Collab session (docs/multiplayer.md sections 6, 10, 12, 13, 18).
   Phase 0: lifecycle skeleton, transport handshake (hello/welcome) and the
   local nickname. No scheduler, compile or UI integration yet: with no room
   joined the engine behaves exactly as without collab. */
typedef enum {
    COLLAB_IDLE=0,
    COLLAB_CONNECTING,
    COLLAB_CONNECTED,
    COLLAB_BACKOFF,
    COLLAB_FAILED
} CollabState;
typedef struct {
    char url[512];
    char nick_override[64];
} CollabConfig;
const char *collab_state_string(CollabState state);
CollabState collab_state(void);
/* Opens the transport and exchanges hello/welcome. Returns nonzero on
   success (state CONNECTED); on failure fills err and enters FAILED. */
int collab_connect(const CollabConfig *cfg, char *err, size_t errlen);
void collab_disconnect(void);
/* Nickname (D14, sections 12.1, 13.5, 18.3): one value per user, stored in
   collab.json, reused in every room. Empty override means stored/generated. */
int collab_nick_get(char *buf, size_t cap);
int collab_nick_set(const char *name, char *err, size_t errlen);
/* Validation only (no persistence): 0 when name is acceptable. */
int collab_nick_check(const char *name, char *err, size_t errlen);
/* Capability gates (section 13.2), checked on the generator before
   preparation. Slot 0 is local and never gated. Remote slots are denied
   by default; per-room consent allowlists arrive with rooms (Phase 2).
   The test hook forces remote slots open; production never calls it. */
typedef enum {
    COLLAB_CAP_OSC=0,
    COLLAB_CAP_MIDI_OUT=1
} CollabCap;
#ifdef HAVE_COLLAB
int collab_capability_allowed(uint16_t slot, CollabCap cap,
    const char *target);
void collab_capability_set_test(int allow_remote);
/* Consent allowlist (section 13.2, 18.3): per capability and exact target,
   scoped to a room tag, persisted in collab.json under "allow". A NULL
   target never matches (call sites must pass the real target). Returns
   nonzero on success. */
int collab_allow(CollabCap cap, const char *room, const char *target);
int collab_deny(CollabCap cap, const char *room, const char *target);
/* Re-reads collab.json into the in-memory allowlist (persistence check). */
void collab_consent_reload(void);
/* Current room tag for consent scoping (connected URL, else ""). Consent
   never applies without a room. */
const char *collab_room_tag(void);
/* Hap authorship (M1 namespaced ids "N/..." or bare "N"): leading slot
   number, or 0 when absent/malformed. */
int collab_slot_from_id(const char *id);
/* Room composite (sections 9.4, 10.3): per-slot last-evaluated bodies in
   ascending slot order. Remote bodies compile under the remote profile
   whenever any slot is remote (including shared slot 0); all-local rooms
   keep the local profile. A failing slot falls back to its last good body
   with one recompile so one typo never silences the room; when every
   slot fails the previous program keeps playing. On success the composite
   is staged at at_cycle (must be finite) with eval_seq for the shared
   activation path. Not a transport action: callers feed it decrypted eval
   bodies. Returns nonzero when a program was staged. */
void collab_room_reset(void);
int collab_room_set_slot(int slot, const char *body, size_t bodylen,
    int remote);
const char *collab_room_slot_error(int slot);
int collab_room_apply(double at_cycle, unsigned eval_seq, char *err,
    size_t errlen);
#else
static inline int collab_capability_allowed(uint16_t slot, CollabCap cap,
    const char *target) {
    (void)slot;
    (void)cap;
    (void)target;
    return 1;
}
static inline void collab_capability_set_test(int allow_remote) {
    (void)allow_remote;
}
static inline int collab_allow(CollabCap cap, const char *room,
    const char *target) {
    (void)cap; (void)room; (void)target; return 0;
}
static inline int collab_deny(CollabCap cap, const char *room,
    const char *target) {
    (void)cap; (void)room; (void)target; return 0;
}
static inline void collab_consent_reload(void) {}
static inline const char *collab_room_tag(void) { return ""; }
static inline int collab_slot_from_id(const char *id) {
    (void)id; return 0;
}
static inline void collab_room_reset(void) {}
static inline int collab_room_set_slot(int slot, const char *body,
    size_t bodylen, int remote) {
    (void)slot; (void)body; (void)bodylen; (void)remote; return 0;
}
static inline const char *collab_room_slot_error(int slot) {
    (void)slot; return NULL;
}
static inline int collab_room_apply(double at_cycle, unsigned eval_seq,
    char *err, size_t errlen) {
    (void)at_cycle; (void)eval_seq; (void)err; (void)errlen; return 0;
}
#endif
#endif
