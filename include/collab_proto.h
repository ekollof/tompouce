#ifndef COLLAB_PROTO_H
#define COLLAB_PROTO_H
#include <stddef.h>
#include <stdint.h>
/* Wire codec for tompouce.collab.v1 (docs/multiplayer.md section 7).
   Pure computation, no I/O: the same translation unit compiles into the
   engine and into tompouce-director so both sides validate identically.
   Limits below are the section 7.1 table; oversize frames are refused
   before any dispatch. */
#define CP_PROTO_MAJOR 1
#define CP_SUBPROTOCOL "tompouce.collab.v1"
#define CP_WS_PATH "/v1"
#define CP_CONTROL_MAX 16384
#define CP_EVAL_BODY_MAX (256*1024)
#define CP_COMPOSITE_MAX (1024*1024)
#define CP_JOINED_MAX (2*1024*1024)
#define CP_BIN_HEADER 20
#define CP_BIN_PAYLOAD_MAX 65536
#define CP_SNAPSHOT_MAX (1024*1024)
#define CP_JSON_DEPTH_MAX 8
#define CP_ROOM_ID_LEN 26
#define CP_MEMBERS_MAX 16
#define CP_SLOTS_MAX 16
#define CP_ROOMS_MAX 256
#define CP_WS_CLOSE_POLICY 1008
#define CP_WS_CLOSE_OVERSIZE 1009
typedef enum {
    CP_T_UNKNOWN=0,
    CP_T_HELLO, CP_T_AUTH, CP_T_JOIN, CP_T_PING, CP_T_EVAL_REQ,
    CP_T_TEMPO_REQ, CP_T_TRANSPORT_REQ, CP_T_SLOT_CREATE, CP_T_SLOT_UPDATE,
    CP_T_SLOT_DELETE, CP_T_ACK, CP_T_SLIDER, CP_T_INVITE_CREATE,
    CP_T_INVITE_REVOKE, CP_T_INVITE_LIST, CP_T_MEMBER_KICK, CP_T_MEMBER_BAN,
    CP_T_REKEY, CP_T_REKEY_ACK, CP_T_ROOM_UPDATE, CP_T_ROOM_DELETE,
    CP_T_SEALED_MSG, CP_T_LEAVE,
    CP_T_WELCOME, CP_T_AUTH_OK, CP_T_JOINED, CP_T_SYNCED, CP_T_PONG,
    CP_T_CLOCK, CP_T_EVAL, CP_T_SNAPSHOT_REQ, CP_T_MEMBER_JOINED,
    CP_T_MEMBER_LEFT, CP_T_MEMBER_UPDATED, CP_T_MEMBER_KICKED, CP_T_REKEY_OUT,
    CP_T_SLOT_CREATED, CP_T_SLOT_UPDATED, CP_T_SLOT_DELETED, CP_T_INVITES,
    CP_T_ERROR, CP_T_KICKED, CP_T_ROOM_UPDATED
} CpType;
/* Error codes from the section 7.3 table. */
typedef enum {
    CP_E_NONE=0,
    CP_E_PROTO, CP_E_TYPE, CP_E_SCHEMA, CP_E_AUTH, CP_E_PERM, CP_E_RATE,
    CP_E_SIZE, CP_E_ROOM_FULL, CP_E_LOCKED, CP_E_BANNED, CP_E_NOT_FOUND,
    CP_E_E2E_REQUIRED, CP_E_INVITE_INVALID, CP_E_EPOCH, CP_E_QUOTA,
    CP_E_INTERNAL
} CpError;
const char *cp_error_string(CpError error);
CpType cp_type_from_string(const char *name);
const char *cp_type_string(CpType type);
/* Parsed control frame. `text` is copied; the caller releases `owned`
   with cJSON_Delete. Returns a CpError (CP_E_NONE on success). */
typedef struct {
    CpType type;
    uint32_t id;
    int has_id;
    void *owned;
} CpMsg;
CpError cp_parse_control(const char *text, size_t len, CpMsg *out);
void cp_msg_free(CpMsg *msg);
/* Field accessors validate type, range and length. Strings are length
   capped; room ids additionally match [a-z2-7]{26}. */
int cp_get_string(const void *obj, const char *key, const char **value,
    size_t maxlen);
int cp_get_room_id(const void *obj, const char *key, const char **value);
int cp_get_u32(const void *obj, const char *key, uint32_t *value);
int cp_get_bool(const void *obj, const char *key, int *value);
int cp_get_double(const void *obj, const char *key, double *value);
/* Cycle pair [numerator, denominator]; denominator must be one of
   1,2,4,8,16,32,64 and the numerator an integer in [0, 2^53). */
int cp_get_cycle(const void *obj, const char *key, uint64_t *num,
    unsigned *den);
/* Base64url without padding; decoded length must fit maxlen. */
int cp_get_b64(const void *obj, const char *key, uint8_t *out,
    size_t maxlen, size_t *outlen);
size_t cp_b64_encode(const uint8_t *in, size_t len, char *out, size_t cap);
int cp_b64_decode(const char *in, uint8_t *out, size_t cap, size_t *outlen);
uint64_t cp_cycle_to_double_num(uint64_t num, unsigned den, double *value);
/* Binary frame header (section 7.4). All integers are big-endian on the
   wire. Returns a CpError; on success *payload points inside frame. */
typedef struct {
    uint8_t kind;
    uint16_t slot;
    uint32_t member;
    uint32_t seq;
    uint16_t epoch;
    uint8_t flags;
} CpBinHeader;
#define CP_BIN_VER 1
#define CP_BIN_FLAG_PLAINTEXT 0x01
CpError cp_parse_binary(const uint8_t *frame, size_t len, CpBinHeader *hdr,
    const uint8_t **payload, size_t *payload_len);
#endif
