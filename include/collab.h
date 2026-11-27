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
#endif
#endif
