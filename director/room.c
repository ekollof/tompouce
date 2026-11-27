#include "room.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static DirectorRoom rooms[DIRECTOR_ROOMS_MAX];
uint64_t director_now_us(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC,&now);
    return (uint64_t)now.tv_sec*1000000u+(uint64_t)now.tv_nsec/1000u;
}
DirectorRoom *director_room_find(const char *id) {
    unsigned i;
    if (!id) return NULL;
    for (i=0;i<DIRECTOR_ROOMS_MAX;i++)
        if (rooms[i].used && !strcmp(rooms[i].id,id)) return &rooms[i];
    return NULL;
}
DirectorRoom *director_room_at(unsigned index) {
    if (index>=DIRECTOR_ROOMS_MAX) return NULL;
    return &rooms[index];
}
DirectorRoom *director_room_claim(const char *id) {
    unsigned i;
    size_t len;
    if (!id) return NULL;
    len=strlen(id);
    if (len!=26) return NULL;
    for (i=0;i<DIRECTOR_ROOMS_MAX;i++)
        if (!rooms[i].used) {
            memset(&rooms[i],0,sizeof(rooms[i]));
            memcpy(rooms[i].id,id,27);
            rooms[i].used=1;
            return &rooms[i];
        }
    return NULL;
}
DirectorRoom *director_room_create(const char *id, const uint8_t *opk,
    uint64_t now_us) {
    DirectorRoom *room=director_room_find(id);
    if (room) return NULL;
    room=director_room_claim(id);
    if (!room) return NULL;
    if (opk) {
        memcpy(room->opk,opk,32);
        room->has_opk=1;
    }
    room->epoch_num=0;
    room->epoch_den=1;
    room->epoch_us=now_us;
    room->epoch_cps=0.5;
    room->running=0;
    room->quantum_num=0;
    room->quantum_den=1;
    room->locked=0;
    room->max_members=DIRECTOR_MEMBERS_MAX;
    room->created_us=now_us;
    return room;
}
void director_room_destroy(DirectorRoom *room) {
    unsigned slot, k;
    if (!room) return;
    for (slot=0;slot<DIRECTOR_SLOTS_MAX;slot++) {
        DirectorSlot *state=&room->slots[slot];
        free(state->snapshot.data);
        free(state->name_enc);
        free(state->last_enc);
        if (state->log) {
            for (k=0;k<state->log_count;k++)
                free(state->log[(state->log_start+k)%DIRECTOR_LOG_CAP].data);
            free(state->log);
        }
    }
    memset(room,0,sizeof(*room));
}
DirectorMember *director_member_add(DirectorRoom *room, const uint8_t *pk,
    int is_owner, uint64_t now_us) {
    unsigned i;
    uint32_t id=1;
    if (!room || !pk) return NULL;
    for (i=0;i<DIRECTOR_MEMBERS_MAX;i++) {
        unsigned j;
        int clash=0;
        for (j=0;j<DIRECTOR_MEMBERS_MAX;j++)
            if (room->members[j].used && room->members[j].id==id) clash=1;
        if (!clash) break;
        id++;
    }
    if (i>=DIRECTOR_MEMBERS_MAX) return NULL;
    for (i=0;i<DIRECTOR_MEMBERS_MAX;i++)
        if (!room->members[i].used) {
            DirectorMember *member=&room->members[i];
            memset(member,0,sizeof(*member));
            member->used=1;
            member->id=id;
            memcpy(member->pk,pk,32);
            member->is_owner=is_owner;
            member->last_seen_us=now_us;
            director_bucket_init(&member->bucket,now_us);
            return member;
        }
    return NULL;
}
DirectorMember *director_member_by_pk(DirectorRoom *room,
    const uint8_t *pk) {
    unsigned i;
    if (!room || !pk) return NULL;
    for (i=0;i<DIRECTOR_MEMBERS_MAX;i++)
        if (room->members[i].used && !memcmp(room->members[i].pk,pk,32))
            return &room->members[i];
    return NULL;
}
DirectorMember *director_member_by_id(DirectorRoom *room, uint32_t id) {
    unsigned i;
    if (!room || !id) return NULL;
    for (i=0;i<DIRECTOR_MEMBERS_MAX;i++)
        if (room->members[i].used && room->members[i].id==id)
            return &room->members[i];
    return NULL;
}
void director_member_remove(DirectorRoom *room, DirectorMember *member) {
    if (!room || !member) return;
    free(member->profile);
    memset(member,0,sizeof(*member));
}
double director_cycle_at(const DirectorRoom *room, uint64_t now_us) {
    double base, delta;
    if (!room || room->epoch_den==0) return 0;
    base=(double)room->epoch_num/(double)room->epoch_den;
    if (now_us<room->epoch_us) return base;
    delta=(double)(now_us-room->epoch_us)*room->epoch_cps/1000000.0;
    return base+delta;
}
void director_activation_at(const DirectorRoom *room, uint64_t now_us,
    uint64_t *num, unsigned *den, uint64_t *act_us) {
    uint64_t lead=250000u, delay=0u, n;
    unsigned i;
    double cycle, target;
    if (!room || !num || !den || !act_us) return;
    for (i=0;i<DIRECTOR_MEMBERS_MAX;i++)
        if (room->members[i].used && room->members[i].has_delay &&
            room->members[i].delay_us>delay)
            delay=room->members[i].delay_us;
    if (delay+350000u>lead) lead=delay+350000u;
    /* Bounded even if every sample is attacker-controlled: a late member
       activates at the next window (section 8.7), nobody stalls the room. */
    if (lead>1000000u) lead=1000000u;
    cycle=director_cycle_at(room,now_us+lead);
    target=ceil(cycle*64.0-1e-9);
    if (target<0) target=0;
    n=(uint64_t)target;
    *num=n;
    *den=64;
    if (room->epoch_cps>0 && room->epoch_den>0) {
        double base=(double)room->epoch_num/(double)room->epoch_den;
        double seconds=(target/64.0-base)/room->epoch_cps;
        if (seconds<0) seconds=0;
        *act_us=room->epoch_us+(uint64_t)(seconds*1000000.0);
    } else {
        *act_us=now_us+lead;
    }
}
int director_store_frame(DirectorRoom *room, unsigned slot, uint32_t member,
    uint32_t seq, uint16_t epoch, uint8_t kind, const uint8_t *payload,
    size_t len, int is_snapshot) {
    DirectorSlot *state;
    uint8_t *copy;
    if (!room || slot>=DIRECTOR_SLOTS_MAX || !payload) return 0;
    if (len>1024u*1024u) return 0;
    state=&room->slots[slot];
    copy=malloc(len?len:1);
    if (!copy) return 0;
    if (len) memcpy(copy,payload,len);
    if (is_snapshot) {
        free(state->snapshot.data);
        state->snapshot.data=copy;
        state->snapshot.len=len;
        state->snapshot.member=member;
        state->snapshot.seq=seq;
        state->snapshot.epoch=epoch;
        state->snapshot.kind=kind;
        state->has_snapshot=1;
        state->used=1;
        return 1;
    }
    if (!state->log) {
        state->log=calloc(DIRECTOR_LOG_CAP,sizeof(*state->log));
        if (!state->log) {
            free(copy);
            return 0;
        }
    }
    if (state->log_count<DIRECTOR_LOG_CAP) {
        state->log[(state->log_start+state->log_count)%DIRECTOR_LOG_CAP]=
            (DirectorStored){copy,len,member,seq,epoch,kind};
        state->log_count++;
    } else {
        free(state->log[state->log_start].data);
        state->log[state->log_start]=
            (DirectorStored){copy,len,member,seq,epoch,kind};
        state->log_start=(state->log_start+1)%DIRECTOR_LOG_CAP;
    }
    state->used=1;
    return 1;
}
void director_bucket_init(DirectorBucket *bucket, uint64_t now_us) {
    if (!bucket) return;
    bucket->ctrl_tokens=256.0;
    bucket->bin_tokens=8192.0;
    bucket->bucket_us=now_us;
}
int director_bucket_consume(DirectorBucket *bucket, int binary,
    uint64_t now_us) {
    double *tokens, rate, burst, elapsed;
    if (!bucket) return 0;
    if (binary) {
        tokens=&bucket->bin_tokens;
        rate=4096.0;
        burst=8192.0;
    } else {
        tokens=&bucket->ctrl_tokens;
        rate=128.0;
        burst=256.0;
    }
    if (now_us>bucket->bucket_us) {
        elapsed=(double)(now_us-bucket->bucket_us)/1000000.0;
        *tokens+=elapsed*rate;
        if (*tokens>burst) *tokens=burst;
        bucket->bucket_us=now_us;
    }
    if (*tokens<1.0) return 0;
    *tokens-=1.0;
    return 1;
}
unsigned director_strike(DirectorRoom *room, DirectorMember *member,
    uint64_t now_us) {
    (void)room;
    if (!member) return 0;
    if (now_us-member->strike_window_us>10000000u) {
        member->strikes=0;
        member->strike_window_us=now_us;
    }
    member->strikes++;
    return member->strikes;
}
