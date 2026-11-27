/* Phase 0 collab client core: transport validation, nick handling and a
   hello/welcome handshake against a real director spawned on loopback.
   The loopback section skips (exit 0) when no director binary is around;
   TOMPOUCE_DIRECTOR_BIN overrides the path. */
#include "collab.h"
#include "collab_proto.h"
#include "collab_transport.h"
#include <assert.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
static pid_t test_child=-1;
static void test_cleanup(void) {
    if (test_child>0) {
        kill(test_child,15);
        waitpid(test_child,NULL,0);
        test_child=-1;
    }
}
static void check(int ok, const char *what, int line) {
    if (!ok) {
        fprintf(stderr,"collab transport FAILED at line %d: %s\n",line,what);
        exit(1);
    }
}
#define CHECK(ok, what) check(!!(ok), what, __LINE__)
static void check_nick_ok(const char *name) {
    char err[128]="";
    CHECK(collab_nick_check(name,err,sizeof(err)),name);
}
static void check_nick_bad(const char *name) {
    char err[128]="";
    CHECK(!collab_nick_check(name,err,sizeof(err)),name);
    CHECK(err[0]!=0,name);
}
static int free_port(int base) {
    int attempt;
    for (attempt=0;attempt<20;attempt++) {
        int fd=socket(AF_INET,SOCK_STREAM,0);
        struct sockaddr_in addr;
        if (fd<0) continue;
        memset(&addr,0,sizeof(addr));
        addr.sin_family=AF_INET;
        addr.sin_port=htons((uint16_t)(base+attempt));
        addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        if (bind(fd,(struct sockaddr *)&addr,sizeof(addr))==0) {
            close(fd);
            return base+attempt;
        }
        close(fd);
    }
    return -1;
}
int main(void) {
    char err[256]="";
    char nick[64]="";
    char template[]="/tmp/tompouce-collab-nick-XXXXXX";
    char *tmpdir;
    const char *binary;
    int port;
    pid_t pid;
    CollabConfig cfg;
    /* Offline transport validation. */
    CHECK(ctr_websocket_supported(err,sizeof(err)),"curl wss floor");
    CHECK(ctr_open(NULL,CP_SUBPROTOCOL,err,sizeof(err))==NULL,"null url");
    CHECK(ctr_open("",CP_SUBPROTOCOL,err,sizeof(err))==NULL,"empty url");
    CHECK(ctr_open("http://127.0.0.1:1/",CP_SUBPROTOCOL,err,
        sizeof(err))==NULL,"http scheme refused");
    CHECK(ctr_open("ws://127.0.0.1:1/",NULL,err,sizeof(err))==NULL,
        "null subprotocol");
    CHECK(ctr_open("ws://127.0.0.1:1/",CP_SUBPROTOCOL,err,
        sizeof(err))==NULL,"refused port fails");
    CHECK(err[0]!=0,"error text set");
    /* Plaintext ws:// stays on loopback and LAN; anything else needs the
       explicit opt-in. */
    {
        int insecure=0;
        CHECK(ctr_ws_policy_ok("ws://127.0.0.1:1/",NULL,err,sizeof(err)),"loopback policy");
        CHECK(ctr_ws_policy_ok("ws://localhost:80/x",NULL,NULL,0),"localhost policy");
        CHECK(ctr_ws_policy_ok("ws://[::1]:80/x",NULL,NULL,0),"ipv6 loopback policy");
        CHECK(ctr_ws_policy_ok("ws://192.168.1.20:7420/v1",NULL,NULL,0),"rfc1918 /16");
        CHECK(ctr_ws_policy_ok("ws://10.0.0.5/",NULL,NULL,0),"rfc1918 /8");
        CHECK(ctr_ws_policy_ok("ws://172.16.0.1/",NULL,NULL,0),"rfc1918 /12 low");
        CHECK(ctr_ws_policy_ok("ws://172.31.255.255/",NULL,NULL,0),"rfc1918 /12 high");
        CHECK(ctr_ws_policy_ok("ws://169.254.10.20/",NULL,NULL,0),"link-local v4");
        CHECK(ctr_ws_policy_ok("ws://[fe80::1]/",NULL,NULL,0),"link-local v6");
        CHECK(ctr_ws_policy_ok("wss://example.com:443/v1",NULL,NULL,0),"wss anywhere");
        CHECK(!ctr_ws_policy_ok("ws://8.8.8.8/",NULL,err,sizeof(err)),"public refused");
        CHECK(strstr(err,"non-LAN")!=NULL,"policy text set");
        CHECK(!ctr_ws_policy_ok("ws://172.32.0.1/",NULL,NULL,0),"172.32 not private");
        CHECK(!ctr_ws_policy_ok("ws://user@192.0.2.1:9/",NULL,err,sizeof(err)),"userinfo smuggle refused");
        CHECK(strstr(err,"non-LAN")!=NULL,"policy text set with userinfo");
        CHECK(!ctr_ws_policy_ok("http://127.0.0.1:1/",NULL,NULL,0),"http refused");
        CHECK(!ctr_ws_policy_ok("",NULL,NULL,0),"empty refused");
        CHECK(setenv("TOMPOUCE_COLLAB_INSECURE","1",1)==0,"setenv");
        CHECK(ctr_ws_policy_ok("ws://8.8.8.8/",&insecure,err,sizeof(err)),"opt-in passes");
        CHECK(insecure,"opt-in reported");
        CHECK(unsetenv("TOMPOUCE_COLLAB_INSECURE")==0,"unsetenv");
        CHECK(!ctr_ws_policy_ok("ws://8.8.8.8/",NULL,NULL,0),"opt-in cleared");
    }
    CHECK(!ctr_send_text(NULL,"{}",2),"null send");
    {
        int binary=0;
        uint8_t *out=NULL;
        size_t outlen=0;
        CHECK(!ctr_recv(NULL,&binary,&out,&outlen,0),"null recv");
        CHECK(ctr_fd(NULL)==-1,"null fd");
        ctr_close(NULL);
    }
    /* Nick validation (section 13.5). */
    check_nick_ok("andrath");
    check_nick_ok("a");
    check_nick_ok("amber-otter");
    check_nick_ok("  spaced   out  ");
    check_nick_ok("012345678901234567890123"); /* 24 columns */
    check_nick_ok("mira");
    check_nick_ok("\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5"
                  "abcdefgh"); /* 4 wide + 8 narrow = 16 cols, 20 bytes */
    check_nick_bad("");
    check_nick_bad("     ");
    check_nick_bad("a\x01" "b");
    check_nick_bad("a\x7f" "b");
    check_nick_bad("a\xe2\x80\x8e" "b"); /* U+200E */
    check_nick_bad("a\xe2\x80\x8f" "b"); /* U+200F */
    check_nick_bad("012345678901234567890123456789012"); /* 33 bytes */
    check_nick_bad("xxxxxxxxxxxxxxxxxxxxxxxxx"); /* 25 columns */
    check_nick_bad("\xff");
    check_nick_bad("a\xc3"); /* truncated */
    /* Nick persistence against an isolated config dir. */
    tmpdir=mkdtemp(template);
    CHECK(tmpdir!=NULL,"mkdtemp");
    CHECK(setenv("XDG_CONFIG_HOME",tmpdir,1)==0,"xdg");
    CHECK(collab_nick_set("  Test   Name  ",err,sizeof(err)),"nick set");
    CHECK(collab_nick_get(nick,sizeof(nick)),"nick get");
    CHECK(strcmp(nick,"Test Name")==0,"nick normalized");
    CHECK(!collab_nick_set("a\x01" "b",err,sizeof(err)),"bad nick refused");
    CHECK(collab_nick_get(nick,sizeof(nick)),"stored nick kept");
    CHECK(strcmp(nick,"Test Name")==0,"stored nick kept value");
    /* Loopback hello/welcome against a real director. */
    binary=getenv("TOMPOUCE_DIRECTOR_BIN");
    if (!binary || !*binary) binary="./build/tompouce-director";
    if (access(binary,X_OK)!=0) {
        puts("collab transport loopback skipped: no director binary");
        return 0;
    }
    if (atexit(test_cleanup)!=0) return 1;
    port=free_port(17629);
    CHECK(port>0,"free port");
    pid=fork();
    CHECK(pid>=0,"fork");
    if (pid==0) {
        static char addr[64];
        snprintf(addr,sizeof(addr),"127.0.0.1:%d",port);
        execl(binary,"tompouce-director","--listen",addr,(char *)NULL);
        _exit(127);
    }
    test_child=pid;
    memset(&cfg,0,sizeof(cfg));
    snprintf(cfg.url,sizeof(cfg.url),"ws://127.0.0.1:%d/v1",port);
    {
        int attempt, connected=0;
        /* The director needs a moment to bind; retry the handshake. */
        for (attempt=0;attempt<50 && !connected;attempt++) {
            if (attempt>0) usleep(100000);
            memset(err,0,sizeof(err));
            connected=collab_connect(&cfg,err,sizeof(err));
            if (!connected) collab_disconnect();
        }
        CHECK(connected,"hello/welcome loopback");
        CHECK(collab_state()==COLLAB_CONNECTED,"state connected");
        /* Fresh config dir: the generated default was saved on connect. */
        CHECK(collab_nick_get(nick,sizeof(nick)),"generated nick");
        CHECK(strlen(nick)>=3 && strchr(nick,'-')!=NULL,"nick shape");
        collab_disconnect();
        CHECK(collab_state()==COLLAB_IDLE,"state idle");
    }
    puts("collab transport ok");
    return 0;
}
