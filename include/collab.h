#ifndef COLLAB_H
#define COLLAB_H
#include <stddef.h>
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
#endif
