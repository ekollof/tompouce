#include "collab.h"
#include "collab_proto.h"
#include "collab_transport.h"
#include <cjson/cJSON.h>
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
    char path[1100], dir[1024], tmp[1120];
    char *text=NULL;
    cJSON *root=NULL;
    FILE *file=NULL;
    char *printed=NULL;
    size_t length;
    int fd, ok=0;
    if (!collab_json_path(path,sizeof(path))) return 0;
    if (!collab_config_dir(dir,sizeof(dir))) return 0;
    mkdir(dir,0700);
    text=collab_read_file(path,NULL);
    if (text) {
        root=cJSON_Parse(text);
        free(text);
    }
    if (!root) root=cJSON_CreateObject();
    if (!root) return 0;
    cJSON_DeleteItemFromObjectCaseSensitive(root,"nick");
    if (!cJSON_AddStringToObject(root,"nick",nick)) {
        cJSON_Delete(root);
        return 0;
    }
    printed=cJSON_Print(root);
    cJSON_Delete(root);
    if (!printed) return 0;
    length=strlen(printed);
    if (snprintf(tmp,sizeof(tmp),"%s/.collab.json.tmp",dir)>=(int)sizeof(tmp)) {
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
