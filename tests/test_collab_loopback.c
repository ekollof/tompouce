/* Loopback integration: two scripted clients against a real director.
   The director binary is spawned on an ephemeral loopback port; when it is
   missing the test skips (TOMPOUCE_DIRECTOR_BIN overrides the path). */
#include "../src/collab_proto.c"
#include <arpa/inet.h>
#include <assert.h>
#include <cjson/cJSON.h>
#include <curl/curl.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <sodium.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#define TEST_ROOM "testroomabcdefghijklmnopqr"
#define TEST_CPS 0.5
typedef struct {
    CURL *handle;
    struct curl_slist *headers;
    uint8_t pk[32];
    uint8_t sk[64];
    char *resume;
} TestClient;
static pid_t test_child=-1;
static char test_keypath[64]="";
static void test_cleanup(void) {
    if (test_child>0) {
        kill(test_child,15);
        waitpid(test_child,NULL,0);
        test_child=-1;
    }
    if (test_keypath[0]) {
        unlink(test_keypath);
        test_keypath[0]=0;
    }
}
static void check(int ok, const char *what, int line) {
    if (!ok) {
        fprintf(stderr,"collab loopback FAILED at line %d: %s\n",line,what);
        exit(1);
    }
}
#define CHECK(ok, what) check(!!(ok), what, __LINE__)
static void b64e(const uint8_t *in, size_t len, char *out, size_t cap) {
    CHECK(cp_b64_encode(in,len,out,cap)>0 || len==0,"b64 encode");
}
static CURL *ws_open(const char *url) {
    CURL *handle=curl_easy_init();
    struct curl_slist *headers=NULL;
    CURLcode code;
    if (!handle) return NULL;
    headers=curl_slist_append(headers,
        "Sec-WebSocket-Protocol: tompouce.collab.v1");
    if (!headers) {
        curl_easy_cleanup(handle);
        return NULL;
    }
    curl_easy_setopt(handle,CURLOPT_URL,url);
    curl_easy_setopt(handle,CURLOPT_CONNECT_ONLY,2L);
    curl_easy_setopt(handle,CURLOPT_HTTPHEADER,headers);
    curl_easy_setopt(handle,CURLOPT_TIMEOUT,10L);
    code=curl_easy_perform(handle);
    if (code!=CURLE_OK) {
        fprintf(stderr,"connect %s: %s\n",url,curl_easy_strerror(code));
        curl_slist_free_all(headers);
        curl_easy_cleanup(handle);
        return NULL;
    }
    curl_easy_setopt(handle,CURLOPT_TIMEOUT,0L);
    {
        /* Blocking reads with a kernel timeout: libcurl may already hold
           complete frames (coalesced sends), which poll() on the socket
           cannot see. Never pre-poll; just read. */
        curl_socket_t sock=CURL_SOCKET_BAD;
        struct timeval timeout;
        if (curl_easy_getinfo(handle,CURLINFO_ACTIVESOCKET,&sock)!=CURLE_OK ||
            sock==CURL_SOCKET_BAD) {
            curl_easy_cleanup(handle);
            return NULL;
        }
        timeout.tv_sec=10;
        timeout.tv_usec=0;
        setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
        setsockopt(sock,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    }
    return handle;
}
static int ws_send(CURL *handle, const void *data, size_t len, int binary) {
    size_t sent=0;
    while (sent<len) {
        size_t chunk=0;
        CURLcode code=curl_ws_send(handle,(const char *)data+sent,len-sent,
            &chunk,0,binary?CURLWS_BINARY:CURLWS_TEXT);
        if (code!=CURLE_OK) return 0;
        if (chunk==0) return 0;
        sent+=chunk;
    }
    return 1;
}
/* One complete incoming frame; binary flag and malloc'd payload out. */
/* One complete incoming frame; binary flag and malloc'd payload out.
   libcurl holds its own readahead, so a poll-then-read scheme misses
   frames that are already buffered: always try reading first and only
   poll when the read would block. Bounded by an overall deadline. */
static uint64_t test_now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC,&now);
    return (uint64_t)now.tv_sec*1000u+(uint64_t)now.tv_nsec/1000000u;
}
static int ws_wait(CURL *handle, int timeout_ms) {
    curl_socket_t sock=CURL_SOCKET_BAD;
    struct pollfd fd;
    int ready;
    if (curl_easy_getinfo(handle,CURLINFO_ACTIVESOCKET,&sock)!=CURLE_OK)
        return 0;
    if (sock==CURL_SOCKET_BAD) return 0;
    fd.fd=sock;
    fd.events=POLLIN;
    fd.revents=0;
    ready=poll(&fd,1,timeout_ms);
    return ready>0 && (fd.revents&POLLIN);
}
static int ws_recv(CURL *handle, int *is_binary, uint8_t **out,
    size_t *outlen, int timeout_ms) {
    uint8_t *buffer=NULL;
    size_t length=0, capacity=0;
    int binary=0;
    uint64_t deadline=test_now_ms()+(uint64_t)(timeout_ms<0?0:timeout_ms);
    for (;;) {
        char chunk[16384];
        size_t got=0;
        const struct curl_ws_frame *meta=NULL;
        CURLcode code=curl_ws_recv(handle,chunk,sizeof(chunk),&got,&meta);
        if (code==CURLE_AGAIN) {
            int64_t left;
            if (test_now_ms()>=deadline) {
                free(buffer);
                return 0;
            }
            left=(int64_t)(deadline-test_now_ms());
            if (!ws_wait(handle,left>60000?60000:(int)left)) continue;
            continue;
        }
        if (code!=CURLE_OK) {
            free(buffer);
            return 0;
        }
        if (meta && (meta->flags&CURLWS_CLOSE)) {
            free(buffer);
            return 0;
        }
        if (meta && (meta->flags&CURLWS_BINARY)) binary=1;
        if (got>0) {
            if (length+got>16u*1024u*1024u) {
                free(buffer);
                return 0;
            }
            if (length+got>capacity) {
                size_t next=capacity?capacity*2:4096;
                uint8_t *grown;
                while (next<length+got) next*=2;
                grown=realloc(buffer,next);
                if (!grown) {
                    free(buffer);
                    return 0;
                }
                buffer=grown;
                capacity=next;
            }
            memcpy(buffer+length,chunk,got);
            length+=got;
        }
        if (meta && !(meta->flags&CURLWS_CONT)) break;
        /* No complete frame yet (or another fragment follows): poll again. */
    }
    *is_binary=binary;
    *out=buffer;
    *outlen=length;
    return 1;
}
static cJSON *recv_json(CURL *handle, int *binary_out) {
    uint8_t *payload=NULL;
    size_t length=0;
    cJSON *obj;
    int binary=0;
    if (!ws_recv(handle,&binary,&payload,&length,10000)) return NULL;
    if (binary_out) *binary_out=binary;
    if (binary || !payload) {
        free(payload);
        return NULL;
    }
    obj=cJSON_ParseWithLength((const char *)payload,length);
    free(payload);
    return obj;
}
static const char *require_string(const cJSON *obj, const char *key) {
    const cJSON *item=cJSON_GetObjectItemCaseSensitive(obj,key);
    CHECK(cJSON_IsString(item) && item->valuestring,key);
    return item->valuestring;
}
static cJSON *expect_type(CURL *handle, const char *type) {
    cJSON *obj;
    int tries;
    for (tries=0;tries<20;tries++) {
        obj=recv_json(handle,NULL);
        CHECK(obj!=NULL,"frame");
        if (!strcmp(require_string(obj,"t"),type)) return obj;
        cJSON_Delete(obj);
    }
    fprintf(stderr,"never saw %s\n",type);
    exit(1);
}
static void client_new(TestClient *client, const char *url) {
    memset(client,0,sizeof(*client));
    client->handle=ws_open(url);
    CHECK(client->handle!=NULL,"connect");
    crypto_sign_keypair(client->pk,client->sk);
}
static void client_close(TestClient *client) {
    if (client->handle) curl_easy_cleanup(client->handle);
    if (client->headers) curl_slist_free_all(client->headers);
    free(client->resume);
    memset(client,0,sizeof(*client));
}
static void send_json(CURL *handle, cJSON *obj) {
    char *text=cJSON_PrintUnformatted(obj);
    int ok;
    CHECK(text!=NULL,"print");
    ok=ws_send(handle,text,strlen(text),0);
    free(text);
    CHECK(ok,"send");
    cJSON_Delete(obj);
}
int main(void) {
    char url[64], keypath[]="/tmp/tompouce-collab-key-XXXXXX";
    const char *binary;
    int keyfd, port, attempt;
    pid_t child=-1;
    uint8_t room_key[32];
    TestClient alice, bob;
    cJSON *msg;
    const char *challenge_b64;
    uint8_t challenge[32];
    size_t challenge_len=0;
    char pk_b64[64], sig_b64[128], proof_b64[64];
    uint8_t sig[64], proof[32], mac_msg[32+26];
    uint64_t eval_seq=0;
    int found_binary=0, found_sync=0;
    if (sodium_init()<0) {
        puts("collab loopback skipped: libsodium unavailable");
        return 0;
    }
    binary=getenv("TOMPOUCE_DIRECTOR_BIN");
    if (!binary || !*binary) binary="./build/tompouce-director";
    if (access(binary,X_OK)!=0) {
        puts("collab loopback skipped: no director binary");
        return 0;
    }
    if (atexit(test_cleanup)!=0) return 1;
    keyfd=mkstemp(keypath);
    CHECK(keyfd>=0,"mkstemp");
    memset(room_key,0x5a,sizeof(room_key));
    CHECK(write(keyfd,room_key,sizeof(room_key))==sizeof(room_key),"key");
    close(keyfd);
    for (attempt=0;attempt<20;attempt++) {
        pid_t pid;
        CURL *probe;
        int probe_fd;
        struct sockaddr_in probe_addr;
        port=17429+attempt;
        /* Only talk to a director we spawned ourselves: skip ports that
           are already bound (e.g. leftovers from a killed run). */
        probe_fd=socket(AF_INET,SOCK_STREAM,0);
        if (probe_fd<0) continue;
        memset(&probe_addr,0,sizeof(probe_addr));
        probe_addr.sin_family=AF_INET;
        probe_addr.sin_port=htons((uint16_t)port);
        probe_addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        if (bind(probe_fd,(struct sockaddr *)&probe_addr,
            sizeof(probe_addr))!=0) {
            close(probe_fd);
            continue;
        }
        close(probe_fd);
        pid=fork();
        CHECK(pid>=0,"fork");
        if (pid==0) {
            static char addr[64];
            snprintf(addr,sizeof(addr),"127.0.0.1:%d",17429+attempt);
            execl(binary,"tompouce-director","--listen",addr,
                "--room-key-file",keypath,(char *)NULL);
            _exit(127);
        }
        snprintf(url,sizeof(url),"ws://127.0.0.1:%d/v1",port);
        probe=curl_easy_init();
        if (probe) {
            int tries;
            curl_easy_setopt(probe,CURLOPT_URL,url);
            curl_easy_setopt(probe,CURLOPT_CONNECT_ONLY,2L);
            for (tries=0;tries<50;tries++) {
                struct curl_slist *hdrs=NULL;
                hdrs=curl_slist_append(hdrs,
                    "Sec-WebSocket-Protocol: tompouce.collab.v1");
                curl_easy_setopt(probe,CURLOPT_HTTPHEADER,hdrs);
                curl_easy_setopt(probe,CURLOPT_TIMEOUT_MS,200L);
                if (curl_easy_perform(probe)==CURLE_OK) {
                    curl_slist_free_all(hdrs);
                    curl_easy_cleanup(probe);
                    probe=NULL;
                    break;
                }
                curl_slist_free_all(hdrs);
                curl_easy_setopt(probe,CURLOPT_HTTPHEADER,NULL);
                usleep(100000);
            }
            if (probe) curl_easy_cleanup(probe);
            else {
                child=pid;
                break;
            }
        }
        kill(pid,9);
        waitpid(pid,NULL,0);
    }
    CHECK(child>0,"director spawn");
    test_child=child;
    {
        size_t n=strlen(keypath);
        if (n<sizeof(test_keypath)) {
            memcpy(test_keypath,keypath,n+1);
        }
    }
    /* Alice creates the room as owner. */
    snprintf(url,sizeof(url),"ws://127.0.0.1:%d/v1",port);
    client_new(&alice,url);
    {
        cJSON *hello=cJSON_CreateObject();
        cJSON *proto=cJSON_CreateArray();
        cJSON *caps=cJSON_CreateArray();
        cJSON_AddStringToObject(hello,"t","hello");
        cJSON_AddItemToArray(proto,cJSON_CreateNumber(1));
        cJSON_AddItemToObject(hello,"proto",proto);
        cJSON_AddItemToArray(caps,cJSON_CreateString("e2e"));
        cJSON_AddItemToObject(hello,"caps",caps);
        send_json(alice.handle,hello);
    }
    msg=expect_type(alice.handle,"welcome");
    challenge_b64=require_string(msg,"challenge");
    CHECK(cp_b64_decode(challenge_b64,challenge,sizeof(challenge),
        &challenge_len) && challenge_len==32,"challenge");
    cJSON_Delete(msg);
    {
        uint8_t m[18+32+26+32];
        cJSON *auth=cJSON_CreateObject();
        memcpy(m,"tompouce-collab-v1",18);
        memcpy(m+18,challenge,32);
        memcpy(m+18+32,TEST_ROOM,26);
        memcpy(m+18+32+26,alice.pk,32);
        CHECK(crypto_sign_detached(sig,NULL,m,sizeof(m),alice.sk)==0,
            "sign");
        b64e(alice.pk,32,pk_b64,sizeof(pk_b64));
        b64e(sig,64,sig_b64,sizeof(sig_b64));
        cJSON_AddStringToObject(auth,"t","auth");
        cJSON_AddNumberToObject(auth,"id",1);
        cJSON_AddStringToObject(auth,"room",TEST_ROOM);
        cJSON_AddStringToObject(auth,"pk",pk_b64);
        cJSON_AddStringToObject(auth,"sig",sig_b64);
        cJSON_AddTrueToObject(auth,"owner");
        send_json(alice.handle,auth);
    }
    msg=expect_type(alice.handle,"auth_ok");
    {
        const char *resume=require_string(msg,"resume");
        alice.resume=malloc(strlen(resume)+1);
        CHECK(alice.resume!=NULL,"resume");
        memcpy(alice.resume,resume,strlen(resume)+1);
    }
    cJSON_Delete(msg);
    {
        cJSON *join=cJSON_CreateObject();
        cJSON_AddStringToObject(join,"t","join");
        cJSON_AddStringToObject(join,"room",TEST_ROOM);
        cJSON_AddStringToObject(join,"profile","e30");
        send_json(alice.handle,join);
    }
    msg=expect_type(alice.handle,"joined");
    {
        const cJSON *you=cJSON_GetObjectItemCaseSensitive(msg,"you");
        const cJSON *member, *role;
        CHECK(cJSON_IsObject(you),"you");
        member=cJSON_GetObjectItemCaseSensitive(you,"member");
        role=cJSON_GetObjectItemCaseSensitive(you,"role");
        CHECK(cJSON_IsNumber(member) && member->valuedouble==1,"you.member");
        CHECK(cJSON_IsString(role) && !strcmp(role->valuestring,"owner"),
            "you.role");
    }
    cJSON_Delete(msg);
    for (int i=0;i<4;i++) {
        cJSON *frame=recv_json(alice.handle,NULL);
        const char *t;
        CHECK(frame!=NULL,"drain");
        t=require_string(frame,"t");
        if (!strcmp(t,"synced")) {
            found_sync=1;
            cJSON_Delete(frame);
            break;
        }
        cJSON_Delete(frame);
    }
    CHECK(found_sync,"synced");
    /* Bob joins with the preshared room key. */
    client_new(&bob,url);
    {
        cJSON *hello=cJSON_CreateObject();
        cJSON *proto=cJSON_CreateArray();
        cJSON *caps=cJSON_CreateArray();
        cJSON_AddStringToObject(hello,"t","hello");
        cJSON_AddItemToArray(proto,cJSON_CreateNumber(1));
        cJSON_AddItemToObject(hello,"proto",proto);
        cJSON_AddItemToArray(caps,cJSON_CreateString("e2e"));
        cJSON_AddItemToObject(hello,"caps",caps);
        send_json(bob.handle,hello);
    }
    msg=expect_type(bob.handle,"welcome");
    challenge_b64=require_string(msg,"challenge");
    CHECK(cp_b64_decode(challenge_b64,challenge,sizeof(challenge),
        &challenge_len) && challenge_len==32,"challenge");
    cJSON_Delete(msg);
    memcpy(mac_msg,challenge,32);
    memcpy(mac_msg+32,TEST_ROOM,26);
    {
        uint8_t m[18+32+26+32];
        cJSON *auth=cJSON_CreateObject();
        memcpy(m,"tompouce-collab-v1",18);
        memcpy(m+18,challenge,32);
        memcpy(m+18+32,TEST_ROOM,26);
        memcpy(m+18+32+26,bob.pk,32);
        CHECK(crypto_sign_detached(sig,NULL,m,sizeof(m),bob.sk)==0,
            "sign");
        CHECK(crypto_generichash(proof,sizeof(proof),mac_msg,sizeof(mac_msg),
            room_key,sizeof(room_key))==0,"proof");
        b64e(bob.pk,32,pk_b64,sizeof(pk_b64));
        b64e(sig,64,sig_b64,sizeof(sig_b64));
        b64e(proof,32,proof_b64,sizeof(proof_b64));
        cJSON_AddStringToObject(auth,"t","auth");
        cJSON_AddStringToObject(auth,"room",TEST_ROOM);
        cJSON_AddStringToObject(auth,"pk",pk_b64);
        cJSON_AddStringToObject(auth,"sig",sig_b64);
        cJSON_AddStringToObject(auth,"proof",proof_b64);
        send_json(bob.handle,auth);
    }
    msg=expect_type(bob.handle,"auth_ok");
    cJSON_Delete(msg);
    {
        cJSON *join=cJSON_CreateObject();
        cJSON_AddStringToObject(join,"t","join");
        cJSON_AddStringToObject(join,"room",TEST_ROOM);
        send_json(bob.handle,join);
    }
    msg=expect_type(bob.handle,"joined");
    cJSON_Delete(msg);
    msg=expect_type(bob.handle,"synced");
    cJSON_Delete(msg);
    msg=expect_type(alice.handle,"member_joined");
    cJSON_Delete(msg);
    /* Eval fan-out carries the same instant to both. */
    {
        cJSON *req=cJSON_CreateObject();
        cJSON *a, *b;
        cJSON_AddStringToObject(req,"t","eval_req");
        cJSON_AddNumberToObject(req,"slot",0);
        cJSON_AddStringToObject(req,"enc","e30");
        send_json(alice.handle,req);
        a=expect_type(alice.handle,"eval");
        b=expect_type(bob.handle,"eval");
        eval_seq=(uint64_t)cJSON_GetObjectItemCaseSensitive(a,
            "eval_seq")->valuedouble;
        CHECK(eval_seq==1,"seq");
        CHECK(cJSON_GetObjectItemCaseSensitive(b,
            "eval_seq")->valuedouble==1,"seq b");
        {
            const cJSON *ca=cJSON_GetObjectItemCaseSensitive(a,"at_cycle");
            const cJSON *cb=cJSON_GetObjectItemCaseSensitive(b,"at_cycle");
            CHECK(cJSON_GetArraySize(ca)==2 && cJSON_GetArraySize(cb)==2,
                "cycle pair");
            CHECK(cJSON_GetArrayItem(ca,1)->valuedouble==64,"grid");
            CHECK(cJSON_GetArrayItem(ca,0)->valuedouble==
                cJSON_GetArrayItem(cb,0)->valuedouble,"same instant");
        }
        cJSON_Delete(a);
        cJSON_Delete(b);
    }
    /* Binary doc_update from Bob arrives at Alice with sender rewritten. */
    {
        uint8_t frame[24]={1,1,0,0,0,0,0,2,0,0,0,9,0,0,0,0,0,0,0,4,
            'a','b','c','d'};
        int is_binary=0;
        uint8_t *payload=NULL;
        size_t length=0;
        CHECK(ws_send(bob.handle,frame,sizeof(frame),1),"bin send");
        for (int i=0;i<10 && !found_binary;i++) {
            CHECK(ws_recv(alice.handle,&is_binary,&payload,&length,10000),
                "bin recv");
            if (is_binary && length>=20) {
                uint32_t member=((uint32_t)payload[4]<<24)|
                    ((uint32_t)payload[5]<<16)|
                    ((uint32_t)payload[6]<<8)|payload[7];
                CHECK(member==2,"member rewrite");
                CHECK(length==24 && memcmp(payload+20,"abcd",4)==0,
                    "payload");
                found_binary=1;
            }
            free(payload);
            payload=NULL;
        }
        CHECK(found_binary,"relay");
    }
    /* Ping round trip. */
    {
        cJSON *ping=cJSON_CreateObject(), *pong;
        cJSON_AddStringToObject(ping,"t","ping");
        cJSON_AddNumberToObject(ping,"t0",123456789);
        send_json(alice.handle,ping);
        pong=expect_type(alice.handle,"pong");
        CHECK(cJSON_GetObjectItemCaseSensitive(pong,"t0")->valuedouble==
            123456789,"t0");
        cJSON_Delete(pong);
    }
    /* Bob leaves; Alice sees member_left. */
    {
        cJSON *leave=cJSON_CreateObject(), *left;
        cJSON_AddStringToObject(leave,"t","leave");
        send_json(bob.handle,leave);
        left=expect_type(alice.handle,"member_left");
        cJSON_Delete(left);
    }
    client_close(&alice);
    client_close(&bob);
    printf("collab loopback hello/auth/join/eval/binary/leave ok\n");
    return 0;
}
