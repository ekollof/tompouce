#include "collab_proto.h"
#include <cjson/cJSON.h>
#include <math.h>
#include <string.h>
typedef struct { const char *name; CpType type; } CpTypeName;
static const CpTypeName type_names[] = {
    {"hello",CP_T_HELLO},{"auth",CP_T_AUTH},{"join",CP_T_JOIN},
    {"ping",CP_T_PING},{"eval_req",CP_T_EVAL_REQ},{"tempo_req",CP_T_TEMPO_REQ},
    {"transport_req",CP_T_TRANSPORT_REQ},{"slot_create",CP_T_SLOT_CREATE},
    {"slot_update",CP_T_SLOT_UPDATE},{"slot_delete",CP_T_SLOT_DELETE},
    {"ack",CP_T_ACK},{"slider",CP_T_SLIDER},{"invite_create",CP_T_INVITE_CREATE},
    {"invite_revoke",CP_T_INVITE_REVOKE},{"invite_list",CP_T_INVITE_LIST},
    {"member_kick",CP_T_MEMBER_KICK},{"member_ban",CP_T_MEMBER_BAN},
    {"rekey",CP_T_REKEY},{"rekey_ack",CP_T_REKEY_ACK},
    {"room_update",CP_T_ROOM_UPDATE},{"room_delete",CP_T_ROOM_DELETE},
    {"sealed_msg",CP_T_SEALED_MSG},{"leave",CP_T_LEAVE},
    {"welcome",CP_T_WELCOME},{"auth_ok",CP_T_AUTH_OK},{"joined",CP_T_JOINED},
    {"synced",CP_T_SYNCED},{"pong",CP_T_PONG},{"clock",CP_T_CLOCK},
    {"eval",CP_T_EVAL},{"snapshot_req",CP_T_SNAPSHOT_REQ},
    {"member_joined",CP_T_MEMBER_JOINED},{"member_left",CP_T_MEMBER_LEFT},
    {"member_updated",CP_T_MEMBER_UPDATED},
    {"member_kicked",CP_T_MEMBER_KICKED},{"rekey_out",CP_T_REKEY_OUT},
    {"slot_created",CP_T_SLOT_CREATED},{"slot_updated",CP_T_SLOT_UPDATED},
    {"slot_deleted",CP_T_SLOT_DELETED},{"invites",CP_T_INVITES},
    {"error",CP_T_ERROR},{"kicked",CP_T_KICKED},
};
const char *cp_error_string(CpError error) {
    switch (error) {
    case CP_E_NONE: return "ok";
    case CP_E_PROTO: return "proto";
    case CP_E_TYPE: return "type";
    case CP_E_SCHEMA: return "schema";
    case CP_E_AUTH: return "auth";
    case CP_E_PERM: return "perm";
    case CP_E_RATE: return "rate";
    case CP_E_SIZE: return "size";
    case CP_E_ROOM_FULL: return "room_full";
    case CP_E_LOCKED: return "locked";
    case CP_E_BANNED: return "banned";
    case CP_E_NOT_FOUND: return "not_found";
    case CP_E_E2E_REQUIRED: return "e2e_required";
    case CP_E_INVITE_INVALID: return "invite_invalid";
    case CP_E_EPOCH: return "epoch";
    case CP_E_QUOTA: return "quota";
    default: break;
    }
    return "internal";
}
CpType cp_type_from_string(const char *name) {
    unsigned i;
    if (!name) return CP_T_UNKNOWN;
    for (i=0;i<sizeof(type_names)/sizeof(type_names[0]);i++)
        if (!strcmp(name,type_names[i].name)) return type_names[i].type;
    return CP_T_UNKNOWN;
}
const char *cp_type_string(CpType type) {
    unsigned i;
    for (i=0;i<sizeof(type_names)/sizeof(type_names[0]);i++)
        if (type_names[i].type==type) return type_names[i].name;
    return "unknown";
}
/* Depth-first nesting check; arrays and objects both count. */
static int depth_ok(const cJSON *node, int depth) {
    const cJSON *child;
    if (depth>CP_JSON_DEPTH_MAX) return 0;
    for (child=node?node->child:NULL;child;child=child->next)
        if ((cJSON_IsObject(child) || cJSON_IsArray(child)) &&
            !depth_ok(child,depth+1)) return 0;
    return 1;
}
static size_t type_cap(CpType type) {
    switch (type) {
    case CP_T_EVAL:
    case CP_T_EVAL_REQ:
        return CP_COMPOSITE_MAX;
    case CP_T_JOINED:
        return CP_JOINED_MAX;
    default:
        break;
    }
    return CP_CONTROL_MAX;
}
CpError cp_parse_control(const char *text, size_t len, CpMsg *out) {
    const cJSON *type_item, *id_item;
    cJSON *root;
    if (!text || !out || len==0 || len>CP_JOINED_MAX) return CP_E_SIZE;
    root=cJSON_ParseWithLength(text,len);
    if (!root) return CP_E_SCHEMA;
    if (!cJSON_IsObject(root) || !depth_ok(root,1)) {
        cJSON_Delete(root);
        return CP_E_SCHEMA;
    }
    type_item=cJSON_GetObjectItemCaseSensitive(root,"t");
    if (!cJSON_IsString(type_item) || !type_item->valuestring) {
        cJSON_Delete(root);
        return CP_E_SCHEMA;
    }
    out->type=cp_type_from_string(type_item->valuestring);
    if (out->type==CP_T_UNKNOWN) {
        cJSON_Delete(root);
        return CP_E_TYPE;
    }
    if (len>type_cap(out->type)) {
        cJSON_Delete(root);
        return CP_E_SIZE;
    }
    out->has_id=0;
    out->id=0;
    id_item=cJSON_GetObjectItemCaseSensitive(root,"id");
    if (id_item) {
        double id;
        if (!cJSON_IsNumber(id_item) || !isfinite(id_item->valuedouble)) {
            cJSON_Delete(root);
            return CP_E_SCHEMA;
        }
        id=id_item->valuedouble;
        if (id<0 || id>4294967295.0 || floor(id)!=id) {
            cJSON_Delete(root);
            return CP_E_SCHEMA;
        }
        out->has_id=1;
        out->id=(uint32_t)id;
    }
    out->owned=root;
    return CP_E_NONE;
}
void cp_msg_free(CpMsg *msg) {
    if (msg && msg->owned) {
        cJSON_Delete((cJSON *)msg->owned);
        msg->owned=NULL;
    }
}
static const cJSON *field(const void *obj, const char *key) {
    if (!obj || !key) return NULL;
    return cJSON_GetObjectItemCaseSensitive((const cJSON *)obj,key);
}
int cp_get_string(const void *obj, const char *key, const char **value,
    size_t maxlen) {
    const cJSON *item=field(obj,key);
    if (!cJSON_IsString(item) || !item->valuestring) return 0;
    if (strlen(item->valuestring)>maxlen) return 0;
    *value=item->valuestring;
    return 1;
}
int cp_get_room_id(const void *obj, const char *key, const char **value) {
    const char *text;
    size_t i;
    if (!cp_get_string(obj,key,&text,CP_ROOM_ID_LEN)) return 0;
    if (strlen(text)!=CP_ROOM_ID_LEN) return 0;
    for (i=0;i<CP_ROOM_ID_LEN;i++) {
        char c=text[i];
        int lower=c>='a' && c<='z', digit=c>='2' && c<='7';
        if (!lower && !digit) return 0;
    }
    *value=text;
    return 1;
}
int cp_get_u32(const void *obj, const char *key, uint32_t *value) {
    const cJSON *item=field(obj,key);
    double number;
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)) return 0;
    number=item->valuedouble;
    if (number<0 || number>4294967295.0 || floor(number)!=number) return 0;
    *value=(uint32_t)number;
    return 1;
}
int cp_get_bool(const void *obj, const char *key, int *value) {
    const cJSON *item=field(obj,key);
    if (!cJSON_IsBool(item)) return 0;
    *value=cJSON_IsTrue(item);
    return 1;
}
int cp_get_double(const void *obj, const char *key, double *value) {
    const cJSON *item=field(obj,key);
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)) return 0;
    *value=item->valuedouble;
    return 1;
}
int cp_get_cycle(const void *obj, const char *key, uint64_t *num,
    unsigned *den) {
    const cJSON *item=field(obj,key), *first, *second;
    double n, d;
    if (!cJSON_IsArray(item) || cJSON_GetArraySize(item)!=2) return 0;
    first=cJSON_GetArrayItem(item,0);
    second=cJSON_GetArrayItem(item,1);
    if (!cJSON_IsNumber(first) || !cJSON_IsNumber(second)) return 0;
    n=first->valuedouble;
    d=second->valuedouble;
    if (!isfinite(n) || !isfinite(d) || n<0 ||
        n>=9007199254740992.0 || floor(n)!=n) return 0;
    switch ((int)d) {
    case 1: case 2: case 4: case 8: case 16: case 32: case 64:
        break;
    default:
        return 0;
    }
    *num=(uint64_t)n;
    *den=(unsigned)d;
    return 1;
}
uint64_t cp_cycle_to_double_num(uint64_t num, unsigned den, double *value) {
    if (!den || !value) return 0;
    *value=(double)num/(double)den;
    return 1;
}
static int b64_value(char c, unsigned *value) {
    if (c>='A' && c<='Z') *value=(unsigned)(c-'A');
    else if (c>='a' && c<='z') *value=(unsigned)(c-'a'+26);
    else if (c>='0' && c<='9') *value=(unsigned)(c-'0'+52);
    else if (c=='-') *value=62;
    else if (c=='_') *value=63;
    else return 0;
    return 1;
}
int cp_b64_decode(const char *in, uint8_t *out, size_t cap,
    size_t *outlen) {
    size_t len, full, i, at=0;
    unsigned a, b, c, d;
    if (!in || !out || !outlen) return 0;
    len=strlen(in);
    if (len%4==1) return 0;
    full=len/4*3;
    if (len%4==2) full+=1;
    else if (len%4==3) full+=2;
    if (full>cap) return 0;
    for (i=0;i+4<=len;i+=4) {
        if (in[i]=='=' || in[i+1]=='=' || in[i+2]=='=' || in[i+3]=='=') return 0;
        if (!b64_value(in[i],&a) || !b64_value(in[i+1],&b) ||
            !b64_value(in[i+2],&c) || !b64_value(in[i+3],&d)) return 0;
        out[at++]=(uint8_t)(a<<2 | b>>4);
        out[at++]=(uint8_t)(b<<4 | c>>2);
        out[at++]=(uint8_t)(c<<6 | d);
    }
    if (len%4==2) {
        if (in[len-2]=='=' || in[len-1]=='=') return 0;
        if (!b64_value(in[len-2],&a) || !b64_value(in[len-1],&b)) return 0;
        if (b&15) return 0;
        out[at++]=(uint8_t)(a<<2 | b>>4);
    } else if (len%4==3) {
        if (in[len-3]=='=' || in[len-2]=='=' || in[len-1]=='=') return 0;
        if (!b64_value(in[len-3],&a) || !b64_value(in[len-2],&b) ||
            !b64_value(in[len-1],&c)) return 0;
        if (c&3) return 0;
        out[at++]=(uint8_t)(a<<2 | b>>4);
        out[at++]=(uint8_t)(b<<4 | c>>2);
    }
    *outlen=at;
    return 1;
}
size_t cp_b64_encode(const uint8_t *in, size_t len, char *out, size_t cap) {
    static const char alphabet[]=
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t need=len/3*4+(len%3?len%3+1:0)+1, i, at=0;
    if (!in || !out || need>cap) return 0;
    for (i=0;i+3<=len;i+=3) {
        out[at++]=alphabet[in[i]>>2];
        out[at++]=alphabet[((in[i]&3)<<4)|(in[i+1]>>4)];
        out[at++]=alphabet[((in[i+1]&15)<<2)|(in[i+2]>>6)];
        out[at++]=alphabet[in[i+2]&63];
    }
    if (len%3==1) {
        out[at++]=alphabet[in[len-1]>>2];
        out[at++]=alphabet[(in[len-1]&3)<<4];
    } else if (len%3==2) {
        out[at++]=alphabet[in[len-2]>>2];
        out[at++]=alphabet[((in[len-2]&3)<<4)|(in[len-1]>>4)];
        out[at++]=alphabet[(in[len-1]&15)<<2];
    }
    out[at]=0;
    return at;
}
int cp_get_b64(const void *obj, const char *key, uint8_t *out,
    size_t maxlen, size_t *outlen) {
    const char *text;
    if (!cp_get_string(obj,key,&text,65536)) return 0;
    return cp_b64_decode(text,out,maxlen,outlen);
}
static uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0]<<8 | p[1]);
}
static uint32_t read_u32(const uint8_t *p) {
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 |
        (uint32_t)p[2]<<8 | p[3];
}
CpError cp_parse_binary(const uint8_t *frame, size_t len, CpBinHeader *hdr,
    const uint8_t **payload, size_t *payload_len) {
    uint32_t declared;
    size_t cap;
    if (!frame || !hdr || !payload || !payload_len) return CP_E_SCHEMA;
    if (len<CP_BIN_HEADER) return CP_E_SCHEMA;
    if (frame[0]!=CP_BIN_VER) return CP_E_PROTO;
    if (frame[1]<1 || frame[1]>4) return CP_E_SCHEMA;
    if (frame[15]!=0) return CP_E_SCHEMA;
    if (frame[14]&~CP_BIN_FLAG_PLAINTEXT) return CP_E_SCHEMA;
    declared=read_u32(frame+16);
    if (declared!=len-CP_BIN_HEADER) return CP_E_SCHEMA;
    cap=frame[1]==3?CP_SNAPSHOT_MAX:CP_BIN_PAYLOAD_MAX;
    if (declared>cap) return CP_E_SIZE;
    hdr->kind=frame[1];
    hdr->slot=read_u16(frame+2);
    hdr->member=read_u32(frame+4);
    hdr->seq=read_u32(frame+8);
    hdr->epoch=read_u16(frame+12);
    hdr->flags=frame[14];
    *payload=frame+CP_BIN_HEADER;
    *payload_len=declared;
    return CP_E_NONE;
}
