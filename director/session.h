#ifndef DIRECTOR_SESSION_H
#define DIRECTOR_SESSION_H
#include "room.h"
#include <stddef.h>
#include <stdint.h>
struct lws;
/* Per-connection state. The room layer never sees sockets; this layer
   never interprets sealed payloads. */
typedef struct CpTx CpTx;
struct CpTx {
    uint8_t *data;
    size_t len;
    size_t sent;
    int binary;
    CpTx *next;
};
typedef struct {
    struct lws *wsi;
    int phase;
    uint64_t connected_us;
    uint64_t last_rx_us;
    uint8_t challenge[32];
    DirectorRoom *room;
    DirectorMember *member;
    uint8_t *rx;
    size_t rx_len;
    size_t rx_cap;
    int rx_binary;
    int rx_oversize;
    DirectorBucket bucket;
    CpTx *tx_head;
    CpTx *tx_tail;
    int close_code;
    const char *close_msg;
} DirectorSession;
typedef struct {
    const char *listen_host;
    int listen_port;
    uint8_t admission_key[32];
    int has_admission_key;
    int allow_plaintext;
    const char *server_name;
    const char *server_version;
    const char *server_source;
} DirectorConfig;
void director_sessions_init(void);
DirectorSession *director_session_create(struct lws *wsi);
void director_session_free(DirectorSession *session);
DirectorSession *director_session_lookup(struct lws *wsi);
/* Connection lost or explicit leave: broadcast presence, keep or drop the
   member record. */
void director_session_closed(DirectorSession *session, int keep_member);
/* Queue a control frame (text) or binary payload for writing. */
int director_session_send(DirectorSession *session, const char *text,
    size_t len);
int director_session_send_binary(DirectorSession *session, const uint8_t *data,
    size_t len);
/* Drain the TX queue. Returns 1 if more remains. */
int director_session_flush(DirectorSession *session);
/* Inbound bytes from the lws callback. */
void director_session_receive(DirectorSession *session, const uint8_t *data,
    size_t len, int binary, int first, int final,
    const DirectorConfig *config);
/* Periodic sweep: hello timeouts, pending tempo epochs, idle members. */
void director_session_sweep(uint64_t now_us, const DirectorConfig *config);
/* Fan-out helpers used by dispatch. */
void director_broadcast_text(DirectorRoom *room, DirectorSession *except,
    const char *text, size_t len);
void director_broadcast_binary(DirectorRoom *room, DirectorSession *except,
    const uint8_t *frame, size_t len);
#endif
