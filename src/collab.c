#include "collab.h"
#include "collab_proto.h"
#include "collab_transport.h"
#include "javascript.h"
#include "track.h"
#include "audio.h"
#include <cjson/cJSON.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
/* Engine version claim, matching src/mcp.c. */
#define COLLAB_CLIENT_VERSION "0.1.0"
/* Nick limits from section 13.5. */
#define NICK_BYTES_MAX 32
#define NICK_COLS_MAX 24
/* collab.json access mirrors section 18.3: XDG_CONFIG_HOME or ~/.config. */
#define COLLAB_JSON_BYTES_MAX 65536
static CollabState collab_current_state=COLLAB_IDLE;
static CollabTransport *collab_transport=NULL;
static char collab_url[512]="";
static char collab_nick[64]="";
const char *collab_state_string(CollabState state) {
    switch (state) {
    case COLLAB_IDLE: return "idle";
    case COLLAB_CONNECTING: return "connecting";
    case COLLAB_CONNECTED: return "connected";
    case COLLAB_BACKOFF: return "backoff";
    case COLLAB_FAILED: return "failed";
    }
    return "unknown";
}
CollabState collab_state(void) {
    return collab_current_state;
}
/* --- nick validation (section 13.5) --- */
/* Decode one UTF-8 sequence. Returns the code point and sets *len, or -1. */
static int nick_decode(const uint8_t *s, size_t left, size_t *len) {
    uint8_t first;
    int point;
    if (left==0) return -1;
    first=s[0];
    if (first<0x80) {
        *len=1;
        return first;
    }
    if ((first&0xe0)==0xc0) {
        if (left<2 || (s[1]&0xc0)!=0x80) return -1;
        point=((first&0x1f)<<6)|(s[1]&0x3f);
        if (point<0x80) return -1; /* overlong */
        *len=2;
        return point;
    }
    if ((first&0xf0)==0xe0) {
        if (left<3 || (s[1]&0xc0)!=0x80 || (s[2]&0xc0)!=0x80) return -1;
        point=((first&0x0f)<<12)|((s[1]&0x3f)<<6)|(s[2]&0x3f);
        if (point<0x800) return -1; /* overlong */
        if (point>=0xd800 && point<=0xdfff) return -1; /* surrogate */
        *len=3;
        return point;
    }
    if ((first&0xf8)==0xf0) {
        if (left<4 || (s[1]&0xc0)!=0x80 || (s[2]&0xc0)!=0x80 ||
            (s[3]&0xc0)!=0x80) return -1;
        point=((first&0x07)<<18)|((s[1]&0x3f)<<12)|((s[2]&0x3f)<<6)|
            (s[3]&0x3f);
        if (point<0x10000 || point>0x10ffff) return -1;
        *len=4;
        return point;
    }
    return -1;
}
/* Bidi and format controls are rejected outright (section 13.5). */
static int nick_rejected(int point) {
    if (point<0x20) return 1; /* C0 */
    if (point>=0x7f && point<=0x9f) return 1; /* DEL, C1 */
    if (point==0x061c) return 1;
    if (point==0x200e || point==0x200f) return 1;
    if (point>=0x202a && point<=0x202e) return 1;
    if (point>=0x2066 && point<=0x2069) return 1;
    if (point==0xfeff) return 1;
    if (point>=0xfff9 && point<=0xfffb) return 1;
    return 0;
}
static int nick_combining(int point) {
    if (point>=0x0300 && point<=0x036f) return 1;
    if (point>=0x1ab0 && point<=0x1aff) return 1;
    if (point>=0x1dc0 && point<=0x1dff) return 1;
    if (point>=0x20d0 && point<=0x20ff) return 1;
    if (point>=0xfe20 && point<=0xfe2f) return 1;
    return 0;
}
static int nick_wide(int point) {
    if (point>=0x1100 && point<=0x115f) return 1;
    if (point>=0x2e80 && point<=0x303e) return 1;
    if (point>=0x3041 && point<=0x33ff) return 1;
    if (point>=0x3400 && point<=0x4dbf) return 1;
    if (point>=0x4e00 && point<=0x9fff) return 1;
    if (point>=0xf900 && point<=0xfaff) return 1;
    if (point>=0xff00 && point<=0xff60) return 1;
    if (point>=0xffe0 && point<=0xffe6) return 1;
    if (point>=0x20000 && point<=0x3fffd) return 1;
    return 0;
}
static int nick_space(int point) {
    switch (point) {
    case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x20:
        return 1;
    }
    return 0;
}
static void nick_fail(char *err, size_t errlen, const char *what) {
    if (!err || errlen==0) return;
    snprintf(err,errlen,"nick: %s",what);
    err[errlen-1]=0;
}
/* Validates and normalizes into out (outcap must hold NICK_BYTES_MAX+1).
   Returns the normalized byte length, or -1 with err set. */
static int nick_normalize(const char *name, char *out, size_t outcap,
    char *err, size_t errlen) {
    const uint8_t *bytes;
    size_t left, at=0, cols=0;
    int point, pending_space=0;
    size_t len;
    if (!name) {
        nick_fail(err,errlen,"name required");
        return -1;
    }
    bytes=(const uint8_t *)name;
    left=strnlen(name,NICK_BYTES_MAX+1);
    if (left>NICK_BYTES_MAX) {
        nick_fail(err,errlen,"at most 32 UTF-8 bytes");
        return -1;
    }
    while (left>0) {
        point=nick_decode(bytes,left,&len);
        if (point<0) {
            nick_fail(err,errlen,"invalid UTF-8");
            return -1;
        }
        if (nick_rejected(point)) {
            nick_fail(err,errlen,"control and bidi-control characters are rejected");
            return -1;
        }
        if (nick_space(point)) {
            pending_space=1;
        } else {
            size_t width;
            if (pending_space && at>0) {
                if (at+1>outcap-1) {
                    nick_fail(err,errlen,"at most 32 UTF-8 bytes");
                    return -1;
                }
                out[at++]=' ';
                cols+=1;
            }
            pending_space=0;
            if (nick_combining(point)) {
                width=0;
            } else if (nick_wide(point)) {
                width=2;
            } else {
                width=1;
            }
            if (cols+width>NICK_COLS_MAX) {
                nick_fail(err,errlen,"at most 24 display columns");
                return -1;
            }
            if (at+len>outcap-1) {
                nick_fail(err,errlen,"at most 32 UTF-8 bytes");
                return -1;
            }
            memcpy(out+at,bytes,len);
            at+=len;
            cols+=width;
        }
        bytes+=len;
        left-=len;
    }
    if (at==0) {
        nick_fail(err,errlen,"name required");
        return -1;
    }
    out[at]=0;
    return (int)at;
}
int collab_nick_check(const char *name, char *err, size_t errlen) {
    char normalized[NICK_BYTES_MAX+1];
    return nick_normalize(name,normalized,sizeof(normalized),err,errlen)>=0;
}
/* --- capability gates (section 13.2) --- */
static int collab_test_allow_remote=0;
/* Consent lookup lives below; declared here for the gate. */
static int collab_consent_has(CollabCap cap, const char *room,
    const char *target);
void collab_capability_set_test(int allow_remote) {
    collab_test_allow_remote=allow_remote?1:0;
}
int collab_capability_allowed(uint16_t slot, CollabCap cap,
    const char *target) {
    if (slot==0) return 1;
    switch (cap) {
    case COLLAB_CAP_OSC:
    case COLLAB_CAP_MIDI_OUT:
        break;
    default:
        return 0;
    }
    if (collab_test_allow_remote) return 1;
    /* Consent is room-scoped with exact targets; a missing target or room
       never matches. Call sites must pass the real target. */
    if (!target || !target[0]) return 0;
    return collab_consent_has(cap,collab_room_tag(),target);
}
/* --- generated default (section 18.3): adjective-noun, one CSPRNG byte
   per word masked to 7 bits over embedded 128-entry lists. --- */
static const char *const nick_first[128]={
    "amber","azure","birch","brass","bronze","cedar","chalk","copper",
    "coral","cream","crimson","dusk","ember","fern","flint","frost",
    "garnet","ginger","glacier","granite","hazel","honey","indigo","iron",
    "ivory","jade","juniper","kelp","linen","lunar","maple","marble",
    "mossy","misty","nickel","ochre","olive","onyx","opal","pearl",
    "pepper","pine","plum","quartz","raven","reed","rosy","rusty",
    "sage","sandy","sepia","silky","slate","smoky","solar","stone",
    "sunny","tawny","teal","timber","topaz","umber","velvet","wheat",
    "willow","ashen","autumn","brisk","calm","cinder","clear",
    "cosy","dapple","drowsy","eager","fable","fleet","gentle","glimmer",
    "hollow","humming","idle","jolly","kindly","lithe","mellow","nimble",
    "noble","patient","quiet","rapid","restful","rough","rowdy","rural",
    "shady","silent","silver","sleek","slow","small","smooth","snappy",
    "soft","spare","sparkling","spry","still","sturdy","subtle","supple",
    "swift","tender","tidal","tranquil","vivid","warm","wary","wispy",
    "young",
    "frosty","golden","hazy","humble","icy","jovial","lowly","lucky"
};
static const char *const nick_second[128]={
    "otter","owl","fox","wren","mole","vole","heron","newt",
    "badger","beaver","bison","boar","caplin","chough","civet","conch",
    "cricket","dace","dingo","dove","egret","eland","finch","gannet",
    "gecko","grouse","gull","hare","ibex","ibis","jackal","kestrel",
    "koala","lark","lemur","limpet","loon","lynx","magpie","marmot",
    "marten","merlin","mink","mosquito","mussel","ocelot","okapi","osprey",
    "oyster","panda","parrot","perch","pipit","plover","puffin","quail",
    "quokka","robin","rook","sable","salmon","sculpin","shrew","shrike",
    "skink","smelt","snipe","sparrow","stoat","stork","swallow","swiftlet",
    "teal","tench","thrush","tit","toad","torrent","trout","turbot",
    "vicuna","viper","waxwing","weasel","whelk","whimbrel","widgeon","wombat",
    "drum","reed","stone","brook","canyon","comet","dune","fjord",
    "grove","harbor","islet","lagoon","lantern","meadow","meteor",
    "orchard","prairie","quarry","ridge","spruce","summit","tarn","thicket",
    "tundra","valley","alder","aster","clover","dahlia","heather","holly",
    "iris","lichen","lotus","nettle","orchid","petal","poppy","sedge",
    "sorrel",
};
static int nick_random_bytes(uint8_t *out, size_t len) {
#if defined(__linux__) || defined(__OpenBSD__)
    if (getentropy(out,len)==0) return 1;
#endif
    {
        FILE *random=fopen("/dev/urandom","rb");
        size_t got;
        if (!random) return 0;
        got=fread(out,1,len,random);
        fclose(random);
        return got==len;
    }
}
/* --- collab.json (section 18.3) --- */
static int collab_json_path(char *buf, size_t cap);
static int collab_config_dir(char *buf, size_t cap);
static char *collab_read_file(const char *path, size_t *outlen);
/* Atomic save of a collab.json root (0600, tmp+rename). Consumes root. */
static int collab_json_write(cJSON *root) {
    char path[1100], dir[1024], tmp[1120];
    char *printed=NULL;
    size_t length;
    int fd, ok=0;
    FILE *file=NULL;
    if (!root) return 0;
    if (!collab_json_path(path,sizeof(path))) {
        cJSON_Delete(root);
        return 0;
    }
    if (!collab_config_dir(dir,sizeof(dir))) {
        cJSON_Delete(root);
        return 0;
    }
    mkdir(dir,0700);
    printed=cJSON_Print(root);
    cJSON_Delete(root);
    if (!printed) return 0;
    length=strlen(printed);
    if (snprintf(tmp,sizeof(tmp),"%s/.collab.json.XXXXXX",dir)>=(int)sizeof(tmp)) {
        free(printed);
        return 0;
    }
    fd=mkstemp(tmp);
    if (fd<0) {
        free(printed);
        return 0;
    }
    if (fchmod(fd,0600)!=0) {
        close(fd);
        unlink(tmp);
        free(printed);
        return 0;
    }
    file=fdopen(fd,"w");
    if (!file) {
        close(fd);
        unlink(tmp);
        free(printed);
        return 0;
    }
    if (fwrite(printed,1,length,file)!=length || fputc('\n',file)==EOF) {
        fclose(file);
        unlink(tmp);
        free(printed);
        return 0;
    }
    if (fclose(file)!=0) {
        unlink(tmp);
        free(printed);
        return 0;
    }
    if (rename(tmp,path)!=0) {
        unlink(tmp);
        free(printed);
        return 0;
    }
    free(printed);
    ok=1;
    return ok;
}
/* Parsed collab.json root, or a fresh object when missing/unreadable. */
static cJSON *collab_json_load(void) {
    char path[1100];
    char *text=NULL;
    cJSON *root=NULL;
    if (!collab_json_path(path,sizeof(path))) return NULL;
    text=collab_read_file(path,NULL);
    if (text) {
        root=cJSON_Parse(text);
        free(text);
        if (root) return root;
    }
    return cJSON_CreateObject();
}
static int collab_config_dir(char *buf, size_t cap) {
    const char *base=getenv("XDG_CONFIG_HOME");
    if (base && base[0]=='/') {
        if (snprintf(buf,cap,"%s/tompouce",base)>=(int)cap) return 0;
        return 1;
    }
    base=getenv("HOME");
    if (!base || !base[0]) return 0;
    if (snprintf(buf,cap,"%s/.config/tompouce",base)>=(int)cap) return 0;
    return 1;
}
static int collab_json_path(char *buf, size_t cap) {
    char dir[1024];
    if (!collab_config_dir(dir,sizeof(dir))) return 0;
    if (snprintf(buf,cap,"%s/collab.json",dir)>=(int)cap) return 0;
    return 1;
}
static char *collab_read_file(const char *path, size_t *outlen) {
    FILE *file=fopen(path,"rb");
    long length;
    char *text;
    size_t got;
    if (!file) return NULL;
    if (fseek(file,0,SEEK_END)!=0) {
        fclose(file);
        return NULL;
    }
    length=ftell(file);
    if (length<0 || length>(long)COLLAB_JSON_BYTES_MAX) {
        fclose(file);
        return NULL;
    }
    rewind(file);
    text=malloc((size_t)length+1);
    if (!text) {
        fclose(file);
        return NULL;
    }
    got=fread(text,1,(size_t)length,file);
    fclose(file);
    if (got!=(size_t)length) {
        free(text);
        return NULL;
    }
    text[length]=0;
    if (outlen) *outlen=(size_t)length;
    return text;
}
static int collab_load_stored_nick(char *buf, size_t cap) {
    char path[1100], normalized[NICK_BYTES_MAX+1], err[128];
    char *text=NULL;
    cJSON *root=NULL, *item=NULL;
    int ok=0;
    if (!collab_json_path(path,sizeof(path))) return 0;
    text=collab_read_file(path,NULL);
    if (!text) return 0;
    root=cJSON_Parse(text);
    free(text);
    if (!root) return 0;
    item=cJSON_GetObjectItemCaseSensitive(root,"nick");
    if (cJSON_IsString(item) && item->valuestring &&
        nick_normalize(item->valuestring,normalized,sizeof(normalized),
            err,sizeof(err))>=0) {
        size_t n=strlen(normalized);
        if (n<cap) {
            memcpy(buf,normalized,n+1);
            ok=1;
        }
    }
    cJSON_Delete(root);
    return ok;
}
static int collab_store_nick(const char *nick) {
    cJSON *root=NULL;
    if (!nick) return 0;
    root=collab_json_load();
    if (!root) return 0;
    cJSON_DeleteItemFromObjectCaseSensitive(root,"nick");
    if (!cJSON_AddStringToObject(root,"nick",nick)) {
        cJSON_Delete(root);
        return 0;
    }
    return collab_json_write(root);
}
static void collab_set_nick(const char *nick) {
    size_t n=strlen(nick);
    if (n>=sizeof(collab_nick)) n=sizeof(collab_nick)-1;
    memcpy(collab_nick,nick,n);
    collab_nick[n]=0;
}
static int collab_ensure_nick(const char *override, char *err, size_t errlen) {
    char normalized[NICK_BYTES_MAX+1];
    uint8_t draw[2];
    /* --collab-nick wins for the run and is never written back. */
    if (override && override[0]) {
        if (nick_normalize(override,normalized,sizeof(normalized),
                err,errlen)<0) return 0;
        collab_set_nick(normalized);
        return 1;
    }
    if (collab_load_stored_nick(normalized,sizeof(normalized))) {
        collab_set_nick(normalized);
        return 1;
    }
    /* Generated default, saved before connecting so it stays stable. */
    if (!nick_random_bytes(draw,sizeof(draw))) {
        if (err && errlen>0) {
            snprintf(err,errlen,"nick: no randomness available");
            err[errlen-1]=0;
        }
        return 0;
    }
    if (snprintf(normalized,sizeof(normalized),"%s-%s",
            nick_first[draw[0]&0x7f],
            nick_second[draw[1]&0x7f])>=(int)sizeof(normalized)) {
        if (err && errlen>0) {
            snprintf(err,errlen,"nick: generated name too long");
            err[errlen-1]=0;
        }
        return 0;
    }
    collab_set_nick(normalized);
    if (!collab_store_nick(normalized) && err && errlen>0) {
        /* A failed write must never block the join: keep the session
           name and say so once. */
        snprintf(err,errlen,"nick: kept %s for this session (save failed)",
            normalized);
        err[errlen-1]=0;
    }
    return 1;
}
int collab_nick_get(char *buf, size_t cap) {
    size_t n;
    if (!buf || cap==0) return 0;
    n=strlen(collab_nick);
    if (n>=cap) return 0;
    memcpy(buf,collab_nick,n+1);
    return 1;
}
int collab_nick_set(const char *name, char *err, size_t errlen) {
    char normalized[NICK_BYTES_MAX+1];
    if (nick_normalize(name,normalized,sizeof(normalized),err,errlen)<0)
        return 0;
    collab_set_nick(normalized);
    if (!collab_store_nick(normalized)) {
        if (err && errlen>0) {
            snprintf(err,errlen,"nick: kept %s for this session (save failed)",
                normalized);
            err[errlen-1]=0;
        }
    }
    return 1;
}
/* --- session (section 10.1, Phase 0 subset) --- */
void collab_disconnect(void) {
    ctr_close(collab_transport);
    collab_transport=NULL;
    collab_url[0]=0;
    collab_room_reset();
    collab_current_state=COLLAB_IDLE;
}
static int collab_hello(CollabTransport *ct, char *err, size_t errlen) {
    cJSON *hello=cJSON_CreateObject();
    cJSON *client=NULL, *caps=NULL, *proto=NULL;
    char *text=NULL;
    int ok=0, is_binary=0;
    uint8_t *reply=NULL;
    size_t replylen=0;
    cJSON *welcome=NULL, *type=NULL;
    if (!hello) {
        if (err && errlen>0) snprintf(err,errlen,"collab: out of memory");
        return 0;
    }
    client=cJSON_AddObjectToObject(hello,"client");
    caps=cJSON_CreateArray();
    proto=cJSON_CreateArray();
    if (!client || !caps || !proto) goto done;
    cJSON_AddStringToObject(hello,"t","hello");
    cJSON_AddItemToArray(proto,cJSON_CreateNumber(1));
    cJSON_AddItemToObject(hello,"proto",proto);
    proto=NULL;
    cJSON_AddStringToObject(client,"name","tompouce");
    cJSON_AddStringToObject(client,"version",COLLAB_CLIENT_VERSION);
    cJSON_AddItemToArray(caps,cJSON_CreateString("e2e"));
    cJSON_AddItemToObject(hello,"caps",caps);
    caps=NULL;
    text=cJSON_PrintUnformatted(hello);
    if (!text) goto done;
    if (!ctr_send_text(ct,text,0)) {
        if (err && errlen>0) snprintf(err,errlen,"collab: hello send failed");
        goto done;
    }
    if (!ctr_recv(ct,&is_binary,&reply,&replylen,10000) || is_binary ||
        !reply) {
        if (err && errlen>0)
            snprintf(err,errlen,"collab: no welcome from director");
        goto done;
    }
    welcome=cJSON_ParseWithLength((const char *)reply,replylen);
    if (!welcome) {
        if (err && errlen>0)
            snprintf(err,errlen,"collab: bad welcome frame");
        goto done;
    }
    type=cJSON_GetObjectItemCaseSensitive(welcome,"t");
    if (!cJSON_IsString(type) || strcmp(type->valuestring,"welcome")!=0) {
        if (err && errlen>0)
            snprintf(err,errlen,"collab: expected welcome");
        goto done;
    }
    ok=1;
done:
    cJSON_Delete(welcome);
    free(reply);
    free(text);
    cJSON_Delete(hello);
    return ok;
}
int collab_connect(const CollabConfig *cfg, char *err, size_t errlen) {
    CollabTransport *ct;
    char fail[256]="";
    if (!cfg || !cfg->url[0]) {
        if (err && errlen>0) snprintf(err,errlen,"collab: no url");
        return 0;
    }
    collab_disconnect();
    collab_current_state=COLLAB_CONNECTING;
    if (!collab_ensure_nick(cfg->nick_override[0]?cfg->nick_override:NULL,
            fail,sizeof(fail))) {
        collab_current_state=COLLAB_FAILED;
        if (err && errlen>0) {
            snprintf(err,errlen,"%s",fail);
            err[errlen-1]=0;
        }
        return 0;
    }
    ct=ctr_open(cfg->url,CP_SUBPROTOCOL,fail,sizeof(fail));
    if (!ct) {
        collab_current_state=COLLAB_FAILED;
        if (err && errlen>0) {
            snprintf(err,errlen,"%s",fail);
            err[errlen-1]=0;
        }
        return 0;
    }
    if (!collab_hello(ct,fail,sizeof(fail))) {
        collab_current_state=COLLAB_FAILED;
        ctr_close(ct);
        if (err && errlen>0) {
            snprintf(err,errlen,"%s",fail);
            err[errlen-1]=0;
        }
        return 0;
    }
    collab_transport=ct;
    snprintf(collab_url,sizeof(collab_url),"%s",cfg->url);
    collab_current_state=COLLAB_CONNECTED;
    /* A stored-generated nick that failed to save warned inside
       collab_ensure_nick; surface it once here without failing. */
    if (fail[0] && err && errlen>0) {
        snprintf(err,errlen,"%s",fail);
        err[errlen-1]=0;
    }
    return 1;
}
/* --- consent allowlist (sections 13.2, 18.3) --- */
/* Per capability, room-scoped exact-target allows. Bounded static tables:
   consent is checked per event on the generator, so no allocation there. */
#define COLLAB_ALLOW_ROOMS_MAX 16
#define COLLAB_ALLOW_TARGETS_MAX 16
#define COLLAB_ALLOW_ROOM_MAX 128
#define COLLAB_ALLOW_TARGET_MAX 256
typedef struct {
    char room[COLLAB_ALLOW_ROOM_MAX+1];
    char targets[COLLAB_ALLOW_TARGETS_MAX][COLLAB_ALLOW_TARGET_MAX+1];
    int ntargets;
} CollabAllowRoom;
static CollabAllowRoom collab_allow_osc[COLLAB_ALLOW_ROOMS_MAX];
static CollabAllowRoom collab_allow_midi[COLLAB_ALLOW_ROOMS_MAX];
static int collab_consent_loaded=0;
static const char *collab_cap_key(CollabCap cap) {
    switch (cap) {
    case COLLAB_CAP_OSC: return "osc";
    case COLLAB_CAP_MIDI_OUT: return "midi_out";
    }
    return NULL;
}
static CollabAllowRoom *collab_allow_table(CollabCap cap) {
    switch (cap) {
    case COLLAB_CAP_OSC: return collab_allow_osc;
    case COLLAB_CAP_MIDI_OUT: return collab_allow_midi;
    }
    return NULL;
}
static int collab_allow_valid(const char *room, const char *target) {
    size_t rl, tl;
    if (!room || !target) return 0;
    rl=strlen(room);
    tl=strlen(target);
    return rl>0 && rl<=COLLAB_ALLOW_ROOM_MAX && tl>0 &&
        tl<=COLLAB_ALLOW_TARGET_MAX;
}
static int collab_allow_room_index(CollabAllowRoom *table, const char *room) {
    int i, free_slot=-1;
    for (i=0;i<COLLAB_ALLOW_ROOMS_MAX;i++) {
        if (!table[i].room[0]) {
            if (free_slot<0) free_slot=i;
            continue;
        }
        if (!strcmp(table[i].room,room)) return i;
    }
    return free_slot;
}
static int collab_consent_has(CollabCap cap, const char *room,
    const char *target) {
    CollabAllowRoom *table=collab_allow_table(cap);
    int i, j;
    if (!table || !collab_allow_valid(room,target)) return 0;
    for (i=0;i<COLLAB_ALLOW_ROOMS_MAX;i++) {
        if (strcmp(table[i].room,room)) continue;
        for (j=0;j<table[i].ntargets;j++) {
            if (!strcmp(table[i].targets[j],target)) return 1;
        }
    }
    return 0;
}
/* Push the in-memory tables into root under "allow". */
static int collab_consent_save_json(cJSON *root) {
    cJSON *allow=NULL, *capobj=NULL, *roomobj=NULL;
    int c, i, j;
    static const CollabCap caps[]={COLLAB_CAP_OSC,COLLAB_CAP_MIDI_OUT};
    if (!root) return 0;
    cJSON_DeleteItemFromObjectCaseSensitive(root,"allow");
    allow=cJSON_CreateObject();
    if (!allow) return 0;
    for (c=0;c<2;c++) {
        CollabAllowRoom *table=collab_allow_table(caps[c]);
        capobj=cJSON_CreateObject();
        if (!capobj) {
            cJSON_Delete(allow);
            return 0;
        }
        for (i=0;i<COLLAB_ALLOW_ROOMS_MAX;i++) {
            cJSON *list;
            if (!table[i].room[0]) continue;
            list=cJSON_CreateArray();
            if (!list) {
                cJSON_Delete(allow);
                return 0;
            }
            for (j=0;j<table[i].ntargets;j++) {
                if (!cJSON_AddItemToArray(list,
                        cJSON_CreateString(table[i].targets[j]))) {
                    cJSON_Delete(list);
                    cJSON_Delete(allow);
                    return 0;
                }
            }
            roomobj=list;
            if (!cJSON_AddItemToObject(capobj,table[i].room,roomobj)) {
                cJSON_Delete(roomobj);
                cJSON_Delete(allow);
                return 0;
            }
        }
        if (!cJSON_AddItemToObject(allow,collab_cap_key(caps[c]),capobj)) {
            cJSON_Delete(capobj);
            cJSON_Delete(allow);
            return 0;
        }
    }
    if (!cJSON_AddItemToObject(root,"allow",allow)) {
        cJSON_Delete(allow);
        return 0;
    }
    return 1;
}
static void collab_consent_clear(void) {
    memset(collab_allow_osc,0,sizeof(collab_allow_osc));
    memset(collab_allow_midi,0,sizeof(collab_allow_midi));
}
/* Rebuild the in-memory tables from collab.json. Malformed entries are
   skipped; bounds from the static tables apply. */
static void collab_consent_load(void) {
    char path[1100];
    char *text=NULL;
    cJSON *root=NULL, *allow=NULL;
    int c;
    static const CollabCap caps[]={COLLAB_CAP_OSC,COLLAB_CAP_MIDI_OUT};
    collab_consent_clear();
    collab_consent_loaded=1;
    if (!collab_json_path(path,sizeof(path))) return;
    text=collab_read_file(path,NULL);
    if (!text) return;
    root=cJSON_Parse(text);
    free(text);
    if (!root) return;
    allow=cJSON_GetObjectItemCaseSensitive(root,"allow");
    if (!cJSON_IsObject(allow)) {
        cJSON_Delete(root);
        return;
    }
    for (c=0;c<2;c++) {
        CollabAllowRoom *table=collab_allow_table(caps[c]);
        const cJSON *capobj=cJSON_GetObjectItemCaseSensitive(allow,
            collab_cap_key(caps[c]));
        const cJSON *roomobj=NULL;
        if (!cJSON_IsObject(capobj)) continue;
        cJSON_ArrayForEach(roomobj,capobj) {
            const cJSON *target=NULL;
            int ri;
            if (!roomobj->string || !roomobj->string[0] ||
                strlen(roomobj->string)>COLLAB_ALLOW_ROOM_MAX) continue;
            if (!cJSON_IsArray(roomobj)) continue;
            ri=collab_allow_room_index(table,roomobj->string);
            if (ri<0 || table[ri].room[0]) continue;
            snprintf(table[ri].room,sizeof(table[ri].room),"%s",
                roomobj->string);
            cJSON_ArrayForEach(target,roomobj) {
                if (!cJSON_IsString(target) || !target->valuestring ||
                    !target->valuestring[0] ||
                    strlen(target->valuestring)>COLLAB_ALLOW_TARGET_MAX)
                    continue;
                if (table[ri].ntargets>=COLLAB_ALLOW_TARGETS_MAX) break;
                snprintf(table[ri].targets[table[ri].ntargets],
                    sizeof(table[ri].targets[table[ri].ntargets]),"%s",
                    target->valuestring);
                table[ri].ntargets++;
            }
            if (!table[ri].ntargets) table[ri].room[0]=0;
        }
    }
    cJSON_Delete(root);
}
void collab_consent_reload(void) {
    collab_consent_load();
}
int collab_allow(CollabCap cap, const char *room, const char *target) {
    CollabAllowRoom *table=collab_allow_table(cap);
    cJSON *root=NULL;
    int ri, i;
    if (!table || !collab_allow_valid(room,target)) return 0;
    if (!collab_consent_loaded) collab_consent_load();
    ri=collab_allow_room_index(table,room);
    if (ri<0) return 0;
    if (!table[ri].room[0]) {
        snprintf(table[ri].room,sizeof(table[ri].room),"%s",room);
        table[ri].ntargets=0;
    }
    for (i=0;i<table[ri].ntargets;i++) {
        if (!strcmp(table[ri].targets[i],target)) return 1;
    }
    if (table[ri].ntargets>=COLLAB_ALLOW_TARGETS_MAX) return 0;
    snprintf(table[ri].targets[table[ri].ntargets],
        sizeof(table[ri].targets[table[ri].ntargets]),"%s",target);
    table[ri].ntargets++;
    /* Fail closed: roll back when the file cannot be written. */
    root=collab_json_load();
    if (!root || !collab_consent_save_json(root) ||
        !collab_json_write(root)) {
        table[ri].ntargets--;
        if (!table[ri].ntargets) table[ri].room[0]=0;
        return 0;
    }
    return 1;
}
int collab_deny(CollabCap cap, const char *room, const char *target) {
    CollabAllowRoom *table=collab_allow_table(cap);
    cJSON *root=NULL;
    int ri, i, found=-1;
    char saved[COLLAB_ALLOW_TARGET_MAX+1];
    if (!table || !collab_allow_valid(room,target)) return 0;
    if (!collab_consent_loaded) collab_consent_load();
    for (ri=0;ri<COLLAB_ALLOW_ROOMS_MAX;ri++) {
        if (strcmp(table[ri].room,room)) continue;
        for (i=0;i<table[ri].ntargets;i++) {
            if (!strcmp(table[ri].targets[i],target)) {
                found=i;
                break;
            }
        }
        break;
    }
    if (ri>=COLLAB_ALLOW_ROOMS_MAX || found<0) return 1;
    snprintf(saved,sizeof(saved),"%s",table[ri].targets[found]);
    table[ri].ntargets--;
    memmove(table[ri].targets[found],table[ri].targets[found+1],
        (size_t)(table[ri].ntargets-found)*sizeof(table[ri].targets[0]));
    if (!table[ri].ntargets) table[ri].room[0]=0;
    root=collab_json_load();
    if (!root || !collab_consent_save_json(root) ||
        !collab_json_write(root)) {
        /* Roll back: restore the room and target. */
        int r2=collab_allow_room_index(table,room);
        if (r2<0) return 0;
        if (!table[r2].room[0]) {
            snprintf(table[r2].room,sizeof(table[r2].room),"%s",room);
            table[r2].ntargets=0;
        }
        if (table[r2].ntargets<COLLAB_ALLOW_TARGETS_MAX) {
            snprintf(table[r2].targets[table[r2].ntargets],
                sizeof(table[r2].targets[table[r2].ntargets]),"%s",saved);
            table[r2].ntargets++;
        }
        return 0;
    }
    return 1;
}
const char *collab_room_tag(void) {
    return collab_url;
}
int collab_slot_from_id(const char *id) {
    long slot;
    char *end;
    if (!id || !*id || !isdigit((unsigned char)*id)) return 0;
    slot=strtol(id,&end,10);
    if (end==id || slot<0 || slot>1023) return 0;
    if (*end && *end!='/') return 0;
    return (int)slot;
}
/* --- room composite (sections 9.4, 10.3) --- */
#define COLLAB_ROOM_SLOTS_MAX 32
#define COLLAB_ROOM_BODY_MAX (256*1024)
typedef struct {
    int slot;
    int remote;
    char *body;      /* last evaluated body */
    char *last_good; /* last body that compiled clean */
    char error[256];
    int ok;
} CollabRoomSlot;
static CollabRoomSlot collab_room[COLLAB_ROOM_SLOTS_MAX];
static int collab_room_n=0;
void collab_room_reset(void) {
    int i;
    for (i=0;i<collab_room_n;i++) {
        free(collab_room[i].body);
        free(collab_room[i].last_good);
    }
    memset(collab_room,0,sizeof(collab_room));
    collab_room_n=0;
}
static int collab_room_find(int slot) {
    int i;
    for (i=0;i<collab_room_n;i++) {
        if (collab_room[i].slot==slot) return i;
    }
    return -1;
}
int collab_room_set_slot(int slot, const char *body, size_t bodylen,
    int remote) {
    size_t length;
    char *copy=NULL;
    int at, i;
    if (slot<0 || slot>1023 || !body) return 0;
    length=strlen(body);
    if (length!=bodylen || length>COLLAB_ROOM_BODY_MAX) return 0;
    copy=malloc(length+1);
    if (!copy) return 0;
    memcpy(copy,body,length+1);
    at=collab_room_find(slot);
    if (at>=0) {
        free(collab_room[at].body);
        collab_room[at].body=copy;
        collab_room[at].remote=remote?1:0;
        collab_room[at].ok=0;
        collab_room[at].error[0]=0;
        return 1;
    }
    if (collab_room_n>=COLLAB_ROOM_SLOTS_MAX) {
        free(copy);
        return 0;
    }
    for (at=0;at<collab_room_n;at++) {
        if (collab_room[at].slot>slot) break;
    }
    for (i=collab_room_n;i>at;i--) collab_room[i]=collab_room[i-1];
    collab_room[at].slot=slot;
    collab_room[at].remote=remote?1:0;
    collab_room[at].body=copy;
    collab_room[at].last_good=NULL;
    collab_room[at].error[0]=0;
    collab_room[at].ok=0;
    collab_room_n++;
    return 1;
}
const char *collab_room_slot_error(int slot) {
    int at=collab_room_find(slot);
    if (at<0 || collab_room[at].ok) return NULL;
    return collab_room[at].error;
}
static void collab_room_fail(char *err, size_t errlen, const char *what) {
    if (err && errlen>0) {
        snprintf(err,errlen,"%s",what);
        err[errlen-1]=0;
    }
}
/* Compile the current bodies; on per-slot failure substitute last-good
   bodies once and recompile. Author-facing ok/error always describe the
   current body; *n_ok counts slots contributing audio (ok or substituted
   clean). Updates last_good for ok slots. */
static JsProgram *collab_room_compile(JsProfile profile, JsProgram *previous,
    cJSON **metadata, char *fail, size_t faillen, int *n_ok) {
    CollabSlotText texts[COLLAB_ROOM_SLOTS_MAX];
    JsProgram *program=NULL;
    cJSON *meta=NULL, *slots=NULL;
    int i, ok=0, substituted_any=0;
    int substituted[COLLAB_ROOM_SLOTS_MAX]={0};
    int audible[COLLAB_ROOM_SLOTS_MAX]={0};
    *n_ok=0;
    if (metadata) *metadata=NULL;
    for (i=0;i<collab_room_n;i++) {
        texts[i].slot=collab_room[i].slot;
        texts[i].text=collab_room[i].body;
    }
    program=javascript_compile_slots(texts,collab_room_n,audio_get_bpm(),
        previous,&profile,&meta,fail,faillen);
    if (!program) return NULL;
    if (previous &&
        !javascript_prepare_csound_from(program,meta,previous,fail,faillen)) {
        javascript_free(program);
        cJSON_Delete(meta);
        return NULL;
    }
    slots=meta?cJSON_GetObjectItemCaseSensitive(meta,"slots"):NULL;
    if (!cJSON_IsArray(slots) ||
        cJSON_GetArraySize(slots)!=collab_room_n) {
        snprintf(fail,faillen,"room compile returned no slot report");
        javascript_free(program);
        cJSON_Delete(meta);
        return NULL;
    }
    /* Substitute last-good bodies for failing slots and compile once more:
       every client derives the same composite from the same input, and one
       member's typo never silences the room. */
    for (i=0;i<collab_room_n;i++) {
        const cJSON *entry=cJSON_GetArrayItem(slots,i);
        const cJSON *entry_ok=entry?cJSON_GetObjectItemCaseSensitive(entry,
            "ok"):NULL;
        const cJSON *entry_id=entry?cJSON_GetObjectItemCaseSensitive(entry,
            "id"):NULL;
        int at=cJSON_IsNumber(entry_id)?collab_room_find(entry_id->valueint):
            -1;
        const cJSON *entry_err=entry?cJSON_GetObjectItemCaseSensitive(entry,
            "error"):NULL;
        if (at<0) continue;
        collab_room[at].ok=cJSON_IsTrue(entry_ok);
        if (cJSON_IsString(entry_err) && entry_err->valuestring) {
            snprintf(collab_room[at].error,sizeof(collab_room[at].error),
                "%s",entry_err->valuestring);
        } else collab_room[at].error[0]=0;
        audible[at]=collab_room[at].ok;
        if (!collab_room[at].ok && collab_room[at].last_good) {
            texts[i].text=collab_room[at].last_good;
            substituted[i]=1;
            substituted_any=1;
        }
    }
    if (substituted_any) {
        JsProgram *retry=NULL;
        cJSON *retry_meta=NULL;
        char retry_fail[256]="";
        retry=javascript_compile_slots(texts,collab_room_n,
            audio_get_bpm(),previous,&profile,&retry_meta,retry_fail,
            sizeof(retry_fail));
        if (retry) {
            if (previous && !javascript_prepare_csound_from(retry,
                    retry_meta,previous,retry_fail,sizeof(retry_fail))) {
                javascript_free(retry);
                cJSON_Delete(retry_meta);
            } else {
                javascript_free(program);
                cJSON_Delete(meta);
                program=retry;
                meta=retry_meta;
                slots=cJSON_GetObjectItemCaseSensitive(meta,"slots");
                for (i=0;i<collab_room_n && cJSON_IsArray(slots);i++) {
                    const cJSON *entry=cJSON_GetArrayItem(slots,i);
                    const cJSON *entry_ok=entry?
                        cJSON_GetObjectItemCaseSensitive(entry,"ok"):NULL;
                    const cJSON *entry_id=entry?
                        cJSON_GetObjectItemCaseSensitive(entry,"id"):NULL;
                    int at=cJSON_IsNumber(entry_id)?collab_room_find(
                        entry_id->valueint):-1;
                    const cJSON *entry_err=entry?
                        cJSON_GetObjectItemCaseSensitive(entry,"error"):NULL;
                    if (at<0) continue;
                    if (substituted[i]) {
                        /* The current body stays failed for the author;
                           the room plays the substituted last-good. */
                        if (cJSON_IsTrue(entry_ok)) audible[at]=1;
                        continue;
                    }
                    collab_room[at].ok=cJSON_IsTrue(entry_ok);
                    if (cJSON_IsString(entry_err) && entry_err->valuestring) {
                        snprintf(collab_room[at].error,
                            sizeof(collab_room[at].error),"%s",
                            entry_err->valuestring);
                    } else collab_room[at].error[0]=0;
                    audible[at]=collab_room[at].ok;
                }
            }
        }
    }
    for (i=0;i<collab_room_n;i++) {
        if (collab_room[i].ok) {
            free(collab_room[i].last_good);
            collab_room[i].last_good=NULL;
            if (collab_room[i].body) {
                collab_room[i].last_good=strdup(collab_room[i].body);
                if (!collab_room[i].last_good) {
                    collab_room[i].ok=0;
                    audible[i]=0;
                    snprintf(collab_room[i].error,
                        sizeof(collab_room[i].error),"out of memory");
                    continue;
                }
            }
            audible[i]=1;
        }
        if (audible[i]) ok++;
    }
    *n_ok=ok;
    if (metadata) *metadata=meta;
    else cJSON_Delete(meta);
    return program;
}
int collab_room_apply(double at_cycle, unsigned eval_seq, char *err,
    size_t errlen) {
    JsProgram *program=NULL, *previous=NULL;
    cJSON *metadata=NULL;
    char fail[256]="";
    JsProfile profile;
    int i, any_remote=0, n_ok=0;
    if (!isfinite(at_cycle) || at_cycle<0) {
        collab_room_fail(err,errlen,"room eval needs a finite at_cycle");
        return 0;
    }
    if (collab_room_n==0) {
        collab_room_fail(err,errlen,"room has no slots");
        return 0;
    }
    for (i=0;i<collab_room_n;i++) {
        if (collab_room[i].remote) {
            any_remote=1;
            break;
        }
    }
    profile=any_remote?js_profile_remote():js_profile_local();
    pthread_mutex_lock(&tracks_lock);
    if (track_count==1 && tracks[0].javascript) {
        previous=tracks[0].javascript;
        javascript_retain(previous);
    }
    pthread_mutex_unlock(&tracks_lock);
    program=collab_room_compile(profile,previous,&metadata,fail,sizeof(fail),
        &n_ok);
    javascript_free(previous);
    if (!program) {
        /* The previous program keeps playing; never silence the room. */
        collab_room_fail(err,errlen,fail[0]?fail:"room compile failed");
        return 0;
    }
    if (!n_ok) {
        javascript_free(program);
        cJSON_Delete(metadata);
        collab_room_fail(err,errlen,"every room slot failed");
        return 0;
    }
    if (!track_stage_program(program,metadata,at_cycle,eval_seq)) {
        javascript_free(program);
        cJSON_Delete(metadata);
        collab_room_fail(err,errlen,"no live track to stage into");
        return 0;
    }
    return 1;
}
