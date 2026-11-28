/* Jam-mode M2 client flow against a real director: Alice joins via the
   public collab API (auth, sealed profile, joined/synced verify, replay),
   sends evals, and applies them plus Bob's; Bob is a minimal scripted
   client that checks the broadcast, the ack relay, the drop path for
   bogus seals and the duplicate-seq gate. One audio engine (Alice's);
   Bob never touches collab.c state. */
#include <assert.h>
#include <curl/curl.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "../src/audio.c"
#include "../src/scheduler.c"
#include "collab.h"
#include "collab_crypto.h"
#include "collab_proto.h"
#include "track.h"
#define TEST_ROOM "mfrggzdfmztwq2lknnwg23tpoa"
static const uint8_t room_key[32]={
    0x5a,0x5a,0x5a,0x5a,0x5a,0x5a,0x5a,0x5a,
    0x5a,0x5a,0x5a,0x5a,0x5a,0x5a,0x5a,0x5a,
    0x5a,0x5a,0x5a,0x5a,0x5a,0x5a,0x5a,0x5a,
    0x5a,0x5a,0x5a,0x5a,0x5a,0x5a,0x5a,0x5a};
static const uint8_t bob_master[32]={
    0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,
    0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,0x20,
    0x21,0x22,0x23,0x24,0x25,0x26,0x27,0x28,
    0x29,0x2a,0x2b,0x2c,0x2d,0x2e,0x2f,0x30};
static pid_t test_child=-1;
static char test_keypath[1024]="";
static void test_cleanup(void) {
    if (test_child>0) {
        kill(test_child,15);
        waitpid(test_child,NULL,0);
        test_child=-1;
    }
    if (test_keypath[0]) unlink(test_keypath);
}
static void check(int ok, const char *what, int line) {
    if (!ok) {
        fprintf(stderr,"collab flow FAILED at line %d: %s\n",line,what);
        exit(1);
    }
}
#define CHECK(ok, what) check(!!(ok), what, __LINE__)
/* --- engine driver (Alice hears the room) --- */
static void run_gen(double now) {
    pthread_mutex_lock(&tracks_lock);
    assert(schedule_live_javascript(now));
    pthread_mutex_unlock(&tracks_lock);
}
static void run_ticks(double first, int n) {
    int i;
    for (i=0;i<n;i++) run_gen(first+0.1*i);
}
static void reset(const char *code) {
    audio_set_bpm(120.0);
    audio_time=100.0;
    audio_toggle_play(0);
    event_queue_clear();
    collab_room_reset();
    collab_disconnect();
    assert(track_load_from_code(code));
    audio_time=100.0;
    audio_toggle_play(1);
}
typedef struct {
    double time, freq;
    int slot;
} TaggedKey;
static int tagged_cmp(const void *a, const void *b) {
    const TaggedKey *x=a, *y=b;
    if (x->time<y->time) return -1;
    if (x->time>y->time) return 1;
    if (x->freq<y->freq) return -1;
    if (x->freq>y->freq) return 1;
    return x->slot-y->slot;
}
static int drain_tagged(TaggedKey *out, int cap) {
    QNode *node;
    int n=0;
    pthread_mutex_lock(&tracks_lock);
    for (node=qhead;node && n<cap;node=node->next) {
        out[n].time=node->ev.time;
        out[n].freq=node->ev.freq;
        out[n].slot=node->ev.slot;
        n++;
    }
    pthread_mutex_unlock(&tracks_lock);
    event_queue_clear();
    qsort(out,(size_t)n,sizeof(*out),tagged_cmp);
    return n;
}
/* --- minimal raw client (Bob) --- */
typedef struct {
    CURL *handle;
} RawClient;
static size_t discard_write(char *ptr, size_t size, size_t nmemb,
    void *userdata) {
    (void)ptr;
    (void)userdata;
    return size*nmemb;
}
static uint64_t raw_now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC,&now);
    return (uint64_t)now.tv_sec*1000u+(uint64_t)now.tv_nsec/1000000u;
}
static int raw_send(RawClient *c, const void *data, size_t len, int binary) {
    size_t sent=0;
    while (sent<len) {
        size_t chunk=0;
        CURLcode code=curl_ws_send(c->handle,(const char *)data+sent,
            len-sent,&chunk,0,binary?CURLWS_BINARY:CURLWS_TEXT);
        if (code!=CURLE_OK || chunk==0) return 0;
        sent+=chunk;
    }
    return 1;
}
static int raw_wait(RawClient *c, int timeout_ms) {
    curl_socket_t sock=CURL_SOCKET_BAD;
    struct pollfd fd;
    int ready;
    if (curl_easy_getinfo(c->handle,CURLINFO_ACTIVESOCKET,
            &sock)!=CURLE_OK) {
        return 0;
    }
    if (sock==CURL_SOCKET_BAD) return 0;
    fd.fd=sock;
    fd.events=POLLIN;
    fd.revents=0;
    ready=poll(&fd,1,timeout_ms);
    return ready>0 && (fd.revents&POLLIN);
}
/* One complete text frame (binary frames skipped); NULL on timeout. */
static cJSON *raw_recv(RawClient *c, int timeout_ms) {
    uint64_t deadline=raw_now_ms()+(uint64_t)timeout_ms;
    uint8_t *buffer=NULL;
    size_t length=0, capacity=0;
    for (;;) {
        char chunk[16384];
        size_t got=0;
        const struct curl_ws_frame *meta=NULL;
        CURLcode code;
        if (raw_now_ms()>=deadline) {
            free(buffer);
            return NULL;
        }
        code=curl_ws_recv(c->handle,chunk,sizeof(chunk),&got,&meta);
        if (code==CURLE_AGAIN) {
            int64_t left=(int64_t)(deadline-raw_now_ms());
            if (!raw_wait(c,left>60000?60000:(int)left)) continue;
            continue;
        }
        if (code!=CURLE_OK) {
            free(buffer);
            return NULL;
        }
        if (meta && (meta->flags&CURLWS_CLOSE)) {
            free(buffer);
            return NULL;
        }
        if (meta && (meta->flags&CURLWS_BINARY)) continue;
        if (got==0) continue;
        if (length+got+1<length || length+got+1>4*1024*1024) {
            free(buffer);
            return NULL;
        }
        if (length+got+1>capacity) {
            size_t grown=(length+got+1)*2;
            uint8_t *grown_buffer=realloc(buffer,grown);
            if (!grown_buffer) {
                free(buffer);
                return NULL;
            }
            buffer=grown_buffer;
            capacity=grown;
        }
        memcpy(buffer+length,chunk,got);
        length+=got;
        if (meta && (meta->flags&CURLWS_CONT)) continue;
        {
            cJSON *obj=cJSON_ParseWithLength((const char *)buffer,length);
            free(buffer);
            return obj;
        }
    }
}
static void raw_send_json(RawClient *c, cJSON *obj) {
    char *text=cJSON_PrintUnformatted(obj);
    assert(text);
    assert(raw_send(c,text,strlen(text),0));
    free(text);
}
static cJSON *raw_expect(RawClient *c, const char *want, int timeout_ms) {
    cJSON *obj=raw_recv(c,timeout_ms);
    const cJSON *type;
    assert(obj);
    type=cJSON_GetObjectItemCaseSensitive(obj,"t");
    if (!cJSON_IsString(type) || strcmp(type->valuestring,want)) {
        char *dump=cJSON_PrintUnformatted(obj);
        fprintf(stderr,"expected %s, got %s\n",want,dump?dump:"null");
        free(dump);
        cJSON_Delete(obj);
        assert(0);
    }
    return obj;
}
static void b64e(const uint8_t *in, size_t len, char *out, size_t cap) {
    assert(cp_b64_encode(in,len,out,cap)>0 || len==0);
}
int main(void) {
    char url[64], keypath[]="/tmp/tompouce-flow-key-XXXXXX";
    const char *binary;
    int keyfd, port, attempt;
    pid_t pid=-1;
    char err[256]="";
    CollabConfig cfg;
    uint8_t admission[32];
    uint8_t bob_pk[32], bob_sk[64], bob_sub[32], bob_challenge[32];
    uint64_t bob_seq=0, eval_seq=0;
    double eval_cycle=0;
    RawClient bob;
    cJSON *msg;
    int i;
    assert(sodium_init()>=0);
    assert(ccr_init());
    binary=getenv("TOMPOUCE_DIRECTOR_BIN");
    if (!binary || !*binary) binary="./build/tompouce-director";
    if (access(binary,X_OK)!=0) {
        puts("collab flow skipped: no director binary");
        return 0;
    }
    if (atexit(test_cleanup)!=0) return 1;
    keyfd=mkstemp(keypath);
    CHECK(keyfd>=0,"mkstemp");
    /* Admission key pinned by the brief: kdf(room_key, id 0, "tpcadm01"). */
    CHECK(crypto_kdf_derive_from_key(admission,sizeof(admission),0,
        "tpcadm01",room_key)==0,"kdf");
    CHECK(write(keyfd,admission,sizeof(admission))==sizeof(admission),"key");
    close(keyfd);
    snprintf(test_keypath,sizeof(test_keypath),"%s",keypath);
    port=0;
    for (attempt=0;attempt<20 && port==0;attempt++) {
        int probe_fd, bound=0;
        struct sockaddr_in probe_addr;
        int candidate=17529+attempt;
        probe_fd=socket(AF_INET,SOCK_STREAM,0);
        if (probe_fd<0) continue;
        memset(&probe_addr,0,sizeof(probe_addr));
        probe_addr.sin_family=AF_INET;
        probe_addr.sin_port=htons((uint16_t)candidate);
        probe_addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        bound=bind(probe_fd,(struct sockaddr *)&probe_addr,
            sizeof(probe_addr))==0;
        close(probe_fd);
        if (!bound) continue;
        pid=fork();
        CHECK(pid>=0,"fork");
        if (pid==0) {
            static char addr[64];
            snprintf(addr,sizeof(addr),"127.0.0.1:%d",candidate);
            execl(binary,"tompouce-director","--listen",addr,
                "--admission-key-file",keypath,(char *)NULL);
            _exit(127);
        }
        port=candidate;
    }
    CHECK(port>0,"director spawn");
    test_child=pid;
    snprintf(url,sizeof(url),"ws://127.0.0.1:%d/v1",port);
    /* Alice: live engine plus the public collab API. */
    reset("$: note(60).s('sine')");
    run_ticks(100.0,5);
    memset(&cfg,0,sizeof(cfg));
    snprintf(cfg.url,sizeof(cfg.url),"%s",url);
    {
        int tries, connected=0;
        for (tries=0;tries<50 && !connected;tries++) {
            if (tries>0) usleep(100000);
            memset(err,0,sizeof(err));
            connected=collab_connect(&cfg,err,sizeof(err));
            if (!connected) collab_disconnect();
        }
        CHECK(connected,"alice connect");
    }
    CHECK(collab_join_room(TEST_ROOM,room_key,1,err,sizeof(err)),err);
    CHECK(collab_joined(),"alice joined");
    CHECK(collab_synced(),"alice synced");
    CHECK(collab_member_count()==1,"alice alone");
    CHECK(collab_self_id()==1,"alice is member 1");
    /* Alice's own eval comes back through the director and stages. */
    CHECK(collab_eval_send(0,"$: note(62).s('sine')",err,sizeof(err)),
        "alice eval send");
    {
        int tries;
        for (tries=0;tries<50 && !collab_last_eval_seq();tries++)
            CHECK(collab_poll(2000)>=0,"alice poll");
        CHECK(collab_last_eval_seq()==1,"alice eval_seq 1");
    }
    run_ticks(100.5,60);
    {
        TaggedKey keys[64];
        int n=drain_tagged(keys,64), found=0;
        for (i=0;i<n;i++) {
            if (keys[i].freq>293 && keys[i].freq<294 && keys[i].slot==0)
                found=1;
        }
        if (!found) {
            fprintf(stderr,"alice own eval: %d events",n);
            for (i=0;i<n && i<10;i++)
                fprintf(stderr," (%.1f,%.1f,s%d)",keys[i].time,keys[i].freq,
                    keys[i].slot);
            fprintf(stderr,"\n");
        }
        CHECK(found,"alice hears own eval");
    }
    /* Bob: scripted handshake with a real proof and sealed profile. */
    assert(ccr_room_keypair(bob_master,TEST_ROOM,bob_pk,bob_sk));
    ccr_subkey(room_key,2,bob_sub);
    {
        struct curl_slist *hdrs=NULL;
        CURL *probe=curl_easy_init();
        CHECK(probe!=NULL,"curl");
        curl_easy_setopt(probe,CURLOPT_URL,url);
        curl_easy_setopt(probe,CURLOPT_CONNECT_ONLY,2L);
        curl_easy_setopt(probe,CURLOPT_WRITEFUNCTION,discard_write);
        for (i=0;i<50;i++) {
            if (i>0) usleep(100000);
            hdrs=curl_slist_append(hdrs,
                "Sec-WebSocket-Protocol: tompouce.collab.v1");
            curl_easy_setopt(probe,CURLOPT_HTTPHEADER,hdrs);
            curl_easy_setopt(probe,CURLOPT_TIMEOUT_MS,200L);
            if (curl_easy_perform(probe)==CURLE_OK) {
                curl_slist_free_all(hdrs);
                break;
            }
            curl_slist_free_all(hdrs);
            hdrs=NULL;
            curl_easy_setopt(probe,CURLOPT_HTTPHEADER,NULL);
        }
        CHECK(i<50,"bob probe connect");
        bob.handle=probe;
    }
    {
        cJSON *hello=cJSON_CreateObject();
        cJSON *proto=cJSON_CreateArray();
        cJSON *caps=cJSON_CreateArray();
        cJSON_AddStringToObject(hello,"t","hello");
        cJSON_AddItemToArray(proto,cJSON_CreateNumber(1));
        cJSON_AddItemToObject(hello,"proto",proto);
        cJSON_AddItemToArray(caps,cJSON_CreateString("e2e"));
        cJSON_AddItemToObject(hello,"caps",caps);
        raw_send_json(&bob,hello);
        cJSON_Delete(hello);
    }
    msg=raw_expect(&bob,"welcome",10000);
    {
        const cJSON *ch=cJSON_GetObjectItemCaseSensitive(msg,"challenge");
        size_t outlen=0;
        CHECK(cJSON_IsString(ch) &&
            cp_b64_decode(ch->valuestring,bob_challenge,sizeof(bob_challenge),
                &outlen) &&
            outlen==sizeof(bob_challenge),"bob challenge");
    }
    cJSON_Delete(msg);
    {
        uint8_t sig[64], proof[32];
        uint8_t m[18+32+26+32], amsg[32+26+32];
        char pk_b64[64], sig_b64[128], proof_b64[64];
        cJSON *auth=cJSON_CreateObject();
        memcpy(m,"tompouce-collab-v1",18);
        memcpy(m+18,bob_challenge,32);
        memcpy(m+18+32,TEST_ROOM,26);
        memcpy(m+18+32+26,bob_pk,32);
        CHECK(crypto_sign_detached(sig,NULL,m,sizeof(m),bob_sk)==0,"sign");
        memcpy(amsg,bob_challenge,32);
        memcpy(amsg+32,TEST_ROOM,26);
        memcpy(amsg+32+26,bob_pk,32);
        CHECK(crypto_generichash(proof,sizeof(proof),amsg,sizeof(amsg),
            admission,sizeof(admission))==0,"proof");
        b64e(bob_pk,32,pk_b64,sizeof(pk_b64));
        b64e(sig,64,sig_b64,sizeof(sig_b64));
        b64e(proof,32,proof_b64,sizeof(proof_b64));
        cJSON_AddStringToObject(auth,"t","auth");
        cJSON_AddStringToObject(auth,"room",TEST_ROOM);
        cJSON_AddStringToObject(auth,"pk",pk_b64);
        cJSON_AddStringToObject(auth,"sig",sig_b64);
        cJSON_AddStringToObject(auth,"proof",proof_b64);
        raw_send_json(&bob,auth);
        cJSON_Delete(auth);
    }
    msg=raw_expect(&bob,"auth_ok",10000);
    cJSON_Delete(msg);
    {
        /* Bob's profile is opaque to the director; send a dummy blob so
           Alice exercises the unverified-profile fallback. */
        cJSON *join=cJSON_CreateObject();
        cJSON_AddStringToObject(join,"t","join");
        cJSON_AddStringToObject(join,"room",TEST_ROOM);
        cJSON_AddStringToObject(join,"profile","e30");
        raw_send_json(&bob,join);
        cJSON_Delete(join);
    }
    msg=raw_expect(&bob,"joined",10000);
    cJSON_Delete(msg);
    msg=raw_expect(&bob,"synced",15000);
    cJSON_Delete(msg);
    /* Alice learns Bob with a fallback nick: the dummy profile cannot
       verify, so it never renders. */
    {
        int tries;
        char nick[64]="";
        for (tries=0;tries<50 && collab_member_count()<2;tries++)
            CHECK(collab_poll(2000)>=0,"alice member poll");
        CHECK(collab_member_count()==2,"two members");
        CHECK(!collab_member_nick(2,nick,sizeof(nick)),"fallback nick");
        CHECK(!strcmp(nick,"member-2"),"fallback text");
    }
    /* Bob's sealed eval reaches Alice, stages at the shared instant, and
       Alice's ack relays back with the director's eval_seq. */
    char *bob_good_enc=NULL;
    {
        char *json=NULL, *b64=NULL;
        uint8_t *enc=NULL, nonce[24];
        size_t enclen=0, k;
        cJSON *req=NULL;
        for (k=0;k<sizeof(nonce);k++) nonce[k]=(uint8_t)(0xc0+k);
        json=ccr_eval_json(1,++bob_seq,"$: note(67).s('sine')");
        CHECK(json!=NULL,"bob eval json");
        CHECK(ccr_seal_ctrl_nonce(bob_sub,TEST_ROOM,0,CCR_CTRL_EVAL,1,
            bob_pk,bob_sk,nonce,(const uint8_t *)json,strlen(json),&enc,
            &enclen),"bob seal");
        free(json);
        b64=malloc(4*((enclen+2)/3)+1);
        CHECK(b64!=NULL,"oom");
        b64[cp_b64_encode(enc,enclen,b64,4*((enclen+2)/3)+1)]=0;
        sodium_memzero(enc,enclen);
        free(enc);
        bob_good_enc=b64;
        req=cJSON_CreateObject();
        cJSON_AddStringToObject(req,"t","eval_req");
        cJSON_AddNumberToObject(req,"slot",1);
        cJSON_AddStringToObject(req,"enc",b64);
        raw_send_json(&bob,req);
        cJSON_Delete(req);
    }
    msg=raw_expect(&bob,"eval",10000);
    {
        const cJSON *seq=cJSON_GetObjectItemCaseSensitive(msg,"eval_seq");
        const cJSON *at=cJSON_GetObjectItemCaseSensitive(msg,"at_cycle");
        const cJSON *slot=cJSON_GetObjectItemCaseSensitive(msg,"slot");
        const cJSON *enc=cJSON_GetObjectItemCaseSensitive(msg,"enc");
        CHECK(cJSON_IsNumber(seq),"bob eval seq");
        eval_seq=(uint64_t)seq->valuedouble;
        CHECK(eval_seq==2,"bob eval_seq 2");
        CHECK(cJSON_IsArray(at) && cJSON_GetArraySize(at)==2,"cycle pair");
        eval_cycle=(double)cJSON_GetArrayItem(at,0)->valuedouble/
            (double)cJSON_GetArrayItem(at,1)->valuedouble;
        CHECK(isfinite(eval_cycle) && eval_cycle>0,"at_cycle");
        CHECK(cJSON_IsNumber(slot) && slot->valuedouble==1,"slot");
        CHECK(cJSON_IsString(enc),"enc");
    }
    cJSON_Delete(msg);
    {
        int tries;
        for (tries=0;tries<50 && collab_last_eval_seq()<eval_seq;tries++)
            CHECK(collab_poll(2000)>=0,"alice eval poll");
        CHECK(collab_last_eval_seq()==eval_seq,"alice applied");
    }
    msg=raw_expect(&bob,"ack",10000);
    {
        const cJSON *ref=cJSON_GetObjectItemCaseSensitive(msg,"ref");
        CHECK(cJSON_IsNumber(ref) &&
            (uint64_t)ref->valuedouble==eval_seq,"ack ref");
        CHECK(!cJSON_GetObjectItemCaseSensitive(msg,"late_by"),
            "no late_by on time");
    }
    cJSON_Delete(msg);
    run_ticks(106.5,60);
    {
        TaggedKey keys[64];
        int n=drain_tagged(keys,64), found=0;
        for (i=0;i<n;i++) {
            if (keys[i].freq>391 && keys[i].freq<393 && keys[i].slot==1)
                found=1;
        }
        if (!found) {
            fprintf(stderr,"bob eval: %d events",n);
            for (i=0;i<n && i<16;i++)
                fprintf(stderr," (%.1f,%.1f,s%d)",keys[i].time,keys[i].freq,
                    keys[i].slot);
            fprintf(stderr,"\n");
        }
        CHECK(found,"alice hears bob slot 1");
    }
    /* A bogus seal is dropped without staging; the session stays healthy
       and a duplicate of the good eval is a no-op (no second ack). */
    {
        cJSON *req=cJSON_CreateObject();
        cJSON_AddStringToObject(req,"t","eval_req");
        cJSON_AddNumberToObject(req,"slot",1);
        cJSON_AddStringToObject(req,"enc","bogus");
        raw_send_json(&bob,req);
        cJSON_Delete(req);
    }
    {
        int tries;
        for (tries=0;tries<10;tries++)
            CHECK(collab_poll(1000)>=0,"alice bogus poll");
    }
    CHECK(collab_last_eval_seq()==eval_seq,"bogus dropped");
    {
        /* The director still rebroadcasts the bogus body (it never
           decrypts); drain it so the dup check below sees only its own
           frames. */
        cJSON *bogus=raw_recv(&bob,10000);
        const cJSON *t=bogus?cJSON_GetObjectItemCaseSensitive(bogus,"t"):
            NULL;
        CHECK(bogus && cJSON_IsString(t) &&
            !strcmp(t->valuestring,"eval"),"bogus rebroadcast");
        cJSON_Delete(bogus);
    }
    /* A duplicate of the good eval (same author seq) is a no-op: no
       re-activation, no second ack. */
    {
        cJSON *req=cJSON_CreateObject();
        cJSON_AddStringToObject(req,"t","eval_req");
        cJSON_AddNumberToObject(req,"slot",1);
        cJSON_AddStringToObject(req,"enc",bob_good_enc);
        raw_send_json(&bob,req);
        cJSON_Delete(req);
    }
    {
        int tries;
        for (tries=0;tries<10;tries++)
            CHECK(collab_poll(1000)>=0,"alice dup poll");
    }
    CHECK(collab_last_eval_seq()==eval_seq,"dup dropped");
    {
        /* The director still rebroadcasts the duplicate, but Alice's
           per-author seq gate drops it: no re-activation, no ack. */
        cJSON *dup=raw_recv(&bob,10000);
        const cJSON *t=dup?cJSON_GetObjectItemCaseSensitive(dup,"t"):NULL;
        CHECK(dup && cJSON_IsString(t) && !strcmp(t->valuestring,"eval"),
            "dup rebroadcast");
        cJSON_Delete(dup);
        {
            cJSON *extra=raw_recv(&bob,1500);
            if (extra) {
                char *dump=cJSON_PrintUnformatted(extra);
                fprintf(stderr,"unexpected frame after dup: %s\n",
                    dump?dump:"null");
                free(dump);
                cJSON_Delete(extra);
            }
            CHECK(extra==NULL,"no second ack");
        }
    }
    free(bob_good_enc);
    curl_easy_cleanup(bob.handle);
    collab_disconnect();
    sodium_memzero(bob_sk,sizeof(bob_sk));
    puts("collab flow ok");
    return 0;
}
