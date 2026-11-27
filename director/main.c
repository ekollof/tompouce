/* Standalone collab director: single binary, no engine linkage.
   Speaks tompouce.collab.v1 over WebSocket; see docs/multiplayer.md. */
#include "collab_proto.h"
#include "session.h"
#include <libwebsockets.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define DIRECTOR_DEFAULT_PORT 7420
#define DIRECTOR_VERSION "0.1.0"
#define DIRECTOR_SOURCE "https://github.com/ekollof/tompouce"
static volatile int stop_service=0;
static DirectorConfig config;
static void on_signal(int signum) {
    (void)signum;
    stop_service=1;
}
static int read_admission_key(const char *path, uint8_t *key) {
    FILE *file=fopen(path,"rb");
    size_t got;
    if (!file) {
        fprintf(stderr,"tompouce-director: cannot open %s\n",path);
        return 0;
    }
    got=fread(key,1,32,file);
    fclose(file);
    if (got!=32) {
        fprintf(stderr,"tompouce-director: admission key must be 32 bytes\n");
        return 0;
    }
    return 1;
}
static void usage(const char *program) {
    fprintf(stderr,
        "usage: %s [--listen [HOST:]PORT] [--admission-key-file PATH]\n"
        "       [--allow-plaintext] [--version]\n"
        "\n"
        "  --listen            bind address, default 127.0.0.1:7420\n"
        "  --admission-key-file  32-byte admission key for LAN bootstrap\n"
        "                      (crypto_kdf of the room key, context \"tpcadm01\";\n"
        "                      the director never sees the room key itself)\n"
        "  --allow-plaintext   accept binary frames with the plaintext bit\n"
        "  --version           print the version and exit\n",
        program);
}
static int callback(struct lws *wsi, enum lws_callback_reasons reason,
    void *user, void *in, size_t len) {
    DirectorSession *session;
    (void)user;
    switch (reason) {
    case LWS_CALLBACK_ESTABLISHED:
        session=director_session_create(wsi);
        if (!session) {
            unsigned char busy[]="busy";
            lws_close_reason(wsi,(enum lws_close_status)1013,busy,
                sizeof(busy)-1);
            return -1;
        }
        break;
    case LWS_CALLBACK_RECEIVE:
        session=director_session_lookup(wsi);
        if (!session) return -1;
        director_session_receive(session,(const uint8_t *)in,len,
            lws_frame_is_binary(wsi),lws_is_first_fragment(wsi),
            lws_is_final_fragment(wsi),&config);
        break;
    case LWS_CALLBACK_SERVER_WRITEABLE:
        session=director_session_lookup(wsi);
        if (!session) return -1;
        director_session_flush(session);
        break;
    case LWS_CALLBACK_CLOSED:
        session=director_session_lookup(wsi);
        if (session) {
            director_session_closed(session,1);
            director_session_free(session);
        }
        break;
    default:
        break;
    }
    return 0;
}
static struct lws_protocols protocols[2];
static void protocols_init(void) {
    /* Field assignment (not an initializer list): struct lws_protocols
       grows across libwebsockets versions and static storage zeroes the
       rest, which keeps -Werror=missing-field-initializers useful. */
    protocols[0].name=CP_SUBPROTOCOL;
    protocols[0].callback=callback;
    protocols[0].rx_buffer_size=16384;
}
int main(int argc, char **argv) {
    struct lws_context_creation_info info;
    struct lws_context *context;
    const char *listen_host="127.0.0.1";
    int listen_port=DIRECTOR_DEFAULT_PORT;
    uint64_t last_sweep=0;
    int i;
    memset(&config,0,sizeof(config));
    config.server_name="tompouce-director";
    config.server_version=DIRECTOR_VERSION;
    config.server_source=DIRECTOR_SOURCE;
    for (i=1;i<argc;i++) {
        if (!strcmp(argv[i],"--help") || !strcmp(argv[i],"-h")) {
            usage(argv[0]);
            return 0;
        }
        if (!strcmp(argv[i],"--version")) {
            printf("tompouce-director %s\n",DIRECTOR_VERSION);
            return 0;
        }
        if (!strcmp(argv[i],"--allow-plaintext")) {
            config.allow_plaintext=1;
            continue;
        }
        if (!strcmp(argv[i],"--admission-key-file") && i+1<argc) {
            if (!read_admission_key(argv[++i],config.admission_key)) return 1;
            config.has_admission_key=1;
            continue;
        }
        if (!strcmp(argv[i],"--listen") && i+1<argc) {
            const char *value=argv[++i], *colon=strrchr(value,':');
            if (colon && colon!=value) {
                size_t host_len=(size_t)(colon-value);
                static char host[256];
                if (host_len>=sizeof(host)) {
                    fprintf(stderr,"tompouce-director: host too long\n");
                    return 1;
                }
                memcpy(host,value,host_len);
                host[host_len]=0;
                listen_host=host;
                listen_port=atoi(colon+1);
            } else if (colon) {
                listen_host=NULL;
                listen_port=atoi(colon+1);
            } else {
                listen_port=atoi(value);
            }
            if (listen_port<=0 || listen_port>65535) {
                fprintf(stderr,"tompouce-director: bad port\n");
                return 1;
            }
            continue;
        }
        fprintf(stderr,"tompouce-director: unknown argument %s\n",argv[i]);
        usage(argv[0]);
        return 1;
    }
    signal(SIGINT,on_signal);
    signal(SIGTERM,on_signal);
    memset(&info,0,sizeof(info));
    info.port=listen_port;
    info.iface=listen_host;
    info.protocols=protocols;
    protocols_init();
    director_sessions_init();
    context=lws_create_context(&info);
    if (!context) {
        fprintf(stderr,"tompouce-director: cannot listen on %s:%d\n",
            listen_host,listen_port);
        return 1;
    }
    fprintf(stderr,"tompouce-director: listening on %s:%d\n",
        listen_host?listen_host:"*",listen_port);
    while (!stop_service) {
        uint64_t now;
        lws_service(context,50);
        now=director_now_us();
        if (now-last_sweep>1000000u) {
            last_sweep=now;
            director_session_sweep(now,&config);
        }
    }
    lws_context_destroy(context);
    return 0;
}
