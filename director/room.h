#ifndef DIRECTOR_ROOM_H
#define DIRECTOR_ROOM_H
#include <stddef.h>
#include <stdint.h>
/* Single-room-first director state (docs/multiplayer.md section 6.3).
   Phase 1 scope: fixed room/slot/member caps, opaque per-slot sealed
   snapshot plus capped update log, room clock epochs, no ACLs yet (every
   joined member edits every slot; roles arrive with Phase 2 invites).
   No I/O here; the server loop in main.c owns sockets and timers. */
#define DIRECTOR_ROOMS_MAX 256
#define DIRECTOR_MEMBERS_MAX 16
#define DIRECTOR_SLOTS_MAX 16
#define DIRECTOR_LOG_CAP 256
#define DIRECTOR_RESUME_SECS 600
typedef struct {
    double ctrl_tokens;
    double bin_tokens;
    uint64_t bucket_us;
} DirectorBucket;
void director_bucket_init(DirectorBucket *bucket, uint64_t now_us);
typedef struct {
    uint8_t key[32];
    int present;
    uint64_t issued_us;
} DirectorResume;
typedef struct {
    int used;
    uint32_t id;
    uint8_t pk[32];
    int is_owner;
    char *profile;
    uint64_t delay_us;
    int has_delay;
    uint64_t min_rtt_us;
    int has_rtt;
    uint64_t last_seen_us;
    DirectorResume resume;
    /* Malformed-frame strikes for the 1008 policy. */
    unsigned strikes;
    uint64_t strike_window_us;
    /* Token buckets refilled per second (see director_bucket_consume). */
    DirectorBucket bucket;
} DirectorMember;
typedef struct {
    uint8_t *data;
    size_t len;
    uint32_t member;
    uint32_t seq;
    uint16_t epoch;
    uint8_t kind;
} DirectorStored;
typedef struct {
    int used;
    DirectorStored snapshot;
    int has_snapshot;
    DirectorStored *log;
    unsigned log_start;
    unsigned log_count;
    /* Sealed slot name as sent, owner member id, room-wide mute. */
    char *name_enc;
    uint32_t owner;
    int muted;
    /* Latest eval record for late joiners (sealed body, verbatim). */
    char *last_enc;
    uint64_t last_seq;
    uint32_t last_author;
    uint64_t last_num;
    unsigned last_den;
    int has_last;
} DirectorSlot;
typedef struct {
    int used;
    char id[27];
    uint8_t opk[32];
    int has_opk;
    uint64_t eval_seq;
    uint64_t epoch_seq;
    /* Clock epoch: cycle c_e at server time t_us with tempo cps. */
    uint64_t epoch_num;
    unsigned epoch_den;
    uint64_t epoch_us;
    double epoch_cps;
    int running;
    /* Scheduled tempo change not yet active. */
    int has_next;
    uint64_t next_num;
    unsigned next_den;
    uint64_t next_us;
    double next_cps;
    DirectorMember members[DIRECTOR_MEMBERS_MAX];
    DirectorSlot slots[DIRECTOR_SLOTS_MAX];
    uint64_t created_us;
    /* Room policy (room_update, owner only). Quantum [0,1] means live. */
    uint64_t quantum_num;
    unsigned quantum_den;
    int locked;
    unsigned max_members;
} DirectorRoom;
/* Monotonic server time in microseconds. */
uint64_t director_now_us(void);
/* Find a room by id (validated 26-char name) or an empty slot. */
DirectorRoom *director_room_find(const char *id);
DirectorRoom *director_room_claim(const char *id);
/* Table access for resume scans and sweeps. */
DirectorRoom *director_room_at(unsigned index);
/* Create on first owner auth: records opk, starts epoch c=0 stopped. */
DirectorRoom *director_room_create(const char *id, const uint8_t *opk,
    uint64_t now_us);
void director_room_destroy(DirectorRoom *room);
/* Member slots: 0..members-1, stable while used. */
DirectorMember *director_member_add(DirectorRoom *room, const uint8_t *pk,
    int is_owner, uint64_t now_us);
DirectorMember *director_member_by_pk(DirectorRoom *room, const uint8_t *pk);
DirectorMember *director_member_by_id(DirectorRoom *room, uint32_t id);
void director_member_remove(DirectorRoom *room, DirectorMember *member);
/* Room cycle c(t) under the current epoch. */
double director_cycle_at(const DirectorRoom *room, uint64_t now_us);
/* Activation instant for an eval/tempo: ceil64(c(now+lead)) with
   lead = max(250 ms, max member delay + 350 ms). */
void director_activation_at(const DirectorRoom *room, uint64_t now_us,
    uint64_t *num, unsigned *den, uint64_t *act_us);
/* Opaque store: replace snapshot (kind 3) or append to the log. */
int director_store_frame(DirectorRoom *room, unsigned slot, uint32_t member,
    uint32_t seq, uint16_t epoch, uint8_t kind, const uint8_t *payload,
    size_t len, int is_snapshot);
/* Token buckets: 1 control and 64 binary tokens per second, burst 256
   control / 8192 binary. Callers pass the member bucket when authed, the
   session bucket before that. */
void director_bucket_init(DirectorBucket *bucket, uint64_t now_us);
int director_bucket_consume(DirectorBucket *bucket, int binary,
    uint64_t now_us);
/* Strike accounting for the 1008 policy: returns strikes in the window. */
unsigned director_strike(DirectorRoom *room, DirectorMember *member,
    uint64_t now_us);
#endif
