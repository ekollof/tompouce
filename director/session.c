#include "session.h"
#include "collab_proto.h"
#include <cjson/cJSON.h>
#include <libwebsockets.h>
#include <math.h>
#include <sodium.h>
#include <stdlib.h>
#include <string.h>
#define DIRECTOR_SESSIONS_MAX 4096
#define DIRECTOR_HELLO_TIMEOUT_US 5000000u
#define DIRECTOR_PROFILE_MAX 4096
#define DIRECTOR_NAME_MAX 128
static DirectorSession *sessions[DIRECTOR_SESSIONS_MAX];
static unsigned session_count=0;
/* Recent evals for ack relay: eval_seq -> author member id. */
#define DIRECTOR_RECENT_EVALS 64
static struct {
    DirectorRoom *room;
    uint64_t seq;
    uint32_t member;
} recent_evals[DIRECTOR_RECENT_EVALS];
static unsigned recent_pos=0;
void director_sessions_init(void) {
    memset(sessions,0,sizeof(sessions));
    memset(recent_evals,0,sizeof(recent_evals));
    session_count=0;
    recent_pos=0;
}
DirectorSession *director_session_create(struct lws *wsi) {
    DirectorSession *session;
    unsigned i;
    if (session_count>=DIRECTOR_SESSIONS_MAX) return NULL;
    for (i=0;i<DIRECTOR_SESSIONS_MAX;i++)
        if (!sessions[i]) break;
    if (i>=DIRECTOR_SESSIONS_MAX) return NULL;
    session=calloc(1,sizeof(*session));
    if (!session) return NULL;
    session->wsi=wsi;
    session->connected_us=director_now_us();
    session->last_rx_us=session->connected_us;
    director_bucket_init(&session->bucket,session->connected_us);
    sessions[i]=session;
    session_count++;
    return session;
}
static void tx_free_list(CpTx *head) {
    while (head) {
        CpTx *next=head->next;
        free(head->data);
        free(head);
        head=next;
    }
}
void director_session_free(DirectorSession *session) {
    unsigned i;
    if (!session) return;
    for (i=0;i<DIRECTOR_SESSIONS_MAX;i++)
        if (sessions[i]==session) {
            sessions[i]=NULL;
            session_count--;
            break;
        }
    if (session->member) {
        /* Presence ends with the connection, but the member record and
           its resume token survive for reconnect (see the sweep). */
        session->member=NULL;
    }
    tx_free_list(session->tx_head);
    free(session->rx);
    free(session);
}
DirectorSession *director_session_lookup(struct lws *wsi) {
    unsigned i;
    for (i=0;i<DIRECTOR_SESSIONS_MAX;i++)
        if (sessions[i] && sessions[i]->wsi==wsi) return sessions[i];
    return NULL;
}
static int session_enqueue(DirectorSession *session, const uint8_t *data,
    size_t len, int binary) {
    CpTx *node;
    if (!session || !data || !len) return 0;
    node=malloc(sizeof(*node));
    if (!node) return 0;
    node->data=malloc((size_t)LWS_PRE+len);
    if (!node->data) {
        free(node);
        return 0;
    }
    memcpy(node->data+LWS_PRE,data,len);
    node->len=len;
    node->sent=0;
    node->binary=binary;
    node->next=NULL;
    if (session->tx_tail) session->tx_tail->next=node;
    else session->tx_head=node;
    session->tx_tail=node;
    lws_callback_on_writable(session->wsi);
    return 1;
}
int director_session_send(DirectorSession *session, const char *text,
    size_t len) {
    return session_enqueue(session,(const uint8_t *)text,len,0);
}
int director_session_send_binary(DirectorSession *session, const uint8_t *data,
    size_t len) {
    return session_enqueue(session,data,len,1);
}
int director_session_flush(DirectorSession *session) {
    while (session->tx_head) {
        CpTx *node=session->tx_head;
        int protocol=node->binary?LWS_WRITE_BINARY:LWS_WRITE_TEXT;
        int written=lws_write(session->wsi,node->data+LWS_PRE+node->sent,
            node->len-node->sent,(enum lws_write_protocol)protocol);
        if (written<0) return 0;
        node->sent+=(size_t)written;
        if (node->sent<node->len) {
            lws_callback_on_writable(session->wsi);
            return 1;
        }
        session->tx_head=node->next;
        if (!session->tx_head) session->tx_tail=NULL;
        free(node->data);
        free(node);
    }
    return 0;
}
void director_broadcast_text(DirectorRoom *room, DirectorSession *except,
    const char *text, size_t len) {
    unsigned i;
    if (!room || !text) return;
    for (i=0;i<DIRECTOR_SESSIONS_MAX;i++) {
        DirectorSession *peer=sessions[i];
        if (peer && peer!=except && peer->room==room && peer->member)
            session_enqueue(peer,(const uint8_t *)text,len,0);
    }
}
void director_broadcast_binary(DirectorRoom *room, DirectorSession *except,
    const uint8_t *frame, size_t len) {
    unsigned i;
    if (!room || !frame) return;
    for (i=0;i<DIRECTOR_SESSIONS_MAX;i++) {
        DirectorSession *peer=sessions[i];
        if (peer && peer!=except && peer->room==room && peer->member)
            session_enqueue(peer,frame,len,1);
    }
}
/* --- JSON builders (caller frees the printed string) --- */
static char *json_print(cJSON *obj) {
    char *text;
    if (!obj) return NULL;
    text=cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return text;
}
static void json_add_cycle(cJSON *obj, const char *key, uint64_t num,
    unsigned den) {
    cJSON *pair=cJSON_CreateArray();
    if (!pair) return;
    cJSON_AddItemToArray(pair,cJSON_CreateNumber((double)num));
    cJSON_AddItemToArray(pair,cJSON_CreateNumber((double)den));
    cJSON_AddItemToObject(obj,key,pair);
}
static char *build_error(CpError code, const char *msg, int has_id,
    uint32_t id) {
    cJSON *obj=cJSON_CreateObject();
    if (!obj) return NULL;
    cJSON_AddStringToObject(obj,"t","error");
    if (has_id) cJSON_AddNumberToObject(obj,"ref",(double)id);
    cJSON_AddStringToObject(obj,"code",cp_error_string(code));
    if (msg) cJSON_AddStringToObject(obj,"msg",msg);
    return json_print(obj);
}
static void send_error(DirectorSession *s, CpError code, const char *msg,
    int has_id, uint32_t id) {
    char *text=build_error(code,msg,has_id,id);
    if (!text) return;
    director_session_send(s,text,strlen(text));
    free(text);
}
static void send_ack(DirectorSession *s, uint32_t id) {
    cJSON *obj=cJSON_CreateObject();
    char *text;
    if (!obj) return;
    cJSON_AddStringToObject(obj,"t","ack");
    cJSON_AddNumberToObject(obj,"ref",(double)id);
    text=json_print(obj);
    if (!text) return;
    director_session_send(s,text,strlen(text));
    free(text);
}
static void close_with(DirectorSession *s, int status, const char *reason) {
    unsigned char buffer[128];
    size_t len=reason?strlen(reason):0;
    if (len>sizeof(buffer)) len=sizeof(buffer);
    if (reason) memcpy(buffer,reason,len);
    lws_close_reason(s->wsi,(enum lws_close_status)status,buffer,len);
}
/* --- hello --- */
static int has_cap(const void *obj, const char *want) {
    const cJSON *caps=cJSON_GetObjectItemCaseSensitive(
        (const cJSON *)obj,"caps");
    const cJSON *item;
    if (!cJSON_IsArray(caps)) return 0;
    for (item=caps->child;item;item=item->next)
        if (cJSON_IsString(item) && item->valuestring &&
            !strcmp(item->valuestring,want)) return 1;
    return 0;
}
static void handle_hello(DirectorSession *s, const CpMsg *msg,
    const DirectorConfig *config) {
    cJSON *obj, *server, *limits;
    char *text, challenge[64];
    char client_version[128];
    const char *version=NULL;
    (void)config;
    obj=(cJSON *)msg->owned;
    {
        const cJSON *protos=cJSON_GetObjectItemCaseSensitive(obj,"proto");
        const cJSON *item;
        int ok=0;
        if (!cJSON_IsArray(protos)) {
            send_error(s,CP_E_SCHEMA,"proto must be an array",msg->has_id,
                msg->id);
            return;
        }
        for (item=protos->child;item;item=item->next)
            if (cJSON_IsNumber(item) && item->valuedouble==1) ok=1;
        if (!ok) {
            send_error(s,CP_E_PROTO,"no common protocol major",msg->has_id,
                msg->id);
            close_with(s,1002,"no common protocol");
            return;
        }
    }
    if (!has_cap(obj,"e2e")) {
        send_error(s,CP_E_PERM,"e2e capability required",msg->has_id,msg->id);
        return;
    }
    {
        const cJSON *client=cJSON_GetObjectItemCaseSensitive(obj,"client");
        if (client && cJSON_IsObject(client)) {
            const cJSON *v=cJSON_GetObjectItemCaseSensitive(client,"version");
            if (cJSON_IsString(v) && v->valuestring) version=v->valuestring;
        }
    }
    if (version) {
        size_t n=strlen(version);
        if (n>=sizeof(client_version)) n=sizeof(client_version)-1;
        memcpy(client_version,version,n);
        client_version[n]=0;
    } else {
        memcpy(client_version,"unknown",8);
    }
    randombytes_buf(s->challenge,sizeof(s->challenge));
    cp_b64_encode(s->challenge,sizeof(s->challenge),challenge,
        sizeof(challenge));
    obj=cJSON_CreateObject();
    if (!obj) return;
    server=cJSON_CreateObject();
    limits=cJSON_CreateObject();
    if (!server || !limits) {
        cJSON_Delete(obj);
        if (server) cJSON_Delete(server);
        if (limits) cJSON_Delete(limits);
        return;
    }
    cJSON_AddStringToObject(obj,"t","welcome");
    cJSON_AddNumberToObject(obj,"proto",1);
    {
        cJSON *caps=cJSON_CreateArray();
        if (caps) {
            cJSON_AddItemToArray(caps,cJSON_CreateString("e2e"));
            cJSON_AddItemToObject(obj,"caps",caps);
        }
    }
    cJSON_AddStringToObject(obj,"challenge",challenge);
    cJSON_AddStringToObject(server,"name",config->server_name);
    cJSON_AddStringToObject(server,"version",config->server_version);
    cJSON_AddStringToObject(server,"source",config->server_source);
    cJSON_AddItemToObject(obj,"server",server);
    cJSON_AddNumberToObject(limits,"ctrl",CP_CONTROL_MAX);
    cJSON_AddNumberToObject(limits,"update",CP_BIN_PAYLOAD_MAX);
    cJSON_AddNumberToObject(limits,"eval_body",CP_EVAL_BODY_MAX);
    cJSON_AddItemToObject(obj,"limits",limits);
    cJSON_AddNumberToObject(obj,"now_us",(double)director_now_us());
    text=json_print(obj);
    if (!text) return;
    director_session_send(s,text,strlen(text));
    free(text);
    s->phase=1;
}
/* --- auth --- */
static int verify_sig(const uint8_t *pk, const uint8_t *sig,
    const uint8_t *challenge, const char *room) {
    static const char context[]="tompouce-collab-v1";
    uint8_t msg[sizeof(context)-1+32+26+32];
    memcpy(msg,context,sizeof(context)-1);
    memcpy(msg+sizeof(context)-1,challenge,32);
    memcpy(msg+sizeof(context)-1+32,room,26);
    memcpy(msg+sizeof(context)-1+32+26,pk,32);
    return crypto_sign_verify_detached(sig,msg,sizeof(msg),pk)==0;
}
/* Verifies the Phase-1 bootstrap proof without touching shared state, so
   a bad proof never consumes a room slot (reviewer note). Returns 1 with
   nothing sent on success; otherwise sends the error and returns 0. */
static int check_bootstrap_proof(DirectorSession *s, const void *obj,
    const char *room_id, const uint8_t *pk, const DirectorConfig *config,
    const CpMsg *msg) {
    uint8_t mac[32], material[32+26+32], proof[32];
    size_t proof_len=0;
    if (!config->has_admission_key ||
        !cp_get_b64(obj,"proof",proof,sizeof(proof),
        &proof_len) || proof_len!=sizeof(proof)) {
        send_error(s,CP_E_AUTH,"proof required to claim",
            msg->has_id,msg->id);
        return 0;
    }
    memcpy(material,s->challenge,32);
    memcpy(material+32,room_id,26);
    memcpy(material+32+26,pk,32);
    crypto_generichash(mac,sizeof(mac),material,sizeof(material),
        config->admission_key,sizeof(config->admission_key));
    if (sodium_memcmp(mac,proof,32)!=0) {
        send_error(s,CP_E_AUTH,"bad proof",msg->has_id,msg->id);
        return 0;
    }
    return 1;
}
static void auth_success(DirectorSession *s, DirectorRoom *room,
    DirectorMember *member, const CpMsg *msg) {
    cJSON *obj;
    char *text, pk[64], resume[64];
    unsigned i;
    s->room=room;
    /* One live session per member: a re-auth replaces the old one. */
    for (i=0;i<DIRECTOR_SESSIONS_MAX;i++) {
        DirectorSession *peer=sessions[i];
        if (peer && peer!=s && peer->member==member) {
            peer->member=NULL;
            peer->room=NULL;
            close_with(peer,1000,"replaced");
        }
    }
    s->member=member;
    member->last_seen_us=director_now_us();
    randombytes_buf(member->resume.key,sizeof(member->resume.key));
    member->resume.present=1;
    member->resume.issued_us=director_now_us();
    cp_b64_encode(member->pk,sizeof(member->pk),pk,sizeof(pk));
    cp_b64_encode(member->resume.key,sizeof(member->resume.key),resume,
        sizeof(resume));
    obj=cJSON_CreateObject();
    if (!obj) return;
    cJSON_AddStringToObject(obj,"t","auth_ok");
    cJSON_AddStringToObject(obj,"member_pk",pk);
    cJSON_AddStringToObject(obj,"resume",resume);
    if (msg->has_id) cJSON_AddNumberToObject(obj,"ref",(double)msg->id);
    text=json_print(obj);
    if (!text) return;
    director_session_send(s,text,strlen(text));
    free(text);
    s->phase=2;
}
static void handle_auth(DirectorSession *s, const CpMsg *msg,
    const DirectorConfig *config) {
    const void *obj=msg->owned;
    const char *room_id=NULL, *b64;
    uint8_t pk[32], sig[64], token[32];
    size_t outlen=0;
    DirectorRoom *room;
    if (!cp_get_room_id(obj,"room",&room_id)) {
        send_error(s,CP_E_SCHEMA,"room required",msg->has_id,msg->id);
        return;
    }
    if (!cp_get_b64(obj,"pk",pk,sizeof(pk),&outlen) || outlen!=sizeof(pk)) {
        send_error(s,CP_E_SCHEMA,"pk must be 32 bytes",msg->has_id,msg->id);
        return;
    }
    if (!cp_get_b64(obj,"sig",sig,sizeof(sig),&outlen) || outlen!=sizeof(sig)) {
        send_error(s,CP_E_SCHEMA,"sig must be 64 bytes",msg->has_id,msg->id);
        return;
    }
    if (!verify_sig(pk,sig,s->challenge,room_id)) {
        send_error(s,CP_E_AUTH,"bad signature",msg->has_id,msg->id);
        return;
    }
    room=director_room_find(room_id);
    {
        const cJSON *node=cJSON_GetObjectItemCaseSensitive(
            (const cJSON *)obj,"owner");
        if (node) {
            DirectorMember *member;
            if (!cJSON_IsTrue(node)) {
                send_error(s,CP_E_SCHEMA,"owner must be true",msg->has_id,
                    msg->id);
                return;
            }
            if (!room) {
                if (!check_bootstrap_proof(s,obj,room_id,pk,config,msg))
                    return;
                room=director_room_create(room_id,pk,director_now_us());
                if (!room) {
                    send_error(s,CP_E_ROOM_FULL,"no room slots",
                        msg->has_id,msg->id);
                    return;
                }
                memcpy(room->opk,pk,32);
                room->has_opk=1;
            } else if (!room->has_opk) {
                if (!check_bootstrap_proof(s,obj,room_id,pk,config,msg))
                    return;
                memcpy(room->opk,pk,32);
                room->has_opk=1;
            }
            if (sodium_memcmp(room->opk,pk,32)!=0) {
                send_error(s,CP_E_AUTH,"not the owner",msg->has_id,msg->id);
                return;
            }
            member=director_member_by_pk(room,pk);
            if (!member) {
                member=director_member_add(room,pk,1,director_now_us());
                if (!member) {
                    send_error(s,CP_E_ROOM_FULL,"room is full",msg->has_id,
                        msg->id);
                    return;
                }
            }
            auth_success(s,room,member,msg);
            return;
        }
    }
    {
        const cJSON *node=cJSON_GetObjectItemCaseSensitive(
            (const cJSON *)obj,"resume");
        if (node) {
            unsigned r;
            if (!cJSON_IsString(node) || !node->valuestring ||
                !cp_b64_decode(node->valuestring,token,sizeof(token),
                &outlen) || outlen!=sizeof(token)) {
                send_error(s,CP_E_SCHEMA,"resume must be 32 bytes",
                    msg->has_id,msg->id);
                return;
            }
            for (r=0;r<DIRECTOR_ROOMS_MAX;r++) {
                /* Scan is over all rooms; resume binds member to session. */
            }
            {
                unsigned k;
                for (k=0;k<DIRECTOR_ROOMS_MAX;k++) {
                    DirectorRoom *cand=director_room_at(k);
                    DirectorMember *m;
                    if (!cand || !cand->used) continue;
                    m=director_member_by_pk(cand,pk);
                    if (m && m->resume.present &&
                        sodium_memcmp(m->resume.key,token,32)==0 &&
                        director_now_us()-m->resume.issued_us<=
                        (uint64_t)DIRECTOR_RESUME_SECS*1000000u) {
                        m->resume.present=0;
                        auth_success(s,cand,m,msg);
                        return;
                    }
                }
            }
            send_error(s,CP_E_AUTH,"bad resume",msg->has_id,msg->id);
            return;
        }
    }
    {
        const cJSON *node=cJSON_GetObjectItemCaseSensitive(
            (const cJSON *)obj,"invite");
        if (node) {
            send_error(s,CP_E_INVITE_INVALID,"invites are phase 2",
                msg->has_id,msg->id);
            return;
        }
    }
    if (cp_get_b64(obj,"proof",token,sizeof(token),&outlen) &&
        outlen==sizeof(token)) {
        uint8_t mac[32], material[32+26+32];
        DirectorMember *member;
        if (!config->has_admission_key) {
            send_error(s,CP_E_AUTH,"no admission key configured",
                msg->has_id,msg->id);
            return;
        }
        memcpy(material,s->challenge,32);
        memcpy(material+32,room_id,26);
        memcpy(material+32+26,pk,32);
        crypto_generichash(mac,sizeof(mac),material,sizeof(material),
            config->admission_key,sizeof(config->admission_key));
        if (sodium_memcmp(mac,token,32)!=0) {
            send_error(s,CP_E_AUTH,"bad proof",msg->has_id,msg->id);
            return;
        }
        if (!room) {
            room=director_room_create(room_id,NULL,director_now_us());
            if (!room) {
                send_error(s,CP_E_ROOM_FULL,"no room slots",msg->has_id,
                    msg->id);
                return;
            }
        }
        member=director_member_by_pk(room,pk);
        if (!member) {
            unsigned count=0, i;
            for (i=0;i<DIRECTOR_MEMBERS_MAX;i++)
                if (room->members[i].used) count++;
            if (count>=room->max_members) {
                send_error(s,CP_E_ROOM_FULL,"room is full",msg->has_id,
                    msg->id);
                return;
            }
            member=director_member_add(room,pk,0,director_now_us());
            if (!member) {
                send_error(s,CP_E_ROOM_FULL,"room is full",msg->has_id,
                    msg->id);
                return;
            }
        }
        auth_success(s,room,member,msg);
        return;
    }
    if (cp_get_string(obj,"proof",&b64,65536)) {
        send_error(s,CP_E_SCHEMA,"proof must be 32 bytes",msg->has_id,
            msg->id);
        return;
    }
    send_error(s,CP_E_AUTH,"auth needs owner, resume or proof",msg->has_id,
        msg->id);
}
/* --- join --- */
static void send_member_event(DirectorRoom *room, const char *type,
    DirectorMember *member, DirectorSession *except) {
    cJSON *obj, *entry;
    char *text, pk[64];
    obj=cJSON_CreateObject();
    entry=cJSON_CreateObject();
    if (!obj || !entry) {
        if (obj) cJSON_Delete(obj);
        if (entry) cJSON_Delete(entry);
        return;
    }
    cp_b64_encode(member->pk,sizeof(member->pk),pk,sizeof(pk));
    cJSON_AddStringToObject(obj,"t",type);
    cJSON_AddNumberToObject(entry,"member",(double)member->id);
    cJSON_AddStringToObject(entry,"pk",pk);
    cJSON_AddStringToObject(entry,"role",member->is_owner?"owner":"editor");
    cJSON_AddNumberToObject(entry,"color_idx",(double)(member->id%8));
    if (member->profile)
        cJSON_AddStringToObject(entry,"profile",member->profile);
    cJSON_AddItemToObject(obj,"member",entry);
    text=json_print(obj);
    if (!text) return;
    director_broadcast_text(room,except,text,strlen(text));
    free(text);
}
static void build_clock(cJSON *obj, const DirectorRoom *room) {
    cJSON *epoch=cJSON_CreateObject();
    cJSON *clock=cJSON_CreateObject();
    if (!epoch || !clock) {
        if (epoch) cJSON_Delete(epoch);
        if (clock) cJSON_Delete(clock);
        return;
    }
    json_add_cycle(epoch,"c",room->epoch_num,room->epoch_den);
    cJSON_AddNumberToObject(epoch,"t_us",(double)room->epoch_us);
    cJSON_AddNumberToObject(epoch,"cps",room->epoch_cps);
    cJSON_AddItemToObject(clock,"epoch",epoch);
    if (room->has_next) {
        cJSON *next=cJSON_CreateObject();
        if (next) {
            json_add_cycle(next,"c",room->next_num,room->next_den);
            cJSON_AddNumberToObject(next,"t_us",(double)room->next_us);
            cJSON_AddNumberToObject(next,"cps",room->next_cps);
            cJSON_AddItemToObject(clock,"next",next);
        }
    }
    cJSON_AddBoolToObject(clock,"running",room->running);
    cJSON_AddNumberToObject(clock,"seq",(double)room->clock_seq);
    cJSON_AddItemToObject(obj,"clock",clock);
}
static void handle_join(DirectorSession *s, const CpMsg *msg) {
    const void *obj=msg->owned;
    const char *room_id=NULL, *profile=NULL;
    DirectorRoom *room;
    cJSON *out, *robj, *you, *members, *slots, *clock_holder;
    char *text, opk[64], pk[64];
    unsigned i;
    if (!cp_get_room_id(obj,"room",&room_id)) {
        send_error(s,CP_E_SCHEMA,"room required",msg->has_id,msg->id);
        return;
    }
    if (!s->room || strcmp(s->room->id,room_id)) {
        send_error(s,CP_E_NOT_FOUND,"join the authed room",msg->has_id,
            msg->id);
        return;
    }
    room=s->room;
    if (cp_get_string(obj,"profile",&profile,DIRECTOR_PROFILE_MAX) &&
        profile) {
        size_t n=strlen(profile);
        free(s->member->profile);
        s->member->profile=malloc(n+1);
        if (s->member->profile) memcpy(s->member->profile,profile,n+1);
    }
    out=cJSON_CreateObject();
    robj=cJSON_CreateObject();
    you=cJSON_CreateObject();
    members=cJSON_CreateArray();
    slots=cJSON_CreateArray();
    if (!out || !robj || !you || !members || !slots) {
        if (out) cJSON_Delete(out);
        if (robj) cJSON_Delete(robj);
        if (you) cJSON_Delete(you);
        if (members) cJSON_Delete(members);
        if (slots) cJSON_Delete(slots);
        return;
    }
    cJSON_AddStringToObject(out,"t","joined");
    json_add_cycle(robj,"quantum",room->quantum_num,room->quantum_den);
    cJSON_AddBoolToObject(robj,"locked",room->locked);
    cJSON_AddBoolToObject(robj,"solos_off",room->solos_off);
    cJSON_AddBoolToObject(robj,"e2e",1);
    cJSON_AddNumberToObject(robj,"epoch",(double)room->key_epoch);
    if (room->has_opk) {
        cp_b64_encode(room->opk,sizeof(room->opk),opk,sizeof(opk));
        cJSON_AddStringToObject(robj,"opk",opk);
    }
    cJSON_AddItemToObject(out,"room",robj);
    cJSON_AddNumberToObject(you,"member",(double)s->member->id);
    cJSON_AddStringToObject(you,"role",
        s->member->is_owner?"owner":"editor");
    cJSON_AddNumberToObject(you,"color_idx",
        (double)(s->member->id%8));
    cJSON_AddItemToObject(out,"you",you);
    for (i=0;i<DIRECTOR_MEMBERS_MAX;i++) {
        DirectorMember *m=&room->members[i];
        cJSON *entry;
        if (!m->used) continue;
        entry=cJSON_CreateObject();
        if (!entry) continue;
        cp_b64_encode(m->pk,sizeof(m->pk),pk,sizeof(pk));
        cJSON_AddNumberToObject(entry,"member",(double)m->id);
        cJSON_AddStringToObject(entry,"pk",pk);
        cJSON_AddStringToObject(entry,"role",
            m->is_owner?"owner":"editor");
        cJSON_AddNumberToObject(entry,"color_idx",(double)(m->id%8));
        if (m->profile) cJSON_AddStringToObject(entry,"profile",m->profile);
        cJSON_AddItemToArray(members,entry);
    }
    cJSON_AddItemToObject(out,"members",members);
    for (i=0;i<DIRECTOR_SLOTS_MAX;i++) {
        DirectorSlot *slot=&room->slots[i];
        cJSON *entry;
        if (i!=0 && !slot->used) continue;
        entry=cJSON_CreateObject();
        if (!entry) continue;
        cJSON_AddNumberToObject(entry,"slot",(double)i);
        if (slot->name_enc)
            cJSON_AddStringToObject(entry,"slot_name",slot->name_enc);
        cJSON_AddNumberToObject(entry,"owner",(double)slot->owner);
        cJSON_AddBoolToObject(entry,"muted",slot->muted);
        if (slot->has_last) {
            cJSON *last=cJSON_CreateObject();
            if (last) {
                cJSON_AddStringToObject(last,"enc",slot->last_enc);
                cJSON_AddNumberToObject(last,"seq",(double)slot->last_seq);
                cJSON_AddNumberToObject(last,"author",
                    (double)slot->last_author);
                json_add_cycle(last,"at_cycle",slot->last_num,
                    slot->last_den);
                cJSON_AddItemToObject(entry,"last_eval",last);
            }
        }
        cJSON_AddItemToArray(slots,entry);
    }
    cJSON_AddItemToObject(out,"slots",slots);
    if (msg->has_id) cJSON_AddNumberToObject(out,"ref",(double)msg->id);
    clock_holder=cJSON_CreateObject();
    if (clock_holder) {
        build_clock(clock_holder,room);
        {
            const cJSON *clock=cJSON_GetObjectItemCaseSensitive(clock_holder,
                "clock");
            cJSON_DetachItemFromObject(clock_holder,"clock");
            if (clock) cJSON_AddItemToObject(out,"clock",(cJSON *)clock);
        }
        cJSON_Delete(clock_holder);
    }
    text=json_print(out);
    if (!text) return;
    director_session_send(s,text,strlen(text));
    free(text);
    /* Replay the opaque store, oldest first, then mark synced. */
    for (i=0;i<DIRECTOR_SLOTS_MAX;i++) {
        DirectorSlot *slot=&room->slots[i];
        unsigned k;
        if (!slot->used) continue;
        if (slot->has_snapshot) {
            uint8_t header[20];
            uint8_t *frame;
            header[0]=1;
            header[1]=3;
            header[2]=(uint8_t)(i>>8);
            header[3]=(uint8_t)i;
            header[4]=(uint8_t)(slot->snapshot.member>>24);
            header[5]=(uint8_t)(slot->snapshot.member>>16);
            header[6]=(uint8_t)(slot->snapshot.member>>8);
            header[7]=(uint8_t)slot->snapshot.member;
            header[8]=(uint8_t)(slot->snapshot.seq>>24);
            header[9]=(uint8_t)(slot->snapshot.seq>>16);
            header[10]=(uint8_t)(slot->snapshot.seq>>8);
            header[11]=(uint8_t)slot->snapshot.seq;
            header[12]=(uint8_t)(slot->snapshot.epoch>>8);
            header[13]=(uint8_t)slot->snapshot.epoch;
            header[14]=0;
            header[15]=0;
            header[16]=(uint8_t)(slot->snapshot.len>>24);
            header[17]=(uint8_t)(slot->snapshot.len>>16);
            header[18]=(uint8_t)(slot->snapshot.len>>8);
            header[19]=(uint8_t)slot->snapshot.len;
            frame=malloc(20+slot->snapshot.len);
            if (!frame) continue;
            memcpy(frame,header,20);
            if (slot->snapshot.len)
                memcpy(frame+20,slot->snapshot.data,slot->snapshot.len);
            director_session_send_binary(s,frame,20+slot->snapshot.len);
            free(frame);
        }
        for (k=0;k<slot->log_count;k++) {
            const DirectorStored *entry=&slot->log[(slot->log_start+k)%
                DIRECTOR_LOG_CAP];
            uint8_t header[20];
            uint8_t *frame;
            header[0]=1;
            header[1]=entry->kind?entry->kind:1;
            header[2]=(uint8_t)(i>>8);
            header[3]=(uint8_t)i;
            header[4]=(uint8_t)(entry->member>>24);
            header[5]=(uint8_t)(entry->member>>16);
            header[6]=(uint8_t)(entry->member>>8);
            header[7]=(uint8_t)entry->member;
            header[8]=(uint8_t)(entry->seq>>24);
            header[9]=(uint8_t)(entry->seq>>16);
            header[10]=(uint8_t)(entry->seq>>8);
            header[11]=(uint8_t)entry->seq;
            header[12]=(uint8_t)(entry->epoch>>8);
            header[13]=(uint8_t)entry->epoch;
            header[14]=0;
            header[15]=0;
            header[16]=(uint8_t)(entry->len>>24);
            header[17]=(uint8_t)(entry->len>>16);
            header[18]=(uint8_t)(entry->len>>8);
            header[19]=(uint8_t)entry->len;
            frame=malloc(20+entry->len);
            if (!frame) continue;
            memcpy(frame,header,20);
            if (entry->len) memcpy(frame+20,entry->data,entry->len);
            director_session_send_binary(s,frame,20+entry->len);
            free(frame);
        }
    }
    {
        cJSON *sync=cJSON_CreateObject();
        char *stext;
        if (!sync) return;
        cJSON_AddStringToObject(sync,"t","synced");
        {
            cJSON *ids=cJSON_CreateArray();
            if (ids) {
                for (i=0;i<DIRECTOR_SLOTS_MAX;i++)
                    if (room->slots[i].used || i==0)
                        cJSON_AddItemToArray(ids,
                            cJSON_CreateNumber((double)i));
                cJSON_AddItemToObject(sync,"slots",ids);
            }
        }
        stext=json_print(sync);
        if (!stext) return;
        director_session_send(s,stext,strlen(stext));
        free(stext);
    }
    send_member_event(room,"member_joined",s->member,s);
}
/* --- ping --- */
static void handle_ping(DirectorSession *s, const CpMsg *msg) {
    const void *obj=msg->owned;
    uint64_t now, t0=0;
    cJSON *out;
    char *text;
    {
        const cJSON *item=cJSON_GetObjectItemCaseSensitive(
            (const cJSON *)obj,"t0");
        double v;
        if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
            item->valuedouble<0) {
            send_error(s,CP_E_SCHEMA,"t0 required",msg->has_id,msg->id);
            return;
        }
        v=item->valuedouble;
        if (v>9007199254740992.0 || floor(v)!=v) {
            send_error(s,CP_E_SCHEMA,"t0 must be an integer",msg->has_id,
                msg->id);
            return;
        }
        t0=(uint64_t)v;
    }
    now=director_now_us();
    if (s->member) {
        uint64_t rtt=now>t0?now-t0:0;
        /* t0 is the client's monotonic clock, unreadable here: anything
           absurd is clock garbage, not network delay (a client sending
           t0=0 would otherwise push the whole room's lead to our uptime).
           Proper per-member delay needs director-sent pings (section 8.2,
           Phase 3); until then, smooth hard and cap hard. */
        if (rtt<=5000000u) {
            if (!s->member->has_rtt || rtt<s->member->min_rtt_us) {
                uint64_t delay=rtt/2;
                if (delay>650000u) delay=650000u;
                s->member->min_rtt_us=rtt;
                s->member->has_rtt=1;
                s->member->delay_us=delay;
                s->member->has_delay=1;
            }
        }
        s->member->last_seen_us=now;
    }
    out=cJSON_CreateObject();
    if (!out) return;
    cJSON_AddStringToObject(out,"t","pong");
    cJSON_AddNumberToObject(out,"t0",(double)t0);
    cJSON_AddNumberToObject(out,"t1",(double)now);
    cJSON_AddNumberToObject(out,"t2",(double)director_now_us());
    if (msg->has_id) cJSON_AddNumberToObject(out,"ref",(double)msg->id);
    text=json_print(out);
    if (!text) return;
    director_session_send(s,text,strlen(text));
    free(text);
}
/* --- eval_req --- */
static int check_slot(const void *obj, unsigned *slot) {
    uint32_t id;
    if (!cp_get_u32(obj,"slot",&id) || id>=DIRECTOR_SLOTS_MAX) return 0;
    *slot=(unsigned)id;
    return 1;
}
static void handle_eval_req(DirectorSession *s, const CpMsg *msg) {
    const void *obj=msg->owned;
    const char *enc=NULL;
    unsigned slot=0;
    uint64_t num=0, seq;
    unsigned den=64;
    uint64_t act_us;
    cJSON *out, *at;
    char *text;
    if (!check_slot(obj,&slot)) {
        send_error(s,CP_E_SCHEMA,"slot 0-15 required",msg->has_id,msg->id);
        return;
    }
    if (cJSON_GetObjectItemCaseSensitive((const cJSON *)obj,"body")) {
        send_error(s,CP_E_E2E_REQUIRED,"body must travel sealed in enc",
            msg->has_id,msg->id);
        return;
    }
    if (!cp_get_string(obj,"enc",&enc,CP_COMPOSITE_MAX) || !enc) {
        send_error(s,CP_E_SCHEMA,"enc required",msg->has_id,msg->id);
        return;
    }
    {
        uint64_t qn=0;
        unsigned qd=1;
        if (cJSON_GetObjectItemCaseSensitive((const cJSON *)obj,"q")) {
            double room_q, req_q;
            if (!cp_get_cycle(obj,"q",&qn,&qd)) {
                send_error(s,CP_E_SCHEMA,"bad quantum",msg->has_id,msg->id);
                return;
            }
            room_q=(double)s->room->quantum_num;
            if (s->room->quantum_den>1)
                room_q/=(double)s->room->quantum_den;
            req_q=(double)qn/(double)qd;
            if (req_q<room_q) {
                send_error(s,CP_E_PERM,"quantum finer than room",
                    msg->has_id,msg->id);
                return;
            }
        }
    }
    director_activation_at(s->room,director_now_us(),&num,&den,&act_us);
    seq=++s->room->eval_seq;
    {
        DirectorSlot *state=&s->room->slots[slot];
        char *copy=malloc(strlen(enc)+1);
        if (copy) {
            memcpy(copy,enc,strlen(enc)+1);
            free(state->last_enc);
            state->last_enc=copy;
            state->last_seq=seq;
            state->last_author=s->member->id;
            state->last_num=num;
            state->last_den=den;
            state->has_last=1;
            state->used=1;
            if (!state->owner) state->owner=s->member->id;
        }
    }
    recent_evals[recent_pos%DIRECTOR_RECENT_EVALS].room=s->room;
    recent_evals[recent_pos%DIRECTOR_RECENT_EVALS].seq=seq;
    recent_evals[recent_pos%DIRECTOR_RECENT_EVALS].member=s->member->id;
    recent_pos++;
    out=cJSON_CreateObject();
    at=cJSON_CreateArray();
    if (!out || !at) {
        if (out) cJSON_Delete(out);
        if (at) cJSON_Delete(at);
        return;
    }
    cJSON_AddStringToObject(out,"t","eval");
    cJSON_AddNumberToObject(out,"slot",(double)slot);
    cJSON_AddNumberToObject(out,"author",(double)s->member->id);
    cJSON_AddNumberToObject(out,"eval_seq",(double)seq);
    cJSON_AddItemToArray(at,cJSON_CreateNumber((double)num));
    cJSON_AddItemToArray(at,cJSON_CreateNumber((double)den));
    cJSON_AddItemToObject(out,"at_cycle",at);
    {
        const cJSON *q=cJSON_GetObjectItemCaseSensitive((const cJSON *)obj,
            "q");
        if (q) cJSON_AddItemToObject(out,"q",cJSON_Duplicate(q,1));
    }
    cJSON_AddStringToObject(out,"enc",enc);
    text=json_print(out);
    if (!text) return;
    director_broadcast_text(s->room,NULL,text,strlen(text));
    free(text);
    if (msg->has_id) send_ack(s,msg->id);
}
/* --- tempo_req / transport_req (owner only) --- */
static void broadcast_clock(DirectorRoom *room) {
    cJSON *out=cJSON_CreateObject();
    char *text;
    if (!out) return;
    cJSON_AddStringToObject(out,"t","clock");
    build_clock(out,room);
    text=json_print(out);
    if (!text) return;
    director_broadcast_text(room,NULL,text,strlen(text));
    free(text);
}
static void handle_tempo_req(DirectorSession *s, const CpMsg *msg) {
    const void *obj=msg->owned;
    double cps=0;
    uint64_t num=0, act_us;
    unsigned den=64;
    if (!s->member->is_owner) {
        send_error(s,CP_E_PERM,"tempo needs the owner",msg->has_id,msg->id);
        return;
    }
    if (!cp_get_double(obj,"cps",&cps) || !(cps>0) || cps>4) {
        send_error(s,CP_E_SCHEMA,"cps in (0,4] required",msg->has_id,msg->id);
        return;
    }
    director_activation_at(s->room,director_now_us(),&num,&den,&act_us);
    s->room->has_next=1;
    s->room->next_num=num;
    s->room->next_den=den;
    s->room->next_us=act_us;
    s->room->next_cps=cps;
    broadcast_clock(s->room);
    if (msg->has_id) send_ack(s,msg->id);
}
static void handle_transport_req(DirectorSession *s, const CpMsg *msg) {
    const void *obj=msg->owned;
    const char *action=NULL;
    if (!s->member->is_owner) {
        send_error(s,CP_E_PERM,"transport is owner-only",msg->has_id,msg->id);
        return;
    }
    if (!cp_get_string(obj,"action",&action,16) || !action) {
        send_error(s,CP_E_SCHEMA,"action required",msg->has_id,msg->id);
        return;
    }
    if (!strcmp(action,"start")) {
        s->room->running=1;
        s->room->epoch_num=0;
        s->room->epoch_den=1;
        s->room->epoch_us=director_now_us();
        s->room->clock_seq++;
        s->room->has_next=0;
    } else if (!strcmp(action,"stop")) {
        s->room->running=0;
        s->room->clock_seq++;
    } else {
        send_error(s,CP_E_SCHEMA,"action must be start|stop",msg->has_id,
            msg->id);
        return;
    }
    broadcast_clock(s->room);
    if (msg->has_id) send_ack(s,msg->id);
}
/* --- sealed_msg / slider relay --- */
static void handle_sealed_relay(DirectorSession *s, const CpMsg *msg,
    const char *type) {
    const void *obj=msg->owned;
    const char *enc=NULL;
    unsigned slot=0;
    cJSON *out;
    char *text;
    if (!strcmp(type,"slider") && !check_slot(obj,&slot)) {
        send_error(s,CP_E_SCHEMA,"slot 0-15 required",msg->has_id,msg->id);
        return;
    }
    if (!cp_get_string(obj,"enc",&enc,CP_BIN_PAYLOAD_MAX) || !enc) {
        send_error(s,CP_E_SCHEMA,"enc required",msg->has_id,msg->id);
        return;
    }
    out=cJSON_CreateObject();
    if (!out) return;
    cJSON_AddStringToObject(out,"t",type);
    if (!strcmp(type,"slider"))
        cJSON_AddNumberToObject(out,"slot",(double)slot);
    else
        cJSON_AddNumberToObject(out,"from",(double)s->member->id);
    cJSON_AddStringToObject(out,"enc",enc);
    text=json_print(out);
    if (!text) return;
    director_broadcast_text(s->room,s,text,strlen(text));
    free(text);
}
/* --- slot ops --- */
static void handle_slot_create(DirectorSession *s, const CpMsg *msg) {
    const void *obj=msg->owned;
    const char *name=NULL;
    unsigned slot=0;
    DirectorSlot *state;
    if (!check_slot(obj,&slot)) {
        send_error(s,CP_E_SCHEMA,"slot 0-15 required",msg->has_id,msg->id);
        return;
    }
    state=&s->room->slots[slot];
    if (cp_get_string(obj,"slot_name",&name,1024) && name) {
        size_t n=strlen(name);
        char *copy=malloc(n+1);
        if (copy) {
            memcpy(copy,name,n+1);
            free(state->name_enc);
            state->name_enc=copy;
        }
    }
    if (!state->used) state->owner=s->member->id;
    state->used=1;
    {
        cJSON *out=cJSON_CreateObject();
        char *text;
        if (!out) return;
        cJSON_AddStringToObject(out,"t","slot_created");
        cJSON_AddNumberToObject(out,"slot",(double)slot);
        if (state->name_enc)
            cJSON_AddStringToObject(out,"slot_name",state->name_enc);
        cJSON_AddNumberToObject(out,"owner",(double)state->owner);
        text=json_print(out);
        if (!text) return;
        director_broadcast_text(s->room,NULL,text,strlen(text));
        free(text);
    }
    if (msg->has_id) send_ack(s,msg->id);
}
static int slot_may_edit(DirectorSession *s, unsigned slot) {
    DirectorSlot *state=&s->room->slots[slot];
    if (s->member->is_owner) return 1;
    return state->owner==s->member->id;
}
static void handle_slot_update(DirectorSession *s, const CpMsg *msg) {
    const void *obj=msg->owned;
    const char *name=NULL;
    unsigned slot=0;
    DirectorSlot *state;
    if (!check_slot(obj,&slot)) {
        send_error(s,CP_E_SCHEMA,"slot 0-15 required",msg->has_id,msg->id);
        return;
    }
    if (!slot_may_edit(s,slot)) {
        send_error(s,CP_E_PERM,"slot owner or room owner",msg->has_id,
            msg->id);
        return;
    }
    state=&s->room->slots[slot];
    if (cp_get_string(obj,"slot_name",&name,1024) && name) {
        size_t n=strlen(name);
        char *copy=malloc(n+1);
        if (copy) {
            memcpy(copy,name,n+1);
            free(state->name_enc);
            state->name_enc=copy;
        }
    }
    {
        const cJSON *muted=cJSON_GetObjectItemCaseSensitive(
            (const cJSON *)obj,"muted");
        if (muted) {
            if (!cJSON_IsBool(muted)) {
                send_error(s,CP_E_SCHEMA,"muted must be bool",msg->has_id,
                    msg->id);
                return;
            }
            state->muted=cJSON_IsTrue(muted);
        }
    }
    {
        cJSON *out=cJSON_CreateObject();
        char *text;
        if (!out) return;
        cJSON_AddStringToObject(out,"t","slot_updated");
        cJSON_AddNumberToObject(out,"slot",(double)slot);
        if (state->name_enc)
            cJSON_AddStringToObject(out,"slot_name",state->name_enc);
        cJSON_AddBoolToObject(out,"muted",state->muted);
        text=json_print(out);
        if (!text) return;
        director_broadcast_text(s->room,NULL,text,strlen(text));
        free(text);
    }
    if (msg->has_id) send_ack(s,msg->id);
}
static void handle_slot_delete(DirectorSession *s, const CpMsg *msg) {
    unsigned slot=0;
    if (!check_slot(msg->owned,&slot)) {
        send_error(s,CP_E_SCHEMA,"slot 0-15 required",msg->has_id,msg->id);
        return;
    }
    if (!slot_may_edit(s,slot)) {
        send_error(s,CP_E_PERM,"slot owner or room owner",msg->has_id,
            msg->id);
        return;
    }
    {
        DirectorSlot *state=&s->room->slots[slot];
        unsigned k;
        free(state->snapshot.data);
        free(state->name_enc);
        free(state->last_enc);
        if (state->log) {
            for (k=0;k<state->log_count;k++)
                free(state->log[(state->log_start+k)%DIRECTOR_LOG_CAP].data);
            free(state->log);
        }
        memset(state,0,sizeof(*state));
    }
    {
        cJSON *out=cJSON_CreateObject();
        char *text;
        if (!out) return;
        cJSON_AddStringToObject(out,"t","slot_deleted");
        cJSON_AddNumberToObject(out,"slot",(double)slot);
        text=json_print(out);
        if (!text) return;
        director_broadcast_text(s->room,NULL,text,strlen(text));
        free(text);
    }
    if (msg->has_id) send_ack(s,msg->id);
}
/* --- room_update / room_delete (owner only) --- */
static void handle_room_update(DirectorSession *s, const CpMsg *msg) {
    const void *obj=msg->owned;
    if (!s->member->is_owner) {
        send_error(s,CP_E_PERM,"room config is owner-only",msg->has_id,
            msg->id);
        return;
    }
    {
        const cJSON *locked=cJSON_GetObjectItemCaseSensitive(
            (const cJSON *)obj,"locked");
        if (locked) {
            if (!cJSON_IsBool(locked)) {
                send_error(s,CP_E_SCHEMA,"locked must be bool",msg->has_id,
                    msg->id);
                return;
            }
            s->room->locked=cJSON_IsTrue(locked);
        }
    }
    {
        const cJSON *q=cJSON_GetObjectItemCaseSensitive((const cJSON *)obj,
            "quantum");
        if (q) {
            uint64_t num=0;
            unsigned den=1;
            if (!cp_get_cycle(obj,"quantum",&num,&den)) {
                send_error(s,CP_E_SCHEMA,"bad quantum",msg->has_id,msg->id);
                return;
            }
            s->room->quantum_num=num;
            s->room->quantum_den=den;
        }
    }
    {
        uint32_t max_members=0;
        if (cJSON_GetObjectItemCaseSensitive((const cJSON *)obj,
            "max_members")) {
            if (!cp_get_u32(obj,"max_members",&max_members) ||
                max_members==0 || max_members>DIRECTOR_MEMBERS_MAX) {
                send_error(s,CP_E_SCHEMA,"max_members out of range",
                    msg->has_id,msg->id);
                return;
            }
            s->room->max_members=max_members;
        }
    }
    {
        const cJSON *off=cJSON_GetObjectItemCaseSensitive(
            (const cJSON *)obj,"solos_off");
        if (off) {
            if (!cJSON_IsBool(off)) {
                send_error(s,CP_E_SCHEMA,"solos_off must be bool",
                    msg->has_id,msg->id);
                return;
            }
            s->room->solos_off=cJSON_IsTrue(off);
        }
    }
    {
        cJSON *out=cJSON_CreateObject(), *robj=cJSON_CreateObject();
        char *text;
        if (!out || !robj) {
            cJSON_Delete(out);
            cJSON_Delete(robj);
            return;
        }
        cJSON_AddStringToObject(out,"t","room_updated");
        json_add_cycle(robj,"quantum",s->room->quantum_num,
            s->room->quantum_den);
        cJSON_AddBoolToObject(robj,"locked",s->room->locked);
        cJSON_AddBoolToObject(robj,"solos_off",s->room->solos_off);
        cJSON_AddNumberToObject(robj,"max_members",
            (double)s->room->max_members);
        cJSON_AddItemToObject(out,"room",robj);
        text=json_print(out);
        if (text) {
            director_broadcast_text(s->room,NULL,text,strlen(text));
            free(text);
        }
    }
    if (msg->has_id) send_ack(s,msg->id);
}
static void handle_room_delete(DirectorSession *s, const CpMsg *msg) {
    DirectorRoom *room;
    unsigned i;
    (void)msg;
    if (!s->member->is_owner) {
        send_error(s,CP_E_PERM,"room delete is owner-only",msg->has_id,
            msg->id);
        return;
    }
    room=s->room;
    for (i=0;i<DIRECTOR_SESSIONS_MAX;i++) {
        DirectorSession *peer=sessions[i];
        if (peer && peer->room==room) {
            peer->room=NULL;
            peer->member=NULL;
            close_with(peer,1000,"room deleted");
        }
    }
    director_room_destroy(room);
}
/* --- ack (late-activation relay to the eval author) --- */
static void handle_ack(DirectorSession *s, const CpMsg *msg) {
    const void *obj=msg->owned;
    uint32_t ref=0;
    unsigned i;
    if (!cp_get_u32(obj,"ref",&ref)) return;
    for (i=0;i<DIRECTOR_RECENT_EVALS;i++) {
        unsigned j;
        if (recent_evals[i].room!=s->room ||
            recent_evals[i].seq!=ref)
            continue;
        for (j=0;j<DIRECTOR_SESSIONS_MAX;j++) {
            DirectorSession *peer=sessions[j];
            cJSON *out;
            char *text;
            if (!peer || peer==s || peer->room!=s->room || !peer->member)
                continue;
            if (peer->member->id!=recent_evals[i].member) continue;
            out=cJSON_CreateObject();
            if (!out) return;
            cJSON_AddStringToObject(out,"t","ack");
            cJSON_AddNumberToObject(out,"ref",(double)ref);
            {
                const cJSON *late=cJSON_GetObjectItemCaseSensitive(
                    (const cJSON *)obj,"late_by");
                if (cJSON_IsNumber(late))
                    cJSON_AddNumberToObject(out,"late_by",late->valuedouble);
            }
            text=json_print(out);
            if (!text) return;
            director_session_send(peer,text,strlen(text));
            free(text);
            return;
        }
        return;
    }
    if (msg->has_id) send_ack(s,msg->id);
}
/* --- leave --- */
static void handle_leave(DirectorSession *s, const CpMsg *msg) {
    if (msg->has_id) send_ack(s,msg->id);
    /* Explicit leave drops the member record; a bare disconnect keeps it
       for resume (see director_session_closed). */
    director_session_closed(s,0);
    close_with(s,1000,"bye");
}
/* --- dispatcher --- */
static int is_client_type(CpType type) {
    switch (type) {
    case CP_T_HELLO: case CP_T_AUTH: case CP_T_JOIN: case CP_T_PING:
    case CP_T_EVAL_REQ: case CP_T_TEMPO_REQ: case CP_T_TRANSPORT_REQ:
    case CP_T_SLOT_CREATE: case CP_T_SLOT_UPDATE: case CP_T_SLOT_DELETE:
    case CP_T_ACK: case CP_T_SLIDER: case CP_T_INVITE_CREATE:
    case CP_T_INVITE_REVOKE: case CP_T_INVITE_LIST: case CP_T_MEMBER_KICK:
    case CP_T_MEMBER_BAN: case CP_T_REKEY: case CP_T_REKEY_ACK:
    case CP_T_ROOM_UPDATE: case CP_T_ROOM_DELETE: case CP_T_SEALED_MSG:
    case CP_T_LEAVE:
        return 1;
    default:
        break;
    }
    return 0;
}
static void dispatch_control(DirectorSession *s, const CpMsg *msg,
    const DirectorConfig *config) {
    if (!is_client_type(msg->type)) {
        send_error(s,CP_E_TYPE,"unknown or server-side type",
            msg->has_id,msg->id);
        return;
    }
    if (msg->type!=CP_T_HELLO && s->phase==0) {
        send_error(s,CP_E_PERM,"hello first",msg->has_id,msg->id);
        return;
    }
    if (msg->type!=CP_T_HELLO && msg->type!=CP_T_AUTH && s->phase<2) {
        send_error(s,CP_E_PERM,"auth required",msg->has_id,msg->id);
        return;
    }
    switch (msg->type) {
    case CP_T_HELLO:
        handle_hello(s,msg,config);
        break;
    case CP_T_AUTH:
        handle_auth(s,msg,config);
        break;
    case CP_T_JOIN:
        handle_join(s,msg);
        break;
    case CP_T_PING:
        handle_ping(s,msg);
        break;
    case CP_T_EVAL_REQ:
        handle_eval_req(s,msg);
        break;
    case CP_T_TEMPO_REQ:
        handle_tempo_req(s,msg);
        break;
    case CP_T_TRANSPORT_REQ:
        handle_transport_req(s,msg);
        break;
    case CP_T_SEALED_MSG:
        handle_sealed_relay(s,msg,"sealed_msg");
        break;
    case CP_T_SLIDER:
        handle_sealed_relay(s,msg,"slider");
        break;
    case CP_T_SLOT_CREATE:
        handle_slot_create(s,msg);
        break;
    case CP_T_SLOT_UPDATE:
        handle_slot_update(s,msg);
        break;
    case CP_T_SLOT_DELETE:
        handle_slot_delete(s,msg);
        break;
    case CP_T_ROOM_UPDATE:
        handle_room_update(s,msg);
        break;
    case CP_T_ROOM_DELETE:
        handle_room_delete(s,msg);
        break;
    case CP_T_ACK:
        handle_ack(s,msg);
        break;
    case CP_T_LEAVE:
        handle_leave(s,msg);
        break;
    case CP_T_INVITE_CREATE: case CP_T_INVITE_REVOKE: case CP_T_INVITE_LIST:
    case CP_T_MEMBER_KICK: case CP_T_MEMBER_BAN: case CP_T_REKEY:
        if (!s->member->is_owner) {
            send_error(s,CP_E_PERM,"owner only",msg->has_id,msg->id);
            break;
        }
        send_error(s,CP_E_INTERNAL,"phase 2",msg->has_id,msg->id);
        break;
    case CP_T_REKEY_ACK:
        if (msg->has_id) send_ack(s,msg->id);
        break;
    default:
        send_error(s,CP_E_TYPE,"unknown type",msg->has_id,msg->id);
        break;
    }
}
/* --- binary frames --- */
static void relay_binary(DirectorRoom *room, DirectorSession *except,
    uint8_t kind, unsigned slot, uint32_t member, uint32_t seq,
    uint16_t epoch, const uint8_t *payload, size_t len) {
    uint8_t *frame=malloc(20+len);
    if (!frame) return;
    frame[0]=CP_BIN_VER;
    frame[1]=kind;
    frame[2]=(uint8_t)(slot>>8);
    frame[3]=(uint8_t)slot;
    frame[4]=(uint8_t)(member>>24);
    frame[5]=(uint8_t)(member>>16);
    frame[6]=(uint8_t)(member>>8);
    frame[7]=(uint8_t)member;
    frame[8]=(uint8_t)(seq>>24);
    frame[9]=(uint8_t)(seq>>16);
    frame[10]=(uint8_t)(seq>>8);
    frame[11]=(uint8_t)seq;
    frame[12]=(uint8_t)(epoch>>8);
    frame[13]=(uint8_t)epoch;
    frame[14]=0;
    frame[15]=0;
    frame[16]=(uint8_t)(len>>24);
    frame[17]=(uint8_t)(len>>16);
    frame[18]=(uint8_t)(len>>8);
    frame[19]=(uint8_t)len;
    if (len) memcpy(frame+20,payload,len);
    director_broadcast_binary(room,except,frame,20+len);
    free(frame);
}
static void handle_binary(DirectorSession *s, const uint8_t *frame,
    size_t len, const DirectorConfig *config) {
    CpBinHeader hdr;
    const uint8_t *payload=NULL;
    size_t payload_len=0;
    CpError error;
    if (s->phase<2 || !s->room || !s->member) {
        send_error(s,CP_E_PERM,"auth required",0,0);
        return;
    }
    error=cp_parse_binary(frame,len,&hdr,&payload,&payload_len);
    if (error!=CP_E_NONE) {
        if (director_strike(s->room,s->member,director_now_us())>=3)
            close_with(s,1008,"bad frame");
        else
            send_error(s,error,"binary rejected",0,0);
        return;
    }
    if (hdr.slot>=DIRECTOR_SLOTS_MAX) {
        send_error(s,CP_E_SCHEMA,"slot out of range",0,0);
        return;
    }
    if ((uint64_t)hdr.epoch<s->room->key_epoch) {
        send_error(s,CP_E_EPOCH,"stale epoch",0,0);
        return;
    }
    /* The director never decrypts, but it does check who may write and
       that the author actually signed the frame (section 14.5). The
       signature binds author_pk, so a malicious director cannot forge
       writes and a malicious member cannot forge another member's. */
    {
        DirectorSlot *state=&s->room->slots[hdr.slot];
        int writable=s->member->is_owner || hdr.slot==0 ||
            (state->used && state->owner==s->member->id);
        if (!writable) {
            send_error(s,CP_E_PERM,"slot owner or room owner",0,0);
            return;
        }
    }
    if (hdr.flags&CP_BIN_FLAG_PLAINTEXT) {
        if (!config->allow_plaintext) {
            if (director_strike(s->room,s->member,director_now_us())>=3)
                close_with(s,1008,"plaintext not allowed");
            else
                send_error(s,CP_E_PERM,"plaintext not allowed",0,0);
            return;
        }
    } else {
        uint8_t ad[79];
        uint8_t *msg;
        size_t body_len;
        if (payload_len<24+16+64) {
            if (director_strike(s->room,s->member,director_now_us())>=3)
                close_with(s,1008,"bad frame");
            else
                send_error(s,CP_E_SCHEMA,"frame too short to sign",0,0);
            return;
        }
        body_len=payload_len-64;
        memcpy(ad,"tpc-frame-v1",12);
        memcpy(ad+12,s->room->id,26);
        ad[38]=(uint8_t)((hdr.epoch>>8)&0xff);
        ad[39]=(uint8_t)(hdr.epoch&0xff);
        ad[40]=(uint8_t)((hdr.slot>>8)&0xff);
        ad[41]=(uint8_t)(hdr.slot&0xff);
        ad[42]=hdr.kind;
        ad[43]=(uint8_t)((hdr.seq>>24)&0xff);
        ad[44]=(uint8_t)((hdr.seq>>16)&0xff);
        ad[45]=(uint8_t)((hdr.seq>>8)&0xff);
        ad[46]=(uint8_t)(hdr.seq&0xff);
        memcpy(ad+47,s->member->pk,32);
        msg=malloc(sizeof(ad)+body_len);
        if (!msg) {
            send_error(s,CP_E_INTERNAL,"out of memory",0,0);
            return;
        }
        memcpy(msg,ad,sizeof(ad));
        memcpy(msg+sizeof(ad),payload,body_len);
        if (crypto_sign_verify_detached(payload+body_len,msg,
                sizeof(ad)+body_len,s->member->pk)!=0) {
            free(msg);
            if (director_strike(s->room,s->member,director_now_us())>=3)
                close_with(s,1008,"bad signature");
            else
                send_error(s,CP_E_PERM,"bad signature",0,0);
            return;
        }
        free(msg);
    }
    director_store_frame(s->room,hdr.slot,s->member->id,hdr.seq,hdr.epoch,
        hdr.kind,payload,payload_len,hdr.kind==3);
    relay_binary(s->room,s,hdr.kind,hdr.slot,s->member->id,hdr.seq,
        hdr.epoch,payload,payload_len);
}
/* --- RX entry --- */
#define DIRECTOR_RX_MAX (1024u*1024u)
#define DIRECTOR_RX_BIN_HEAD 20u
static void rx_discard(DirectorSession *s) {
    free(s->rx);
    s->rx=NULL;
    s->rx_len=0;
    s->rx_cap=0;
    s->rx_oversize=0;
}
void director_session_receive(DirectorSession *s, const uint8_t *data,
    size_t len, int binary, int first, int final,
    const DirectorConfig *config) {
    size_t cap;
    uint64_t now;
    (void)config;
    if (!s || !data) return;
    now=director_now_us();
    s->last_rx_us=now;
    {
        DirectorBucket *bucket=s->member?&s->member->bucket:&s->bucket;
        if (!director_bucket_consume(bucket,binary,now)) {
            send_error(s,CP_E_RATE,"too fast",0,0);
            return;
        }
    }
    if (first) {
        rx_discard(s);
        s->rx_binary=binary;
    } else if (binary!=s->rx_binary) {
        rx_discard(s);
        return;
    }
    if (s->rx_oversize) {
        if (final) rx_discard(s);
        return;
    }
    if (binary && s->rx_len<DIRECTOR_RX_BIN_HEAD) {
        size_t have=s->rx_len+len;
        if (have>DIRECTOR_RX_BIN_HEAD+CP_SNAPSHOT_MAX) {
            s->rx_oversize=1;
            if (final) rx_discard(s);
            else close_with(s,1009,"binary frame too large");
            return;
        }
    }
    cap=s->rx_cap?s->rx_cap:4096;
    while (cap<s->rx_len+len) {
        size_t hard=s->rx_binary?DIRECTOR_RX_BIN_HEAD+CP_SNAPSHOT_MAX:
            DIRECTOR_RX_MAX;
        if (cap>=hard) {
            s->rx_oversize=1;
            close_with(s,1009,"frame too large");
            return;
        }
        cap*=2;
        if (cap>hard) cap=hard;
    }
    if (cap!=s->rx_cap) {
        uint8_t *grown=realloc(s->rx,cap);
        if (!grown) {
            rx_discard(s);
            return;
        }
        s->rx=grown;
        s->rx_cap=cap;
    }
    memcpy(s->rx+s->rx_len,data,len);
    s->rx_len+=len;
    if (!final) return;
    if (s->rx_binary &&
        s->rx_len>DIRECTOR_RX_BIN_HEAD+CP_BIN_PAYLOAD_MAX) {
        if (s->rx_len<DIRECTOR_RX_BIN_HEAD || s->rx[1]!=3) {
            rx_discard(s);
            close_with(s,1009,"binary frame too large");
            return;
        }
    }
    if (s->rx_binary) {
        handle_binary(s,s->rx,s->rx_len,config);
    } else {
        CpMsg msg={0};
        CpError error=cp_parse_control((const char *)s->rx,s->rx_len,&msg);
        if (error!=CP_E_NONE) {
            if (error==CP_E_SIZE) {
                rx_discard(s);
                close_with(s,1009,"control frame too large");
                return;
            }
            if (director_strike(s->room,s->member,
                director_now_us())>=3) {
                rx_discard(s);
                close_with(s,1008,"bad control frame");
                return;
            }
            rx_discard(s);
            send_error(s,error,"control rejected",0,0);
            return;
        }
        dispatch_control(s,&msg,config);
        cp_msg_free(&msg);
        rx_discard(s);
    }
}
/* --- sweep --- */
void director_sweep_rooms(uint64_t now_us);
static int member_has_session(DirectorMember *member) {
    unsigned i;
    for (i=0;i<DIRECTOR_SESSIONS_MAX;i++)
        if (sessions[i] && sessions[i]->member==member) return 1;
    return 0;
}
void director_session_closed(DirectorSession *session, int keep_member) {
    if (!session) return;
    if (session->room && session->member) {
        DirectorRoom *room=session->room;
        DirectorMember *member=session->member;
        session->member=NULL;
        session->room=NULL;
        if (keep_member) {
            send_member_event(room,"member_left",member,NULL);
        } else {
            send_member_event(room,"member_left",member,NULL);
            director_member_remove(room,member);
        }
    }
}
void director_session_sweep(uint64_t now_us, const DirectorConfig *config) {
    unsigned i;
    (void)config;
    for (i=0;i<DIRECTOR_SESSIONS_MAX;i++) {
        DirectorSession *s=sessions[i];
        if (!s) continue;
        if (s->phase==0 &&
            now_us-s->connected_us>DIRECTOR_HELLO_TIMEOUT_US)
            close_with(s,1008,"hello timeout");
        if (s->room && s->room->has_next && now_us>=s->room->next_us) {
            s->room->epoch_num=s->room->next_num;
            s->room->epoch_den=s->room->next_den;
            s->room->epoch_us=s->room->next_us;
            s->room->epoch_cps=s->room->next_cps;
            s->room->clock_seq++;
            s->room->has_next=0;
            broadcast_clock(s->room);
        }
    }
    director_sweep_rooms(now_us);
}
void director_sweep_rooms(uint64_t now_us) {
    unsigned r, i;
    for (r=0;r<DIRECTOR_ROOMS_MAX;r++) {
        DirectorRoom *room=director_room_at(r);
        if (!room || !room->used) continue;
        for (i=0;i<DIRECTOR_MEMBERS_MAX;i++) {
            DirectorMember *member=&room->members[i];
            if (!member->used) continue;
            if (member->resume.present &&
                now_us-member->resume.issued_us>
                (uint64_t)DIRECTOR_RESUME_SECS*1000000u)
                member->resume.present=0;
            if (!member_has_session(member) &&
                now_us-member->last_seen_us>
                (uint64_t)DIRECTOR_RESUME_SECS*1000000u) {
                send_member_event(room,"member_left",member,NULL);
                director_member_remove(room,member);
            }
        }
        /* Reclaim memberless rooms (anonymous squats, abandoned lobbies):
           section 18.4 deletes unused rooms, so the table cannot fill. */
        if (director_room_expired(room,now_us)) {
            unsigned s;
            for (s=0;s<DIRECTOR_SESSIONS_MAX;s++) {
                DirectorSession *st=sessions[s];
                if (st && st->room==room) {
                    st->room=NULL;
                    st->member=NULL;
                    close_with(st,1000,"room expired");
                }
            }
            director_room_destroy(room);
        }
    }
}
