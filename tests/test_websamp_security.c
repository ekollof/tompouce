#define _GNU_SOURCE
/* Real curl and production URL/socket policy with owned fake DNS and a
   loopback server. The test's connect shim retargets its one fake public
   address only; no external network or real internal service is contacted. */
#include "../src/websamp.c"
#include <assert.h>
#include <dlfcn.h>
#include <poll.h>
#include <stdatomic.h>

static unsigned server_port;
static atomic_int stop_server,hits;
static atomic_int policy_lookups,connection_lookups;

int getaddrinfo(const char *host,const char *service,const struct addrinfo *hints,struct addrinfo **result) {
    int (*real_getaddrinfo)(const char *,const char *,const struct addrinfo *,struct addrinfo **);
    void *symbol=dlsym(RTLD_NEXT,"getaddrinfo"); memcpy(&real_getaddrinfo,&symbol,sizeof(real_getaddrinfo));
    const char *number;
    if (!strcmp(host,"safe.audit.invalid")) number="8.8.8.8";
    else if (!strcmp(host,"audit.invalid")) {
        if (service) { atomic_fetch_add(&connection_lookups,1); number="127.0.0.1"; }
        else { atomic_fetch_add(&policy_lookups,1); number="93.184.216.34"; }
    } else return EAI_NONAME;
    return real_getaddrinfo(number,service,hints,result);
}

int connect(int fd,__CONST_SOCKADDR_ARG argument,socklen_t length) {
    const struct sockaddr *address=argument.__sockaddr__;
    int (*real_connect)(int,__CONST_SOCKADDR_ARG,socklen_t);
    void *symbol=dlsym(RTLD_NEXT,"connect"); memcpy(&real_connect,&symbol,sizeof(real_connect));
    if (address->sa_family!=AF_INET || length!=sizeof(struct sockaddr_in)) { errno=EPERM; return -1; }
    struct sockaddr_in owned=*(const struct sockaddr_in *)address;
    if (ntohs(owned.sin_port)!=server_port) { errno=EPERM; return -1; }
    if (owned.sin_addr.s_addr==htonl(0x08080808)) owned.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    if (owned.sin_addr.s_addr!=htonl(INADDR_LOOPBACK)) { errno=EPERM; return -1; }
    __CONST_SOCKADDR_ARG target={.__sockaddr__=(const struct sockaddr *)&owned};
    return real_connect(fd,target,sizeof(owned));
}

static void *serve(void *argument) {
    int listener=*(int *)argument;
    while (!atomic_load(&stop_server)) {
        struct pollfd p={.fd=listener,.events=POLLIN};
        if (poll(&p,1,20)<=0) continue;
        int peer=accept(listener,NULL,NULL); assert(peer>=0);
        struct timeval timeout={.tv_sec=1}; setsockopt(peer,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
        char request[1024]={0}; ssize_t got=recv(peer,request,sizeof(request)-1,0); assert(got>0);
        atomic_fetch_add(&hits,1);
        char reply[512];
        int audio=strstr(request," /proof.wav ")!=NULL;
        if (strstr(request," /redirect "))
            snprintf(reply,sizeof(reply),"HTTP/1.1 302 Found\r\nLocation: http://audit.invalid:%u/proof\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",server_port);
        else if (audio)
            snprintf(reply,sizeof(reply),"HTTP/1.1 200 OK\r\nContent-Length: 52\r\nConnection: close\r\n\r\n");
        else snprintf(reply,sizeof(reply),"HTTP/1.1 200 OK\r\nContent-Length: 21\r\nConnection: close\r\n\r\nOWNED_LOOPBACK_SECRET");
        size_t length=strlen(reply); assert(send(peer,reply,length,MSG_NOSIGNAL)==(ssize_t)length);
        if (audio) {
            const unsigned char wav[52]={
                'R','I','F','F',44,0,0,0,'W','A','V','E','f','m','t',' ',
                16,0,0,0,1,0,2,0,68,172,0,0,16,177,2,0,4,0,16,0,
                'd','a','t','a',8,0,0,0};
            assert(send(peer,wav,sizeof(wav),MSG_NOSIGNAL)==(ssize_t)sizeof(wav));
        }
        close(peer);
    }
    return NULL;
}

static void address_policy(void) {
    const struct { const char *ip; int family,allowed; } cases[]={
        {"8.8.8.8",AF_INET,1},{"93.184.216.34",AF_INET,1},
        {"127.0.0.1",AF_INET,0},{"0.0.0.0",AF_INET,0},{"10.0.0.1",AF_INET,0},
        {"169.254.169.254",AF_INET,0},{"172.16.0.1",AF_INET,0},{"192.168.1.1",AF_INET,0},
        {"100.64.0.1",AF_INET,0},{"198.18.0.1",AF_INET,0},{"192.0.2.1",AF_INET,0},
        {"224.0.0.1",AF_INET,0},{"::",AF_INET6,0},{"::1",AF_INET6,0},
        {"fc00::1",AF_INET6,0},{"fe80::1",AF_INET6,0},{"ff02::1",AF_INET6,0},
        {"::ffff:127.0.0.1",AF_INET6,0},{"::ffff:100.64.0.1",AF_INET6,0},
        {"::ffff:8.8.8.8",AF_INET6,1},{"2001:4860:4860::8888",AF_INET6,1},
        {"2001:db8::1",AF_INET6,0},{"2002:7f00:1::",AF_INET6,0}};
    for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        struct sockaddr_storage address={0};
        void *bytes=cases[i].family==AF_INET?(void *)&((struct sockaddr_in *)&address)->sin_addr:
            (void *)&((struct sockaddr_in6 *)&address)->sin6_addr;
        assert(inet_pton(cases[i].family,cases[i].ip,bytes)==1);
        struct addrinfo ai={.ai_family=cases[i].family,.ai_addr=(struct sockaddr *)&address};
        assert(addr_is_public(&ai)==cases[i].allowed);
    }
}

int main(void) {
    address_policy(); assert(!curl_global_init(CURL_GLOBAL_DEFAULT));
    char home[]="/tmp/tompouce-fetch-security-XXXXXX";
    assert(mkdtemp(home) && !setenv("HOME",home,1));
    int listener=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0); assert(listener>=0);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr={.s_addr=htonl(INADDR_LOOPBACK)}};
    assert(!bind(listener,(struct sockaddr *)&address,sizeof(address)) && !listen(listener,4));
    socklen_t length=sizeof(address); assert(!getsockname(listener,(struct sockaddr *)&address,&length));
    server_port=ntohs(address.sin_port);
    pthread_t thread; assert(!pthread_create(&thread,NULL,serve,&listener));
    char url[160],proxy[128];
    snprintf(proxy,sizeof(proxy),"http://127.0.0.1:%u",server_port);
    setenv("http_proxy",proxy,1); setenv("https_proxy",proxy,1); setenv("ALL_PROXY",proxy,1);
    setenv("NO_PROXY","",1); setenv("no_proxy","",1);
    snprintf(url,sizeof(url),"http://safe.audit.invalid:%u/proof",server_port);
    char *text=websamp_fetch_text(url); assert(text && !strcmp(text,"OWNED_LOOPBACK_SECRET")); free(text);
    assert(atomic_load(&hits)==1); /* direct public fetch works despite ambient proxy */
    snprintf(url,sizeof(url),"http://audit.invalid:%u/proof",server_port);
    assert(!websamp_fetch_text(url) && atomic_load(&hits)==1);
    assert(atomic_load(&connection_lookups)>0 && !atomic_load(&policy_lookups));
    snprintf(url,sizeof(url),"http://safe.audit.invalid:%u/redirect",server_port);
    assert(!websamp_fetch_text(url) && atomic_load(&hits)==2); /* no redirected private request */
    char path[512];
    snprintf(url,sizeof(url),"http://safe.audit.invalid:%u/proof.wav",server_port);
    assert(download_audio_unlocked(url,path,sizeof(path)) && atomic_load(&hits)==3);
    assert(!unlink(path));
    snprintf(url,sizeof(url),"http://audit.invalid:%u/proof.wav",server_port);
    assert(!download_audio_unlocked(url,path,sizeof(path)) && atomic_load(&hits)==3);
    atomic_store(&stop_server,1); assert(!pthread_join(thread,NULL)); close(listener);
    curl_global_cleanup();
    const char *directories[]={".cache/tompouce/web",".cache/tompouce",".cache",""};
    for (unsigned i=0;i<sizeof(directories)/sizeof(directories[0]);i++) {
        snprintf(path,sizeof(path),"%s/%s",home,directories[i]); assert(!rmdir(path));
    }
    puts("Connection-time address policy rejects rebinding/private redirects and ignores proxy bypasses");
    return 0;
}
