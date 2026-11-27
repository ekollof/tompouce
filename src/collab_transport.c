#include "collab_transport.h"
#include <curl/curl.h>
#include <poll.h>
#include <stdio.h>
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
static int ctr_host_is_loopback(const char *host, size_t len) {
    size_t i;
    if (len==9 && strncmp(host,"localhost",9)==0) return 1;
    if (len==3 && strncmp(host,"::1",3)==0) return 1;
    /* 127.0.0.0/8, numeric only: anything else is not loopback. */
    if (len<7 || len>15 || strncmp(host,"127.",4)!=0) return 0;
    for (i=4;i<len;i++) {
        if (host[i]=='.') continue;
        if (host[i]<'0' || host[i]>'9') return 0;
    }
    return 1;
}
/* Strict dotted quad; -1 on any deviation. */
static int ctr_parse_ipv4(const char *host, size_t len, uint8_t out[4]) {
    unsigned i, parts=0, value=0, digits=0;
    for (i=0;i<len;i++) {
        if (host[i]=='.') {
            if (!digits || parts>=3) return -1;
            out[parts++]=(uint8_t)value;
            value=0;
            digits=0;
        } else if (host[i]>='0' && host[i]<='9') {
            value=value*10+(unsigned)(host[i]-'0');
            if (++digits>3 || value>255) return -1;
        } else {
            return -1;
        }
    }
    if (!digits || parts!=3) return -1;
    out[3]=(uint8_t)value;
    return 0;
}
/* First hextet of a bracket-stripped IPv6 literal; -1 if unparsable. */
static int ctr_ipv6_first(const char *host, size_t len, unsigned *first) {
    unsigned i, value=0, digits=0;
    for (i=0;i<len;i++) {
        char c=host[i];
        unsigned d;
        if (c==':') break;
        if (c>='0' && c<='9') d=(unsigned)(c-'0');
        else if (c>='a' && c<='f') d=(unsigned)(c-'a')+10;
        else if (c>='A' && c<='F') d=(unsigned)(c-'A')+10;
        else return -1;
        value=value*16+d;
        if (++digits>4 || value>0xffffu) return -1;
    }
    if (!digits) return -1;
    *first=value;
    return 0;
}
/* RFC 1918, IPv4/IPv6 link-local. Anything else is "the internet". */
static int ctr_host_is_private(const char *host, size_t len, int bracketed) {
    uint8_t ip[4];
    unsigned first;
    if (bracketed) {
        if (len==3 && strncmp(host,"::1",3)==0) return 0;
        if (ctr_ipv6_first(host,len,&first)!=0) return 0;
        return first>=0xfe80u && first<=0xfebfu; /* fe80::/10 */
    }
    if (ctr_parse_ipv4(host,len,ip)!=0) return 0;
    if (ip[0]==10) return 1; /* 10.0.0.0/8 */
    if (ip[0]==172 && ip[1]>=16 && ip[1]<=31) return 1; /* 172.16.0.0/12 */
    if (ip[0]==192 && ip[1]==168) return 1; /* 192.168.0.0/16 */
    if (ip[0]==169 && ip[1]==254) return 1; /* 169.254.0.0/16 */
    return 0;
}
/* ws:// is plaintext: allow it on loopback and LAN (RFC 1918/link-local)
   addresses, or anywhere with the explicit Phase 1/1.5 opt-in
   (section 18.3). wss:// may go anywhere. */
int ctr_ws_policy_ok(const char *url, int *insecure_used, char *err,
    size_t errlen) {
    const char *auth, *end, *at, *host;
    size_t hostlen;
    int bracketed;
    if (insecure_used) *insecure_used=0;
    if (!url || !url[0]) {
        ctr_fail(err,errlen,"empty url");
        return 0;
    }
    if (strncmp(url,"wss://",6)==0) {
        return 1;
    }
    if (strncmp(url,"ws://",5)!=0) {
        ctr_fail(err,errlen,"collab url must start with ws:// or wss://");
        return 0;
    }
    auth=url+5;
    end=auth+strcspn(auth,"/?#");
    at=auth;
    while (at<end) {
        if (*at=='@') {
            auth=at+1;
            break;
        }
        at++;
    }
    host=auth;
    bracketed=0;
    if (*host=='[') {
        const char *close=host+1;
        while (close<end && *close!=']') close++;
        if (close>=end) {
            ctr_fail(err,errlen,"collab url has a bad IPv6 host");
            return 0;
        }
        host++;
        hostlen=(size_t)(close-host);
        bracketed=1;
    } else {
        const char *colon=host;
        while (colon<end && *colon!=':') colon++;
        hostlen=(size_t)(colon-host);
    }
    if (hostlen==0) {
        ctr_fail(err,errlen,"collab url has no host");
        return 0;
    }
    if (ctr_host_is_loopback(host,hostlen)) return 1;
    if (ctr_host_is_private(host,hostlen,bracketed)) return 1;
    {
        const char *flag=getenv("TOMPOUCE_COLLAB_INSECURE");
        if (flag && strcmp(flag,"1")==0) {
            if (insecure_used) *insecure_used=1;
            return 1;
        }
    }
    ctr_fail(err,errlen,"refusing plaintext ws:// to a non-LAN host (use wss://, or TOMPOUCE_COLLAB_INSECURE=1 to override)");
    return 0;
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
    {
        int insecure_used=0;
        if (!ctr_ws_policy_ok(url,&insecure_used,err,errlen)) return NULL;
        if (insecure_used)
            fprintf(stderr,"collab: warning: plaintext ws:// to a non-loopback host (TOMPOUCE_COLLAB_INSECURE=1)\n");
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
