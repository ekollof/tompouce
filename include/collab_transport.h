#ifndef COLLAB_TRANSPORT_H
#define COLLAB_TRANSPORT_H
#include <stddef.h>
#include <stdint.h>
/* WebSocket client over libcurl (docs/multiplayer.md section 6.2).
   CONNECT_ONLY with curl_ws_send/curl_ws_recv; the same pattern the
   loopback integration test uses against the real director. */
typedef struct CollabTransport CollabTransport;
/* Runtime floor: libcurl >= 8.11.0 with the "wss" protocol. Fills err on
   failure; returns nonzero when a collab connection may be attempted. */
int ctr_websocket_supported(char *err, size_t errlen);
/* URL policy, testable without connecting: ws:// stays on loopback unless
   TOMPOUCE_COLLAB_INSECURE=1 opts into LAN use; wss:// may go anywhere.
   Sets *insecure_used when the opt-in applied. */
int ctr_ws_policy_ok(const char *url, int *insecure_used, char *err,
    size_t errlen);
/* Opens a ws:// or wss:// URL offering subprotocol (for example
   CP_SUBPROTOCOL). Returns NULL with err set on failure. */
CollabTransport *ctr_open(const char *url, const char *subprotocol,
    char *err, size_t errlen);
int ctr_send_text(CollabTransport *ct, const char *text, size_t len);
int ctr_send_binary(CollabTransport *ct, const uint8_t *data, size_t len);
/* One complete incoming frame; *out is malloc'd (caller frees). Returns 1
   on a frame, 0 on timeout/close/error. timeout_ms < 0 waits up to the
   transport deadline (10 s). */
int ctr_recv(CollabTransport *ct, int *is_binary, uint8_t **out,
    size_t *outlen, int timeout_ms);
/* Pollable socket for UI wakeups, or -1 when unavailable. Reads must still
   go through ctr_recv: libcurl buffers readahead the socket cannot see. */
int ctr_fd(CollabTransport *ct);
void ctr_close(CollabTransport *ct);
#endif
