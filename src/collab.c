#include "collab.h"
#include "collab_crypto.h"
#include "collab_proto.h"
#include "collab_transport.h"
#include "common.h"
#include "javascript.h"
#include "track.h"
#include "audio.h"
#include <cjson/cJSON.h>
#include <ctype.h>
#include <math.h>
#include <sodium.h>
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
static uint8_t collab_challenge[CCR_CHALLENGE_BYTES];
static int collab_have_challenge=0;
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
/* Forward declarations for the M2 flow state below. */
static void collab_session_reset(void);
void collab_disconnect(void) {
    ctr_close(collab_transport);
    collab_transport=NULL;
    collab_url[0]=0;
    collab_have_challenge=0;
    collab_room_reset();
    collab_session_reset();
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
    {
        const cJSON *challenge=cJSON_GetObjectItemCaseSensitive(welcome,
            "challenge");
        size_t outlen=0;
        collab_have_challenge=0;
        if (cJSON_IsString(challenge) && challenge->valuestring &&
            cp_b64_decode(challenge->valuestring,collab_challenge,
                sizeof(collab_challenge),&outlen) &&
            outlen==sizeof(collab_challenge)) {
            collab_have_challenge=1;
        }
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
    if (slot<0 || slot>=CP_SLOTS_MAX || !body) return 0;
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
/* --- room session flow (M2, sections 7.3, 10.3, 14.5, 14.7) --- */
#define COLLAB_MAX_MEMBERS 16
typedef struct {
    uint32_t id;
    uint8_t pk[CCR_PK_BYTES];
    char role[8];
    char nick[64];
    int has_nick;
} CollabMember;
static CollabMember collab_members[COLLAB_MAX_MEMBERS];
static int collab_nmembers=0;
static uint32_t collab_self_member=0;
static int collab_have_self=0;
static uint8_t collab_self_pk[CCR_PK_BYTES];
static uint8_t collab_self_sk[CCR_SK_BYTES];
static int collab_have_keys=0;
static uint8_t collab_roomkey[CCR_KEY_BYTES];
static int collab_have_roomkey=0;
static uint8_t collab_sub_frame[CCR_KEY_BYTES];
static uint8_t collab_sub_ctrl[CCR_KEY_BYTES];
static uint8_t collab_admission[CCR_KEY_BYTES];
static uint16_t collab_key_epoch=0;
static uint8_t collab_opk[CCR_PK_BYTES];
static int collab_have_opk=0;
static char collab_roomid[CCR_ROOM_ID_LEN+1];
static int collab_joined_flag=0;
static int collab_synced_flag=0;
static uint8_t collab_resume[32];
static int collab_have_resume=0;
static uint64_t collab_author_seq[CP_SLOTS_MAX];
static struct {
    uint32_t author;
    uint64_t seq;
    int seen;
} collab_last_seen[CP_SLOTS_MAX];
static uint64_t collab_last_eval_applied=0;
static int collab_solos_off=0;
/* Provisional room→local cycle mapping (M4 replaces it with the clock
   filter). The join anchors the room epoch to the local cursor: later
   director at_cycle values land at local_cursor + (at - room_now), where
   room_now advances at the room tempo from the join instant. Exact when
   room and local tempo agree (the loopback/test case); tempo skew,
   drift and cross-machine delay are M4's job. */
static double collab_epoch_c=0.0;
static double collab_join_audio=0.0;
static double collab_room_cps=0.5;
static void collab_session_reset(void) {
    int i;
    sodium_memzero(collab_self_sk,sizeof(collab_self_sk));
    sodium_memzero(collab_roomkey,sizeof(collab_roomkey));
    sodium_memzero(collab_sub_frame,sizeof(collab_sub_frame));
    sodium_memzero(collab_sub_ctrl,sizeof(collab_sub_ctrl));
    sodium_memzero(collab_admission,sizeof(collab_admission));
    sodium_memzero(collab_resume,sizeof(collab_resume));
    memset(collab_members,0,sizeof(collab_members));
    collab_nmembers=0;
    collab_self_member=0;
    collab_have_self=0;
    collab_have_keys=0;
    collab_have_roomkey=0;
    collab_key_epoch=0;
    collab_have_opk=0;
    collab_roomid[0]=0;
    collab_joined_flag=0;
    collab_synced_flag=0;
    collab_have_resume=0;
    collab_solos_off=0;
    collab_last_eval_applied=0;
    collab_epoch_c=0.0;
    collab_join_audio=0.0;
    collab_room_cps=0.5;
    for (i=0;i<CP_SLOTS_MAX;i++) {
        collab_author_seq[i]=0;
        collab_last_seen[i].seen=0;
    }
}
static void collab_fail(char *err, size_t errlen, const char *what) {
    if (err && errlen>0) {
        snprintf(err,errlen,"%s",what);
        err[errlen-1]=0;
    }
}
/* Receive one frame; text goes to *text_out (caller frees), binary to
   *bin_out (caller frees). Exactly one of them is set. Returns 1 on a
   frame, 0 on timeout, -1 on close/error. */
static int collab_recv_frame(char **text_out, uint8_t **bin_out,
    size_t *binlen_out, int timeout_ms) {
    int is_binary=0;
    uint8_t *payload=NULL;
    size_t length=0;
    *text_out=NULL;
    *bin_out=NULL;
    if (binlen_out) *binlen_out=0;
    if (!collab_transport) return -1;
    if (!ctr_recv(collab_transport,&is_binary,&payload,&length,timeout_ms) ||
        !payload) {
        free(payload);
        return 0;
    }
    if (is_binary) {
        *bin_out=payload;
        if (binlen_out) *binlen_out=length;
        return 1;
    }
    *text_out=malloc(length+1);
    if (!*text_out) {
        free(payload);
        return -1;
    }
    memcpy(*text_out,payload,length);
    (*text_out)[length]=0;
    free(payload);
    return 1;
}
static int collab_send_json(cJSON *obj) {
    char *text;
    int ok;
    if (!obj || !collab_transport) return 0;
    text=cJSON_PrintUnformatted(obj);
    if (!text) return 0;
    ok=ctr_send_text(collab_transport,text,strlen(text));
    free(text);
    return ok;
}
/* malloc'd base64url of in (no padding). */
static char *collab_b64enc(const uint8_t *in, size_t len) {
    size_t cap=4*((len+2)/3)+1;
    char *out=malloc(cap);
    if (!out) return NULL;
    out[cp_b64_encode(in,len,out,cap)]=0;
    return out;
}
static int collab_member_find(uint32_t id) {
    int i;
    for (i=0;i<collab_nmembers;i++) {
        if (collab_members[i].id==id) return i;
    }
    return -1;
}
/* Verify a sealed type-2 profile against the member key and store the
   nick (validated) or drop it. Forged renames never render. */
static void collab_member_profile(int index, const char *sealed_b64) {
    CollabMember *member;
    uint8_t *enc=NULL, *plain=NULL;
    size_t enclen=0, plainlen=0, b64len;
    char nick[64], color[64];
    if (index<0 || index>=collab_nmembers || !sealed_b64) return;
    member=&collab_members[index];
    member->has_nick=0;
    member->nick[0]=0;
    b64len=strlen(sealed_b64);
    if (!b64len || b64len>2*CP_COMPOSITE_MAX) return;
    enc=malloc(b64len*3/4+4);
    if (!enc) return;
    if (!cp_b64_decode(sealed_b64,enc,b64len*3/4+4,&enclen)) {
        free(enc);
        return;
    }
    if (ccr_open_ctrl_typed(collab_sub_ctrl,collab_roomid,collab_key_epoch,
            CCR_CTRL_PROFILE,0,member->pk,enc,enclen,&plain,&plainlen,
            NULL,NULL,NULL)!=1) {
        free(enc);
        free(plain);
        return;
    }
    free(enc);
    if (ccr_profile_parse((const char *)plain,plainlen,nick,sizeof(nick),
            color,sizeof(color)) &&
        collab_nick_check(nick,NULL,0)) {
        snprintf(member->nick,sizeof(member->nick),"%s",nick);
        member->has_nick=1;
    }
    sodium_memzero(plain,plainlen);
    free(plain);
}
static void collab_member_add(uint32_t id, const uint8_t *pk,
    const char *role, const char *profile_b64) {
    CollabMember *member;
    int at=collab_member_find(id);
    if (at<0) {
        if (collab_nmembers>=COLLAB_MAX_MEMBERS) return;
        at=collab_nmembers++;
    }
    member=&collab_members[at];
    member->id=id;
    memcpy(member->pk,pk,CCR_PK_BYTES);
    snprintf(member->role,sizeof(member->role),"%s",role?role:"editor");
    member->has_nick=0;
    member->nick[0]=0;
    if (profile_b64) collab_member_profile(at,profile_b64);
}
/* Forward declarations for the expect loop. */
static int collab_handle_text(cJSON *obj);
/* Receive until a text frame with t==want (or timeout_ms total).
   Other text frames dispatch normally; binary replay is skipped.
   Returns the parsed owned object (caller frees with cJSON_Delete). */
static cJSON *collab_expect(const char *want, int timeout_ms, char *err,
    size_t errlen) {
    uint64_t deadline=(uint64_t)now_seconds()*1000+(uint64_t)timeout_ms;
    while (collab_transport) {
        char *text=NULL;
        uint8_t *bin=NULL;
        size_t binlen=0;
        cJSON *obj=NULL;
        CpMsg msg;
        int64_t left;
        uint64_t now=(uint64_t)now_seconds()*1000;
        int step;
        if (now>=deadline) {
            collab_fail(err,errlen,"collab: timed out waiting for reply");
            return NULL;
        }
        left=(int64_t)(deadline-now);
        step=left>2000?2000:(int)left;
        if (!collab_recv_frame(&text,&bin,&binlen,step)) {
            free(text);
            free(bin);
            continue;
        }
        if (bin) {
            free(bin);
            continue;
        }
        obj=cJSON_Parse(text);
        free(text);
        if (!obj) continue;
        {
            char *flat=cJSON_PrintUnformatted(obj);
            CpError parse=CP_E_PROTO;
            if (flat) {
                parse=cp_parse_control(flat,strlen(flat),&msg);
                free(flat);
            }
            if (parse!=CP_E_NONE) {
                cJSON_Delete(obj);
                continue;
            }
        }
        cp_msg_free(&msg);
        {
            const cJSON *type=cJSON_GetObjectItemCaseSensitive(obj,"t");
            int is_want=cJSON_IsString(type) && type->valuestring &&
                !strcmp(type->valuestring,want);
            if (is_want) return obj;
            collab_handle_text(obj);
            cJSON_Delete(obj);
        }
    }
    collab_fail(err,errlen,"collab: disconnected while waiting");
    return NULL;
}
/* Map a director at_cycle (room frame) into local pending cycles.
   Past instants land on the current cursor: the staged path activates
   them at the next window without rewinding, like a late local load. */
static double collab_pending_for(double at_cycle) {
    double room_now, local_cursor, pending;
    room_now=collab_epoch_c+(audio_time_now()-collab_join_audio)*
        collab_room_cps;
    pthread_mutex_lock(&tracks_lock);
    local_cursor=(track_count==1 && tracks[0].javascript)?
        tracks[0].js_cursor:room_now;
    pthread_mutex_unlock(&tracks_lock);
    pending=at_cycle<=room_now?local_cursor:local_cursor+
        (at_cycle-room_now);
    return pending;
}
/* --- eval receive path --- */
/* Applies one decrypted eval record: per-(author,slot) seq gate, room
   update, shared-instant staging, author ack. Returns 1 when activated. */
static int collab_apply_eval_text(uint32_t author, int slot,
    uint64_t eval_seq, double at_cycle, const char *body) {
    char fail[256]="";
    uint64_t capped=eval_seq>0xffffffffu?0xffffffffu:(uint64_t)eval_seq;
    if (!collab_room_set_slot(slot,body,strlen(body),
            author!=collab_self_member)) {
        log_console("Jam: eval #%llu from member %u rejected (slot)",
            (unsigned long long)eval_seq,author);
        return 0;
    }
    if (!collab_room_apply(collab_pending_for(at_cycle),(unsigned)capped,
            fail,sizeof(fail))) {
        log_console("Jam: eval #%llu did not stage: %s",
            (unsigned long long)eval_seq,fail[0]?fail:"no reason");
        return 0;
    }
    collab_last_eval_applied=eval_seq;
    return 1;
}
static int collab_send_ack(uint64_t eval_seq) {
    cJSON *ack=cJSON_CreateObject();
    int ok;
    if (!ack) return 0;
    cJSON_AddStringToObject(ack,"t","ack");
    cJSON_AddNumberToObject(ack,"ref",(double)(eval_seq>0xffffffffu?
        0xffffffffu:eval_seq));
    ok=collab_send_json(ack);
    cJSON_Delete(ack);
    return ok;
}
static int collab_handle_eval(cJSON *obj) {
    const void *vo=obj;
    uint32_t slot32=0, author32=0;
    uint64_t seq=0, num=0;
    unsigned den=1;
    double at_cycle;
    const char *enc=NULL;
    int slot;
    uint8_t *raw=NULL, *plain=NULL;
    size_t rawlen=0, plainlen=0, b64len;
    char *body=NULL;
    int body_slot=-1;
    uint64_t body_seq=0;
    int applied=0;
    if (!cp_get_u32(vo,"slot",&slot32) || slot32>=CP_SLOTS_MAX) return 1;
    if (!cp_get_u32(vo,"author",&author32)) return 1;
    {
        const cJSON *seq_item=cJSON_GetObjectItemCaseSensitive(obj,
            "eval_seq");
        if (!cJSON_IsNumber(seq_item) || !(seq_item->valuedouble>=0) ||
            seq_item->valuedouble>=9007199254740992.0 ||
            seq_item->valuedouble!=(double)(uint64_t)seq_item->valuedouble)
            return 1;
        seq=(uint64_t)seq_item->valuedouble;
    }
    if (!cp_get_cycle(vo,"at_cycle",&num,&den)) return 1;
    at_cycle=(double)num/(double)den;
    if (!isfinite(at_cycle) || at_cycle<0) return 1;
    if (!cp_get_string(vo,"enc",&enc,2*CP_COMPOSITE_MAX) || !enc) return 1;
    slot=(int)slot32;
    {
        int at=collab_member_find(author32);
        uint8_t *author_pk;
        int rc;
        if (at<0) {
            /* Unknown author: not in the verified member set. */
            return 1;
        }
        author_pk=collab_members[at].pk;
        b64len=strlen(enc);
        raw=malloc(b64len*3/4+4);
        if (!raw) return 1;
        if (!cp_b64_decode(enc,raw,b64len*3/4+4,&rawlen)) {
            free(raw);
            return 1;
        }
        rc=ccr_open_ctrl_typed(collab_sub_ctrl,collab_roomid,
            collab_key_epoch,CCR_CTRL_EVAL,(uint16_t)slot,author_pk,raw,
            rawlen,&plain,&plainlen,NULL,NULL,NULL);
        free(raw);
        if (rc!=1) {
            /* Stale epoch, forgery or undecryptable: drop and count it.
               Ten failures from one member in a minute warn (M-later). */
            free(plain);
            return 1;
        }
    }
    if (!ccr_eval_parse((const char *)plain,plainlen,&body_slot,&body_seq,
            &body) ||
        body_slot!=slot) {
        sodium_memzero(plain,plainlen);
        free(plain);
        free(body);
        return 1;
    }
    sodium_memzero(plain,plainlen);
    free(plain);
    {
        int seen=collab_last_seen[slot].seen;
        uint32_t last_author=collab_last_seen[slot].author;
        uint64_t last_seq=collab_last_seen[slot].seq;
        if (seen && last_author==author32 && body_seq<=last_seq) {
            /* Duplicate or stale per-slot author seq: already applied. */
            free(body);
            return 1;
        }
        collab_last_seen[slot].author=author32;
        collab_last_seen[slot].seq=body_seq;
        collab_last_seen[slot].seen=1;
    }
    applied=collab_apply_eval_text(author32,slot,seq,at_cycle,body);
    free(body);
    if (applied) collab_send_ack(seq);
    return 1;
}
/* --- frame dispatch (shared by poll and the join drain) --- */
/* Returns 1 handled, -1 fatal (caller disconnects). */
static int collab_handle_text(cJSON *obj) {
    const cJSON *type=cJSON_GetObjectItemCaseSensitive(obj,"t");
    const char *name=cJSON_IsString(type)?type->valuestring:"";
    if (!strcmp(name,"eval")) return collab_handle_eval(obj);
    if (!strcmp(name,"member_joined") || !strcmp(name,"member_updated")) {
        const cJSON *member=cJSON_GetObjectItemCaseSensitive(obj,"member");
        const cJSON *mid=cJSON_IsObject(member)?
            cJSON_GetObjectItemCaseSensitive(member,"member"):NULL;
        const cJSON *mpk=cJSON_IsObject(member)?
            cJSON_GetObjectItemCaseSensitive(member,"pk"):NULL;
        const cJSON *role=cJSON_IsObject(member)?
            cJSON_GetObjectItemCaseSensitive(member,"role"):NULL;
        const cJSON *profile=cJSON_IsObject(member)?
            cJSON_GetObjectItemCaseSensitive(member,"profile"):NULL;
        uint8_t pk[CCR_PK_BYTES];
        size_t outlen=0;
        uint32_t id;
        if (!cJSON_IsNumber(mid)) return 1;
        id=(uint32_t)mid->valuedouble;
        if (!cJSON_IsString(mpk) ||
            !cp_b64_decode(mpk->valuestring,pk,sizeof(pk),&outlen) ||
            outlen!=sizeof(pk)) {
            return 1;
        }
        collab_member_add(id,pk,
            cJSON_IsString(role)?role->valuestring:"editor",
            cJSON_IsString(profile)?profile->valuestring:NULL);
        if (id==collab_self_member) collab_have_self=1;
        return 1;
    }
    if (!strcmp(name,"member_left")) {
        const cJSON *member=cJSON_GetObjectItemCaseSensitive(obj,"member");
        const cJSON *mid=cJSON_IsObject(member)?
            cJSON_GetObjectItemCaseSensitive(member,"member"):NULL;
        int at;
        if (!cJSON_IsNumber(mid)) return 1;
        at=collab_member_find((uint32_t)mid->valuedouble);
        if (at>=0) {
            collab_members[at].has_nick=0;
            collab_members[at].nick[0]=0;
        }
        return 1;
    }
    if (!strcmp(name,"room_updated")) {
        const cJSON *room=cJSON_GetObjectItemCaseSensitive(obj,"room");
        const cJSON *solos=cJSON_IsObject(room)?
            cJSON_GetObjectItemCaseSensitive(room,"solos_off"):NULL;
        if (cJSON_IsBool(solos)) collab_solos_off=cJSON_IsTrue(solos);
        return 1;
    }
    if (!strcmp(name,"synced")) {
        collab_synced_flag=1;
        return 1;
    }
    if (!strcmp(name,"ack")) {
        const cJSON *ref=cJSON_GetObjectItemCaseSensitive(obj,"ref");
        const cJSON *late=cJSON_GetObjectItemCaseSensitive(obj,"late_by");
        if (cJSON_IsNumber(late))
            log_console("Jam: peer applied eval #%d %.0f ms late",
                cJSON_IsNumber(ref)?ref->valueint:-1,late->valuedouble);
        return 1;
    }
    if (!strcmp(name,"error")) {
        const cJSON *code=cJSON_GetObjectItemCaseSensitive(obj,"code");
        const cJSON *msg=cJSON_GetObjectItemCaseSensitive(obj,"msg");
        log_console("Jam: director error %s: %s",
            cJSON_IsString(code)?code->valuestring:"?",
            cJSON_IsString(msg)?msg->valuestring:"?");
        if (cJSON_IsString(code) && (!strcmp(code->valuestring,"auth") ||
                !strcmp(code->valuestring,"banned"))) {
            return -1;
        }
        return 1;
    }
    if (!strcmp(name,"kicked")) {
        const cJSON *reason=cJSON_GetObjectItemCaseSensitive(obj,"reason");
        log_console("Jam: removed from room%s%s",
            cJSON_IsString(reason)?": ":"",
            cJSON_IsString(reason)?reason->valuestring:"");
        return -1;
    }
    /* joined/auth_ok/welcome/pong/clock and future types: nothing to do
       here (clock filter lands in M4). */
    (void)name;
    return 1;
}
int collab_poll(int timeout_ms) {
    char *text=NULL;
    uint8_t *bin=NULL;
    size_t binlen=0;
    cJSON *obj=NULL;
    CpMsg msg;
    int rc;
    if (!collab_transport) return -1;
    rc=collab_recv_frame(&text,&bin,&binlen,timeout_ms);
    if (rc<=0) {
        free(text);
        free(bin);
        return rc;
    }
    if (bin) {
        /* Binary doc updates replay with the store; Yjs apply lands with
           shared editing (M-later). */
        free(bin);
        return 1;
    }
    obj=cJSON_Parse(text);
    free(text);
    if (!obj) return 1;
    {
        char *flat=cJSON_PrintUnformatted(obj);
        CpError parse=CP_E_PROTO;
        memset(&msg,0,sizeof(msg));
        if (flat) {
            parse=cp_parse_control(flat,strlen(flat),&msg);
            free(flat);
        }
        if (parse!=CP_E_NONE) {
            cJSON_Delete(obj);
            return 1;
        }
    }
    cp_msg_free(&msg);
    rc=collab_handle_text(obj);
    cJSON_Delete(obj);
    if (rc<0) collab_disconnect();
    return rc;
}
/* --- join (section 14.7) --- */
int collab_joined(void) {
    return collab_joined_flag;
}
int collab_synced(void) {
    return collab_synced_flag;
}
uint64_t collab_last_eval_seq(void) {
    return collab_last_eval_applied;
}
uint32_t collab_self_id(void) {
    return collab_self_member;
}
int collab_member_count(void) {
    return collab_nmembers;
}
int collab_member_nick(uint32_t id, char *buf, size_t cap) {
    int at;
    if (!buf || !cap) return 0;
    at=collab_member_find(id);
    if (at>=0 && collab_members[at].has_nick) {
        snprintf(buf,cap,"%s",collab_members[at].nick);
        return 1;
    }
    snprintf(buf,cap,"member-%u",id);
    return 0;
}
/* Sends auth with a fresh member keypair, a proof when claiming, and
   returns after auth_ok. The challenge comes from the welcome frame. */
static int collab_auth(const char *room_id, int as_owner, char *err,
    size_t errlen) {
    uint8_t sig[CCR_SIG_BYTES], mac[CCR_KEY_BYTES];
    char *pk_b64=NULL, *sig_b64=NULL, *proof_b64=NULL;
    cJSON *auth=NULL, *reply=NULL;
    int ok=0;
    if (!collab_have_keys || !collab_have_roomkey ||
        !collab_have_challenge) {
        collab_fail(err,errlen,"collab: no keys or challenge for auth");
        return 0;
    }
    if (!ccr_auth_sign(collab_self_sk,collab_challenge,room_id,
            collab_self_pk,sig) ||
        !ccr_proof(collab_admission,collab_challenge,room_id,
            collab_self_pk,mac)) {
        collab_fail(err,errlen,"collab: could not sign auth");
        return 0;
    }
    pk_b64=collab_b64enc(collab_self_pk,sizeof(collab_self_pk));
    sig_b64=collab_b64enc(sig,sizeof(sig));
    proof_b64=collab_b64enc(mac,sizeof(mac));
    sodium_memzero(sig,sizeof(sig));
    sodium_memzero(mac,sizeof(mac));
    if (!pk_b64 || !sig_b64 || !proof_b64) {
        free(pk_b64);
        free(sig_b64);
        free(proof_b64);
        collab_fail(err,errlen,"collab: out of memory");
        return 0;
    }
    auth=cJSON_CreateObject();
    if (!auth) {
        free(pk_b64);
        free(sig_b64);
        free(proof_b64);
        collab_fail(err,errlen,"collab: out of memory");
        return 0;
    }
    cJSON_AddStringToObject(auth,"t","auth");
    cJSON_AddStringToObject(auth,"room",room_id);
    cJSON_AddStringToObject(auth,"pk",pk_b64);
    cJSON_AddStringToObject(auth,"sig",sig_b64);
    cJSON_AddStringToObject(auth,"proof",proof_b64);
    if (as_owner) cJSON_AddTrueToObject(auth,"owner");
    free(pk_b64);
    free(sig_b64);
    free(proof_b64);
    if (!collab_send_json(auth)) {
        cJSON_Delete(auth);
        collab_fail(err,errlen,"collab: auth send failed");
        return 0;
    }
    cJSON_Delete(auth);
    reply=collab_expect("auth_ok",10000,err,errlen);
    if (!reply) return 0;
    {
        const cJSON *resume=cJSON_GetObjectItemCaseSensitive(reply,"resume");
        size_t outlen=0;
        if (cJSON_IsString(resume) && resume->valuestring &&
            cp_b64_decode(resume->valuestring,collab_resume,
                sizeof(collab_resume),&outlen) &&
            outlen==sizeof(collab_resume)) {
            collab_have_resume=1;
        }
    }
    cJSON_Delete(reply);
    ok=1;
    return ok;
}
/* Seals our nick as a type-2 profile record under the current epoch. */
static char *collab_sealed_profile(void) {
    char nick[64];
    char *json=NULL;
    uint8_t *enc=NULL;
    size_t enclen=0;
    char *out=NULL;
    if (!collab_nick_get(nick,sizeof(nick))) return NULL;
    json=ccr_profile_json(nick,NULL);
    if (!json) return NULL;
    if (!ccr_seal_ctrl(collab_sub_ctrl,collab_roomid,collab_key_epoch,
            CCR_CTRL_PROFILE,0,collab_self_pk,collab_self_sk,
            (const uint8_t *)json,strlen(json),&enc,&enclen)) {
        free(json);
        return NULL;
    }
    free(json);
    out=collab_b64enc(enc,enclen);
    sodium_memzero(enc,enclen);
    free(enc);
    return out;
}
int collab_join_room(const char *room_id, const uint8_t room_key[32],
    int as_owner, char *err, size_t errlen) {
    uint8_t master[CCR_SEED_BYTES];
    uint8_t pk[CCR_PK_BYTES], sk[CCR_SK_BYTES];
    cJSON *join=NULL, *joined=NULL;
    const cJSON *room=NULL, *members=NULL, *slots=NULL, *you=NULL;
    double max_cycle=-1;
    uint64_t max_seq=0;
    int have_apply=0;
    if (!collab_transport || collab_current_state!=COLLAB_CONNECTED) {
        collab_fail(err,errlen,"collab: connect first");
        return 0;
    }
    if (!room_id || !room_key) {
        collab_fail(err,errlen,"collab: room id and key required");
        return 0;
    }
    if (!ccr_room_ok(room_id)) {
        collab_fail(err,errlen,"collab: bad room id");
        return 0;
    }
    collab_session_reset();
    collab_room_reset();
    if (!ccr_identity_load_or_create(master,NULL,0)) {
        collab_fail(err,errlen,"collab: no identity seed");
        return 0;
    }
    if (!ccr_room_keypair(master,room_id,pk,sk)) {
        sodium_memzero(master,sizeof(master));
        collab_fail(err,errlen,"collab: no member key");
        return 0;
    }
    sodium_memzero(master,sizeof(master));
    memcpy(collab_self_pk,pk,sizeof(pk));
    memcpy(collab_self_sk,sk,sizeof(sk));
    sodium_memzero(sk,sizeof(sk));
    collab_have_keys=1;
    memcpy(collab_roomkey,room_key,CCR_KEY_BYTES);
    collab_have_roomkey=1;
    ccr_admission_key(room_key,collab_admission);
    ccr_subkey(room_key,2,collab_sub_ctrl);
    ccr_subkey(room_key,1,collab_sub_frame);
    collab_key_epoch=0;
    snprintf(collab_roomid,sizeof(collab_roomid),"%s",room_id);
    if (!collab_auth(room_id,as_owner,err,errlen)) {
        collab_session_reset();
        return 0;
    }
    {
        char *profile=collab_sealed_profile();
        if (!profile) {
            collab_fail(err,errlen,"collab: could not seal profile");
            collab_session_reset();
            return 0;
        }
        join=cJSON_CreateObject();
        if (!join) {
            free(profile);
            collab_fail(err,errlen,"collab: out of memory");
            collab_session_reset();
            return 0;
        }
        cJSON_AddStringToObject(join,"t","join");
        cJSON_AddStringToObject(join,"room",room_id);
        cJSON_AddStringToObject(join,"profile",profile);
        free(profile);
        if (!collab_send_json(join)) {
            cJSON_Delete(join);
            collab_fail(err,errlen,"collab: join send failed");
            collab_session_reset();
            return 0;
        }
        cJSON_Delete(join);
    }
    joined=collab_expect("joined",10000,err,errlen);
    if (!joined) {
        collab_session_reset();
        return 0;
    }
    room=cJSON_GetObjectItemCaseSensitive(joined,"room");
    you=cJSON_GetObjectItemCaseSensitive(joined,"you");
    if (!cJSON_IsObject(room) ||
        !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(room,"e2e"))) {
        cJSON_Delete(joined);
        collab_fail(err,errlen,"collab: room is not end-to-end");
        collab_session_reset();
        return 0;
    }
    {
        const cJSON *epoch=cJSON_GetObjectItemCaseSensitive(room,"epoch");
        const cJSON *opk=cJSON_GetObjectItemCaseSensitive(room,"opk");
        size_t outlen=0;
        if (cJSON_IsNumber(epoch) && epoch->valuedouble>=0 &&
            epoch->valuedouble<65536)
            collab_key_epoch=(uint16_t)epoch->valuedouble;
        if (cJSON_IsString(opk) && opk->valuestring &&
            cp_b64_decode(opk->valuestring,collab_opk,sizeof(collab_opk),
                &outlen) &&
            outlen==sizeof(collab_opk)) {
            collab_have_opk=1;
        }
    }
    {
        const cJSON *mid=cJSON_IsObject(you)?
            cJSON_GetObjectItemCaseSensitive(you,"member"):NULL;
        if (!cJSON_IsNumber(mid)) {
            cJSON_Delete(joined);
            collab_fail(err,errlen,"collab: no member id");
            collab_session_reset();
            return 0;
        }
        collab_self_member=(uint32_t)mid->valuedouble;
        collab_have_self=1;
    }
    members=cJSON_GetObjectItemCaseSensitive(joined,"members");
    if (cJSON_IsArray(members)) {
        const cJSON *entry=NULL;
        cJSON_ArrayForEach(entry,members) {
            const cJSON *mid=cJSON_GetObjectItemCaseSensitive(entry,
                "member");
            const cJSON *mpk=cJSON_GetObjectItemCaseSensitive(entry,"pk");
            const cJSON *role=cJSON_GetObjectItemCaseSensitive(entry,
                "role");
            const cJSON *profile=cJSON_GetObjectItemCaseSensitive(entry,
                "profile");
            uint8_t pk[CCR_PK_BYTES];
            size_t outlen=0;
            if (!cJSON_IsNumber(mid) || !cJSON_IsString(mpk) ||
                !cp_b64_decode(mpk->valuestring,pk,sizeof(pk),&outlen) ||
                outlen!=sizeof(pk)) {
                continue;
            }
            collab_member_add((uint32_t)mid->valuedouble,pk,
                cJSON_IsString(role)?role->valuestring:"editor",
                cJSON_IsString(profile)?profile->valuestring:NULL);
        }
    }
    /* Anchor the provisional room→local mapping (M4 owns the real
       filter): the epoch start is adopted at the join instant. */
    {
        const cJSON *clock=cJSON_GetObjectItemCaseSensitive(joined,"clock");
        const cJSON *epoch=cJSON_IsObject(clock)?
            cJSON_GetObjectItemCaseSensitive(clock,"epoch"):NULL;
        const cJSON *c=cJSON_IsObject(epoch)?
            cJSON_GetObjectItemCaseSensitive(epoch,"c"):NULL;
        const cJSON *cps=cJSON_IsObject(epoch)?
            cJSON_GetObjectItemCaseSensitive(epoch,"cps"):NULL;
        uint64_t num=0;
        unsigned den=1;
        if (cp_get_cycle(epoch,"c",&num,&den) && cJSON_IsArray(c)) {
            collab_epoch_c=(double)num/(double)den;
        }
        if (cJSON_IsNumber(cps) && cps->valuedouble>0 &&
            cps->valuedouble<=4) {
            collab_room_cps=cps->valuedouble;
        }
        collab_join_audio=audio_time_now();
    }
    slots=cJSON_GetObjectItemCaseSensitive(joined,"slots");    if (cJSON_IsArray(slots)) {
        const cJSON *entry=NULL;
        cJSON_ArrayForEach(entry,slots) {
            const cJSON *slot_item=cJSON_GetObjectItemCaseSensitive(entry,
                "slot");
            const cJSON *last=cJSON_GetObjectItemCaseSensitive(entry,
                "last_eval");
            const cJSON *enc_item, *author, *seq_item, *cycle;
            uint8_t *raw=NULL, *plain=NULL;
            size_t rawlen=0, plainlen=0, b64len;
            uint64_t seq=0, num=0;
            unsigned den=1;
            char *body=NULL;
            int body_slot=-1;
            uint64_t body_seq=0;
            int slot;
            if (!cJSON_IsObject(last) || !cJSON_IsNumber(slot_item))
                continue;
            slot=(int)slot_item->valuedouble;
            if (slot<0 || slot>=CP_SLOTS_MAX) continue;
            enc_item=cJSON_GetObjectItemCaseSensitive(last,"enc");
            author=cJSON_GetObjectItemCaseSensitive(last,"author");
            seq_item=cJSON_GetObjectItemCaseSensitive(last,"seq");
            cycle=cJSON_GetObjectItemCaseSensitive(last,"at_cycle");
            if (!cJSON_IsString(enc_item) || !enc_item->valuestring ||
                !cJSON_IsNumber(author) || !cJSON_IsNumber(seq_item) ||
                !cycle) {
                continue;
            }
            {
                int at=collab_member_find((uint32_t)author->valuedouble);
                uint8_t *apk;
                int rc;
                if (at<0) continue;
                apk=collab_members[at].pk;
                b64len=strlen(enc_item->valuestring);
                if (!b64len || b64len>2*CP_COMPOSITE_MAX) continue;
                raw=malloc(b64len*3/4+4);
                if (!raw) continue;
                if (!cp_b64_decode(enc_item->valuestring,raw,b64len*3/4+4,
                        &rawlen)) {
                    free(raw);
                    continue;
                }
                rc=ccr_open_ctrl_typed(collab_sub_ctrl,collab_roomid,
                    collab_key_epoch,CCR_CTRL_EVAL,(uint16_t)slot,apk,raw,
                    rawlen,&plain,&plainlen,NULL,NULL,NULL);
                free(raw);
                if (rc!=1) {
                    free(plain);
                    continue;
                }
            }
            if (!ccr_eval_parse((const char *)plain,plainlen,&body_slot,
                    &body_seq,&body) ||
                body_slot!=slot) {
                sodium_memzero(plain,plainlen);
                free(plain);
                free(body);
                continue;
            }
            sodium_memzero(plain,plainlen);
            free(plain);
            seq=(uint64_t)seq_item->valuedouble;
            if (!cp_get_cycle(last,"at_cycle",&num,&den)) {
                free(body);
                continue;
            }
            if (!collab_room_set_slot(slot,body,strlen(body),
                    (uint32_t)author->valuedouble!=collab_self_member)) {
                free(body);
                continue;
            }
            free(body);
            collab_last_seen[slot].author=(uint32_t)author->valuedouble;
            collab_last_seen[slot].seq=body_seq;
            collab_last_seen[slot].seen=1;
            {
                double at=(double)num/(double)den;
                if (at>max_cycle) {
                    max_cycle=at;
                    max_seq=seq;
                    have_apply=1;
                }
            }
        }
    }
    cJSON_Delete(joined);
    if (have_apply && isfinite(max_cycle) && max_cycle>=0) {
        char fail[256]="";
        uint64_t capped=max_seq>0xffffffffu?0xffffffffu:max_seq;
        if (!collab_room_apply(collab_pending_for(max_cycle),
                (unsigned)capped,fail,sizeof(fail))) {
            log_console("Jam: replay did not stage: %s",
                fail[0]?fail:"no reason");
        }
    }
    collab_joined_flag=1;
    /* Drain the stored replay; live frames dispatch normally. */
    {
        uint64_t deadline=(uint64_t)now_seconds()*1000+30000;
        while (!collab_synced_flag && collab_transport) {
            uint64_t now=(uint64_t)now_seconds()*1000;
            int64_t left;
            int step, got;
            if (now>=deadline) {
                collab_fail(err,errlen,"collab: replay timed out");
                collab_disconnect();
                return 0;
            }
            left=(int64_t)(deadline-now);
            step=left>2000?2000:(int)left;
            got=collab_poll(step);
            if (got<0) {
                collab_fail(err,errlen,"collab: replay failed");
                return 0;
            }
        }
    }
    return 1;
}
int collab_eval_send(int slot, const char *body, char *err, size_t errlen) {
    char *json=NULL;
    uint8_t *enc=NULL;
    size_t enclen=0, length;
    char *b64=NULL;
    cJSON *req=NULL;
    uint64_t seq;
    int ok=0;
    if (!collab_transport || !collab_joined_flag || !collab_have_roomkey ||
        !collab_have_keys) {
        collab_fail(err,errlen,"collab: join a room first");
        return 0;
    }
    if (slot<0 || slot>=CP_SLOTS_MAX || !body) {
        collab_fail(err,errlen,"collab: bad slot or body");
        return 0;
    }
    length=strlen(body);
    if (length>CP_EVAL_BODY_MAX) {
        collab_fail(err,errlen,"collab: body exceeds 256 KiB");
        return 0;
    }
    seq=++collab_author_seq[slot];
    if (!seq) seq=++collab_author_seq[slot];
    json=ccr_eval_json(slot,seq,body);
    if (!json) {
        collab_fail(err,errlen,"collab: bad eval record");
        return 0;
    }
    if (!ccr_seal_ctrl(collab_sub_ctrl,collab_roomid,collab_key_epoch,
            CCR_CTRL_EVAL,(uint16_t)slot,collab_self_pk,collab_self_sk,
            (const uint8_t *)json,strlen(json),&enc,&enclen)) {
        free(json);
        collab_fail(err,errlen,"collab: could not seal eval");
        return 0;
    }
    free(json);
    b64=collab_b64enc(enc,enclen);
    sodium_memzero(enc,enclen);
    free(enc);
    if (!b64) {
        collab_fail(err,errlen,"collab: out of memory");
        return 0;
    }
    req=cJSON_CreateObject();
    if (!req) {
        free(b64);
        collab_fail(err,errlen,"collab: out of memory");
        return 0;
    }
    cJSON_AddStringToObject(req,"t","eval_req");
    cJSON_AddNumberToObject(req,"slot",(double)slot);
    cJSON_AddStringToObject(req,"enc",b64);
    free(b64);
    ok=collab_send_json(req);
    cJSON_Delete(req);
    if (!ok) collab_fail(err,errlen,"collab: eval_req send failed");
    return ok;
}
