#ifndef COLLAB_CRYPTO_H
#define COLLAB_CRYPTO_H
#include <stddef.h>
#include <stdint.h>
/* E2E crypto for Jam mode (docs/multiplayer.md sections 13.5, 14.1, 14.5).
   libsodium primitives: Ed25519 member keys, XChaCha20-Poly1305-IETF seals,
   crypto_kdf subkeys, keyed BLAKE2b admission proofs. The director never
   decrypts; it checks signatures, proofs and epochs only. Verified against
   the director's checks in the loopback test, and against shared vectors
   (tests/vectors/collab-crypto.json) for the web front end (M6). */
#define CCR_SEED_BYTES 32
#define CCR_PK_BYTES 32
#define CCR_SK_BYTES 64
#define CCR_SIG_BYTES 64
#define CCR_NONCE_BYTES 24
#define CCR_TAG_BYTES 16
#define CCR_KEY_BYTES 32
#define CCR_ROOM_ID_LEN 26
#define CCR_CHALLENGE_BYTES 32
/* KDF contexts (8 bytes each) and subkey ids. Admission uses id 0 under
   "tpcadm01" (pinned by the director); frames/controls/blobs use ids
   1/2/3 under "tpcsub01". Member seeds use "tpcroom1" with the id below. */
#define CCR_KDF_ADMISSION "tpcadm01"
#define CCR_KDF_SUBKEYS "tpcsub01"
#define CCR_KDF_MEMBER "tpcroom1"
/* Control record types (§14.5): 1 eval, 2 profile, 3 slot name, 4 chat,
   5 manifest, 6 slider. */
#define CCR_CTRL_EVAL 1
#define CCR_CTRL_PROFILE 2
int ccr_init(void);
/* Room id shape: 26 chars [a-z2-7]. */
int ccr_room_ok(const char *room_id);
/* Master seed (section 14.1): load or create
   <config>/tompouce/collab/identity.key (0600, 32 random bytes). */
int ccr_identity_load_or_create(uint8_t seed[CCR_SEED_BYTES], char *err,
    size_t errlen);
/* Per-room member keypair: Ed25519 seed = kdf(master, subkey, "tpcroom1")
   where subkey is LE64(SHA-256(room_id)[0..8]). Returns 0 on bad room id. */
int ccr_room_keypair(const uint8_t master[CCR_SEED_BYTES],
    const char *room_id, uint8_t pk[CCR_PK_BYTES],
    uint8_t sk[CCR_SK_BYTES]);
/* Admission key: kdf(room_key, id 0, "tpcadm01"). */
void ccr_admission_key(const uint8_t room_key[CCR_KEY_BYTES],
    uint8_t out[CCR_KEY_BYTES]);
/* Room content subkeys: kdf(room_key, id, "tpcsub01"), id 1 frames,
   2 control records, 3 blobs. */
void ccr_subkey(const uint8_t room_key[CCR_KEY_BYTES], uint64_t id,
    uint8_t out[CCR_KEY_BYTES]);
/* auth signature: Ed25519(sk, "tompouce-collab-v1" ‖ challenge ‖ room ‖
   pk). room_id is the 26 raw id bytes (no NUL). */
int ccr_auth_sign(const uint8_t sk[CCR_SK_BYTES],
    const uint8_t challenge[CCR_CHALLENGE_BYTES], const char *room_id,
    const uint8_t pk[CCR_PK_BYTES], uint8_t sig[CCR_SIG_BYTES]);
int ccr_auth_verify(const uint8_t pk[CCR_PK_BYTES],
    const uint8_t challenge[CCR_CHALLENGE_BYTES], const char *room_id,
    const uint8_t sig[CCR_SIG_BYTES]);
/* Phase-1 bootstrap proof: keyed BLAKE2b(admission_key, challenge ‖ room ‖
   pk), 32 bytes. */
int ccr_proof(const uint8_t admission[CCR_KEY_BYTES],
    const uint8_t challenge[CCR_CHALLENGE_BYTES], const char *room_id,
    const uint8_t pk[CCR_PK_BYTES], uint8_t mac[CCR_KEY_BYTES]);
/* Control records: enc = epoch(u16BE) ‖ nonce ‖ ct ‖ tag ‖ sig with
   AD = "tpc-ctrl-v1" ‖ room ‖ epoch ‖ type ‖ slot ‖ author_pk. seal
   randomizes the nonce; seal_nonce takes an explicit one (vectors).
   Returns 1 on success (malloc'd *out), 0 on bad input/no mem. */
int ccr_seal_ctrl(const uint8_t key[CCR_KEY_BYTES], const char *room_id,
    uint16_t epoch, uint8_t type, uint16_t slot,
    const uint8_t author_pk[CCR_PK_BYTES],
    const uint8_t author_sk[CCR_SK_BYTES], const uint8_t *msg, size_t msglen,
    uint8_t **out, size_t *outlen);
int ccr_seal_ctrl_nonce(const uint8_t key[CCR_KEY_BYTES], const char *room_id,
    uint16_t epoch, uint8_t type, uint16_t slot,
    const uint8_t author_pk[CCR_PK_BYTES],
    const uint8_t author_sk[CCR_SK_BYTES],
    const uint8_t nonce[CCR_NONCE_BYTES], const uint8_t *msg, size_t msglen,
    uint8_t **out, size_t *outlen);
/* Typed open: the caller supplies the expected epoch/type/slot from the
   director's plaintext routing fields; they are verified as part of the
   AD. Returns 1 and a malloc'd plaintext; 0 schema, -1 bad signature,
   -2 undecryptable. */
int ccr_open_ctrl_typed(const uint8_t key[CCR_KEY_BYTES], const char *room_id,
    uint16_t exp_epoch, uint8_t exp_type, uint16_t exp_slot,
    const uint8_t author_pk[CCR_PK_BYTES], const uint8_t *in, size_t inlen,
    uint8_t **msg, size_t *msglen, uint16_t *got_epoch, uint8_t *got_type,
    uint16_t *got_slot);
/* Binary frames: payload = nonce ‖ ct ‖ tag ‖ sig with AD =
   "tpc-frame-v1" ‖ room ‖ epoch ‖ slot ‖ kind ‖ seq ‖ author_pk. */
int ccr_seal_frame(const uint8_t key[CCR_KEY_BYTES], const char *room_id,
    uint16_t epoch, uint16_t slot, uint8_t kind, uint32_t seq,
    const uint8_t author_pk[CCR_PK_BYTES],
    const uint8_t author_sk[CCR_SK_BYTES], const uint8_t *payload,
    size_t payloadlen, uint8_t **out, size_t *outlen);
int ccr_seal_frame_nonce(const uint8_t key[CCR_KEY_BYTES], const char *room_id,
    uint16_t epoch, uint16_t slot, uint8_t kind, uint32_t seq,
    const uint8_t author_pk[CCR_PK_BYTES],
    const uint8_t author_sk[CCR_SK_BYTES],
    const uint8_t nonce[CCR_NONCE_BYTES], const uint8_t *payload,
    size_t payloadlen, uint8_t **out, size_t *outlen);
int ccr_open_frame(const uint8_t key[CCR_KEY_BYTES], const char *room_id,
    uint16_t epoch, uint16_t slot, uint8_t kind, uint32_t seq,
    const uint8_t author_pk[CCR_PK_BYTES], const uint8_t *in, size_t inlen,
    uint8_t **payload, size_t *payloadlen);
/* Eval record JSON: {"v":1,"slot":N,"seq":N,"body":"..."}. Body capped at
   CP_EVAL_BODY_MAX (256 KiB); seq fits in 2^53. Returns malloc'd text. */
char *ccr_eval_json(int slot, uint64_t seq, const char *body);
int ccr_eval_parse(const char *text, size_t len, int *slot, uint64_t *seq,
    char **body);
/* Profile record JSON: {"v":1,"nick":"..."} with optional "color".
   Nick is 1..32 bytes (semantic validation happens at apply time via
   collab_nick_check). */
char *ccr_profile_json(const char *nick, const char *color_or_null);
int ccr_profile_parse(const char *text, size_t len, char *nick,
    size_t nickcap, char *color, size_t colorcap);
/* Short fingerprint for member lists: first 10 bytes of SHA-256(pk) as
   16 lowercase base32 chars (plus NUL). */
void ccr_fingerprint(const uint8_t pk[CCR_PK_BYTES], char out[17]);
#endif
