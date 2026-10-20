/* Real loopback protocol and socket failure paths, without starting an
   engine, scheduler, browser, audio backend or MIDI device. */
#include "../src/mcp.c"
#include <assert.h>
#include <signal.h>

static int client(void) {
    int fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,0); assert(fd>=0);
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons(mcp_port_bound),
        .sin_addr={.s_addr=htonl(INADDR_LOOPBACK)}};
    assert(!connect(fd,(struct sockaddr *)&address,sizeof(address)));
    struct timeval timeout={.tv_sec=1};
    assert(!setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)));
    mcp_poll(); assert(mcp_input_fd>=0);
    return fd;
}

static void transmit(int fd,const char *text) {
    size_t length=strlen(text);
    assert(send(fd,text,length,MSG_NOSIGNAL)==(ssize_t)length);
    mcp_poll();
}

static void authenticated(char *out,size_t size,const char *method) {
    int n=snprintf(out,size,"{\"jsonrpc\":\"2.0\",\"auth\":\"%s\",\"id\":1,\"method\":\"%s\"}\n",
        mcp_tcp_token(),method);
    assert(n>0 && (size_t)n<size);
}

int main(void) {
    signal(SIGPIPE,SIG_DFL);
    assert(!setenv("TOMPOUCE_MCP_TOKEN","invalid",1));
    assert(!mcp_auth_init(0) && !mcp_auth_init(1));
    unsetenv("TOMPOUCE_MCP_TOKEN");
    assert(!mcp_auth_init(0)); /* explicit TCP cannot start without a capability */
    assert(!mcp_open_listen(0)); /* UI companion generates its own capability */
    assert(strlen(mcp_tcp_token())==64);
    char request[1024],reply[1024];
    int fd=client();
    mcp_client_deadline=now_seconds()-1; mcp_poll();
    assert(mcp_input_fd<0); close(fd);
    fd=client();
    transmit(fd,"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}\n");
    assert(mcp_input_fd<0 && recv(fd,reply,sizeof(reply),0)==0); close(fd);

    fd=client();
    char wrong[65]; memcpy(wrong,mcp_tcp_token(),65); wrong[0]=wrong[0]=='0'?'1':'0';
    snprintf(request,sizeof(request),"{\"auth\":\"%s\",\"method\":\"ping\"}\n",wrong);
    transmit(fd,request);
    assert(mcp_input_fd<0); close(fd);

    fd=client();
    snprintf(request,sizeof(request),"POST / HTTP/1.1\r\nOrigin: https://attacker.invalid\r\n\r\n"
        "{\"auth\":\"%s\",\"method\":\"tools/call\",\"params\":{\"name\":\"music_set_bpm\",\"arguments\":{\"bpm\":133}}}\n",mcp_tcp_token());
    double before=audio_get_bpm(); transmit(fd,request);
    assert(mcp_input_fd<0 && audio_get_bpm()==before); close(fd);

    fd=client();
    char chunk[8192]; memset(chunk,'x',sizeof(chunk));
    for (int i=0;i<129 && mcp_input_fd>=0;i++) {
        assert(send(fd,chunk,sizeof(chunk),MSG_NOSIGNAL)==(ssize_t)sizeof(chunk));
        mcp_poll();
    }
    assert(mcp_input_fd<0); close(fd);

    fd=client();
    authenticated(request,sizeof(request),"ping"); transmit(fd,request);
    ssize_t count=recv(fd,reply,sizeof(reply)-1,0); assert(count>0); reply[count]=0;
    assert(strstr(reply,"\"result\""));
    transmit(fd,"{\"method\":\"ping\"}\n"); /* auth is required on every frame */
    assert(mcp_input_fd<0); close(fd);

    fd=client();
    snprintf(request,sizeof(request),"{\"auth\":\"%s\",\"method\":\"ping\"} garbage\n",mcp_tcp_token());
    transmit(fd,request); assert(mcp_input_fd<0); close(fd);

    /* Deterministically closed output: default SIGPIPE must remain harmless. */
    int pair[2]; assert(!socketpair(AF_UNIX,SOCK_STREAM,0,pair)); close(pair[1]);
    mcp_output_socket=pair[0]; mcp_send_failed=0;
    cJSON *ping=cJSON_Parse("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}");
    handle_request(ping); cJSON_Delete(ping);
    assert(mcp_send_failed); close(pair[0]); mcp_output_socket=-1; mcp_send_failed=0;

    /* A slow reader cannot turn foreground output into a blocking wait. */
    fd=client(); int capacity=4096;
    assert(!setsockopt(mcp_input_fd,SOL_SOCKET,SO_SNDBUF,&capacity,sizeof(capacity)));
    char *source=malloc(2*1024*1024+1); assert(source);
    memset(source,'x',2*1024*1024); source[2*1024*1024]=0;
    assert(current_code_set(source)); free(source);
    authenticated(request,sizeof(request),"tools/call");
    snprintf(request,sizeof(request),"{\"auth\":\"%s\",\"method\":\"tools/call\",\"params\":{\"name\":\"music_get_code\"}}\n",mcp_tcp_token());
    transmit(fd,request); assert(mcp_input_fd<0); close(fd);
    assert(current_code_set(""));
    fd=client(); authenticated(request,sizeof(request),"ping"); transmit(fd,request);
    assert(recv(fd,reply,sizeof(reply),0)>0); close(fd); mcp_poll();
    close(mcp_listen_fd); mcp_listen_fd=-1;
    puts("MCP capabilities, HTTP rejection, peer failure and bounded socket writes hold");
    return 0;
}
