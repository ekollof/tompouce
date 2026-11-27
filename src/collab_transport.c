#include "collab_transport.h"
#include <curl/curl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#define CTR_HANDSHAKE_TIMEOUT_S 10L
#define CTR_IO_TIMEOUT_S 10
#define CTR_FRAME_MAX (16u*1024u*1024u)
struct CollabTransport {
    CURL *handle;
    struct curl_slist *headers;
};
static uint64_t ctr_now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC,&now);
    return (uint64_t)now.tv_sec*1000u+(uint64_t)now.tv_nsec/1000000u;
}
static void ctr_fail(char *err, size_t errlen, const char *what) {
    if (!err || errlen==0) return;
    snprintf(err,errlen,"collab: %s",what);
    err[errlen-1]=0;
}
int ctr_websocket_supported(char *err, size_t errlen) {
    curl_version_info_data *info=curl_version_info(CURLVERSION_NOW);
    unsigned long version=0;
    int wss=0;
    const char *const *proto;
    if (!info) {
        ctr_fail(err,errlen,"no curl version info");
        return 0;
    }
    version=(unsigned long)info->version_num;
    if (version<0x080b00ul) {
        ctr_fail(err,errlen,"collab needs libcurl 8.11 or newer with WebSocket support");
        return 0;
    }
    for (proto=info->protocols; proto && *proto; proto++) {
        if (strcmp(*proto,"wss")==0) {
            wss=1;
            break;
        }
    }
    if (!wss) {
        ctr_fail(err,errlen,"collab needs libcurl with WebSocket support (wss missing)");
        return 0;
    }
    return 1;
}
CollabTransport *ctr_open(const char *url, const char *subprotocol,
    char *err, size_t errlen) {
    CollabTransport *ct=NULL;
    CURL *handle=NULL;
    struct curl_slist *headers=NULL;
    char header[128];
    CURLcode code;
    curl_socket_t sock=CURL_SOCKET_BAD;
    struct timeval timeout;
    if (!url || !url[0] || !subprotocol || !subprotocol[0]) {
        ctr_fail(err,errlen,"empty url or subprotocol");
        return NULL;
    }
    if (strncmp(url,"ws://",5)!=0 && strncmp(url,"wss://",6)!=0) {
        ctr_fail(err,errlen,"collab url must start with ws:// or wss://");
        return NULL;
    }
    if (strlen(subprotocol)>64) {
        ctr_fail(err,errlen,"subprotocol too long");
        return NULL;
    }
    if (!ctr_websocket_supported(err,errlen)) return NULL;
    handle=curl_easy_init();
    if (!handle) {
        ctr_fail(err,errlen,"curl init failed");
        return NULL;
    }
    snprintf(header,sizeof(header),"Sec-WebSocket-Protocol: %s",subprotocol);
    headers=curl_slist_append(headers,header);
    if (!headers) {
        curl_easy_cleanup(handle);
        ctr_fail(err,errlen,"header build failed");
        return NULL;
    }
    ct=calloc(1,sizeof(*ct));
    if (!ct) {
        curl_slist_free_all(headers);
        curl_easy_cleanup(handle);
        ctr_fail(err,errlen,"out of memory");
        return NULL;
    }
    ct->handle=handle;
    ct->headers=headers;
    curl_easy_setopt(handle,CURLOPT_URL,url);
    curl_easy_setopt(handle,CURLOPT_CONNECT_ONLY,2L);
    curl_easy_setopt(handle,CURLOPT_HTTPHEADER,headers);
    curl_easy_setopt(handle,CURLOPT_TIMEOUT,CTR_HANDSHAKE_TIMEOUT_S);
    code=curl_easy_perform(handle);
    if (code!=CURLE_OK) {
        ctr_fail(err,errlen,curl_easy_strerror(code));
        ctr_close(ct);
        return NULL;
    }
    curl_easy_setopt(handle,CURLOPT_TIMEOUT,0L);
    /* Blocking reads with a kernel timeout: libcurl may already hold
       complete frames (coalesced sends), which poll() on the socket cannot
       see. Never pre-poll; just read. */
    if (curl_easy_getinfo(handle,CURLINFO_ACTIVESOCKET,&sock)!=CURLE_OK ||
        sock==CURL_SOCKET_BAD) {
        ctr_fail(err,errlen,"no active socket after handshake");
        ctr_close(ct);
        return NULL;
    }
    timeout.tv_sec=CTR_IO_TIMEOUT_S;
    timeout.tv_usec=0;
    setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    setsockopt(sock,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    return ct;
}
static int ctr_send_all(CollabTransport *ct, const char *data, size_t len,
    int binary) {
    size_t sent=0;
    if (!ct || (!data && len>0)) return 0;
    while (sent<len) {
        size_t chunk=0;
        CURLcode code=curl_ws_send(ct->handle,data+sent,len-sent,&chunk,0,
            binary?CURLWS_BINARY:CURLWS_TEXT);
        if (code!=CURLE_OK || chunk==0) return 0;
        sent+=chunk;
    }
    return 1;
}
int ctr_send_text(CollabTransport *ct, const char *text, size_t len) {
    if (!ct || !text) return 0;
    if (len==0) len=strlen(text);
    if (len>CTR_FRAME_MAX) return 0;
    return ctr_send_all(ct,text,len,0);
}
int ctr_send_binary(CollabTransport *ct, const uint8_t *data, size_t len) {
    if (!ct || !data || len==0 || len>CTR_FRAME_MAX) return 0;
    return ctr_send_all(ct,(const char *)data,len,1);
}
static int ctr_wait(CollabTransport *ct, int timeout_ms) {
    curl_socket_t sock=CURL_SOCKET_BAD;
    struct pollfd fd;
    int ready;
    if (curl_easy_getinfo(ct->handle,CURLINFO_ACTIVESOCKET,&sock)!=CURLE_OK)
        return 0;
    if (sock==CURL_SOCKET_BAD) return 0;
    fd.fd=sock;
    fd.events=POLLIN;
    fd.revents=0;
    ready=poll(&fd,1,timeout_ms);
    return ready>0 && (fd.revents&POLLIN);
}
int ctr_recv(CollabTransport *ct, int *is_binary, uint8_t **out,
    size_t *outlen, int timeout_ms) {
    uint8_t *buffer=NULL;
    size_t length=0, capacity=0;
    int binary=0, ok=0;
    uint64_t deadline;
    if (!ct || !is_binary || !out || !outlen) return 0;
    if (timeout_ms<0) timeout_ms=CTR_IO_TIMEOUT_S*1000;
    deadline=ctr_now_ms()+(uint64_t)(unsigned)timeout_ms;
    for (;;) {
        char chunk[16384];
        size_t got=0;
        const struct curl_ws_frame *meta=NULL;
        CURLcode code=curl_ws_recv(ct->handle,chunk,sizeof(chunk),&got,&meta);
        if (code==CURLE_AGAIN) {
            int64_t left;
            if (ctr_now_ms()>=deadline) break;
            left=(int64_t)(deadline-ctr_now_ms());
            if (!ctr_wait(ct,left>60000?60000:(int)left)) continue;
            continue;
        }
        if (code!=CURLE_OK) break;
        if (meta && (meta->flags&CURLWS_CLOSE)) break;
        if (meta && (meta->flags&CURLWS_BINARY)) binary=1;
        if (got>0) {
            uint8_t *grown;
            size_t next;
            if (length+got>CTR_FRAME_MAX || length+got<length) break;
            next=capacity?capacity:4096;
            while (next<length+got) {
                if (next>CTR_FRAME_MAX/2) {
                    next=CTR_FRAME_MAX;
                    break;
                }
                next*=2;
            }
            if (next<length+got) break;
            grown=realloc(buffer,next);
            if (!grown) break;
            buffer=grown;
            capacity=next;
            memcpy(buffer+length,chunk,got);
            length+=got;
        }
        if (meta && !(meta->flags&CURLWS_CONT)) {
            ok=1;
            break;
        }
        /* No complete frame yet (or another fragment follows): read again. */
    }
    if (!ok) {
        free(buffer);
        return 0;
    }
    *is_binary=binary;
    *out=buffer;
    *outlen=length;
    return 1;
}
int ctr_fd(CollabTransport *ct) {
    curl_socket_t sock=CURL_SOCKET_BAD;
    if (!ct) return -1;
    if (curl_easy_getinfo(ct->handle,CURLINFO_ACTIVESOCKET,&sock)!=CURLE_OK)
        return -1;
    if (sock==CURL_SOCKET_BAD) return -1;
    return (int)sock;
}
void ctr_close(CollabTransport *ct) {
    if (!ct) return;
    if (ct->handle) curl_easy_cleanup(ct->handle);
    if (ct->headers) curl_slist_free_all(ct->headers);
    free(ct);
}
