#define _GNU_SOURCE
#include "csound_host.h"
#include "websamp.h"
#include <stdatomic.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/resource.h>
#ifdef HAVE_CSOUND
#include <csound/csound.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <stddef.h>
#include "presets.inc"
#endif

#define CS_BLOCK 64
#define CS_RING 4096
#define CS_QUEUE 512
#define CS_CODE_MAX (2*1024*1024)
enum { CS_COMPILE = 1, CS_EVENT, CS_RENDER, CS_UPDATE, CS_DIAG };
typedef struct { unsigned command, length; } Message;
typedef struct { int status; char error[1024]; } Reply;
typedef struct {
    double time, duration, frequency, amplitude;
    char instrument[68], controls[1024];
} Pending;
struct CsoundProgram {
    atomic_uint refs;
    atomic_int stop, active;
    atomic_ulong read_frame, write_frame;
    pthread_mutex_t lock;
    pthread_mutex_t update_lock;
    atomic_uint merge_requested, merge_completed;
    pthread_t thread;
    int socket, update_socket, diag_fd, started;
    pid_t child;
    double origin;
    Pending pending[CS_QUEUE];
    unsigned count;
    char *loaded_urls[64];
    unsigned url_count;
    float pcm[CS_RING][2];
};

/* Private socket protocol, never the application's MCP stdout. */
#ifdef HAVE_CSOUND
/* Failed orchestra fetches stay rejected without refetching, matching the
   deployed loader's session promise cache: the second loadOrc of a dead URL
   rejects from cache (one observable fetch for two calls). Successes stay
   per-program (each engine still compiles its own copy); failures are
   process-lifetime like the page session. Oversize content is not cached:
   upstream text may shrink below the limit. */
static char *failed_orchestra_urls[64];
static unsigned failed_orchestra_url_count;
static pthread_mutex_t failed_orchestra_urls_lock = PTHREAD_MUTEX_INITIALIZER;

static int orchestra_fetch_failed(const char *resolved) {
    int failed=0;
    pthread_mutex_lock(&failed_orchestra_urls_lock);
    for (unsigned i=0;i<failed_orchestra_url_count;i++)
        if (!strcmp(failed_orchestra_urls[i],resolved)) { failed=1; break; }
    pthread_mutex_unlock(&failed_orchestra_urls_lock);
    return failed;
}

static void orchestra_record_fetch_failure(const char *resolved) {
    char *key=strdup_safe(resolved);
    if (!key) return;
    pthread_mutex_lock(&failed_orchestra_urls_lock);
    if (failed_orchestra_url_count<64) {
        int known=0;
        for (unsigned i=0;i<failed_orchestra_url_count;i++)
            if (!strcmp(failed_orchestra_urls[i],resolved)) { known=1; break; }
        if (known) free(key);
        else failed_orchestra_urls[failed_orchestra_url_count++]=key;
    } else {
        /* Best effort: without room the next evaluation refetches and fails
           with the same error. */
        free(key);
    }
    pthread_mutex_unlock(&failed_orchestra_urls_lock);
}
static int transfer(int fd, void *buffer, size_t size, int sending) {
    unsigned char *p = buffer;
    while (size) {
        struct pollfd ready = {fd,sending ? POLLOUT : POLLIN,0};
        int status;
        do status = poll(&ready,1,3000); while (status < 0 && errno == EINTR);
        if (status <= 0 || !(ready.revents & ready.events)) return 0;
        ssize_t n = sending ? send(fd,p,size,MSG_NOSIGNAL) : recv(fd,p,size,0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        p += n; size -= (size_t)n;
    }
    return 1;
}

static int send_message(int fd, unsigned command, const void *data, size_t size) {
    if (size > CS_CODE_MAX) return 0;
    Message header = {command,(unsigned)size};
    return transfer(fd,&header,sizeof(header),1) && (!size || transfer(fd,(void *)data,size,1));
}

static int receive_header(int fd, Message *header) {
    /* Idle sessions may have no local output. Execution timeouts belong to
       active requests, not to the interval between orchestra evaluations. */
    struct pollfd ready={fd,POLLIN,0}; int status;
    do { status=poll(&ready,1,-1); } while (status<0 && errno==EINTR);
    return status>0 && transfer(fd,header,sizeof(*header),0);
}

static _Thread_local Reply *worker_reply;
/* Performance-time engine messages have no compile reply to join. They wait
   in a bounded ring for the next loop turn, which forwards them to the
   parent; beyond the bound only a dropped count survives. */
enum { WORKER_DIAG_COUNT = 8, WORKER_DIAG_LENGTH = 224 };
static char worker_diags[WORKER_DIAG_COUNT][WORKER_DIAG_LENGTH];
static unsigned worker_diag_head, worker_diag_count, worker_diag_dropped;
static void worker_diag_emit(const char *text) {
    if (!text || !*text) return;
    if (worker_diag_count<WORKER_DIAG_COUNT) {
        snprintf(worker_diags[(worker_diag_head+worker_diag_count)%WORKER_DIAG_COUNT],
            WORKER_DIAG_LENGTH,"%.223s",text);
        worker_diag_count++;
    } else worker_diag_dropped++;
}
static void worker_diag_flush(void) {
    if (worker_diag_dropped) {
        char summary[WORKER_DIAG_LENGTH];
        snprintf(summary,sizeof(summary),"earlier worker messages omitted (%u)",worker_diag_dropped);
        Message header={CS_DIAG,0};
        /* Fixed framing: a header with no bytes can never arrive split. */
        char frame[sizeof(header)+WORKER_DIAG_LENGTH];
        size_t body=strlen(summary)+1;
        header.length=(unsigned)body;
        memcpy(frame,&header,sizeof(header)); memcpy(frame+sizeof(header),summary,body);
        if (send(5,frame,sizeof(header)+body,MSG_DONTWAIT|MSG_NOSIGNAL)<0) return;
        worker_diag_dropped=0;
    }
    while (worker_diag_count) {
        Message header={CS_DIAG,0};
        char frame[sizeof(header)+WORKER_DIAG_LENGTH];
        size_t body=strlen(worker_diags[worker_diag_head])+1;
        header.length=(unsigned)body;
        memcpy(frame,&header,sizeof(header));
        memcpy(frame+sizeof(header),worker_diags[worker_diag_head],body);
        if (send(5,frame,sizeof(header)+body,MSG_DONTWAIT|MSG_NOSIGNAL)<0) return;
        worker_diag_head=(worker_diag_head+1)%WORKER_DIAG_COUNT; worker_diag_count--;
    }
}
static void cs_message(CSOUND *engine, int attr, const char *text) {
    (void)engine; (void)attr;
    Reply *reply=worker_reply;
    if (!reply) { worker_diag_emit(text); return; }
    size_t length = strlen(text), used = strlen(reply->error);
    if (length >= sizeof(reply->error)) { snprintf(reply->error,sizeof(reply->error),"%.1000s",text); return; }
    if (used+length >= sizeof(reply->error)) {
        size_t discard = used+length-(sizeof(reply->error)-1);
        memmove(reply->error,reply->error+discard,used-discard+1); used -= discard;
    }
    memcpy(reply->error+used,text,length+1);
}

/* Orchestra execution is isolated in a child with no file, network,
   process-creation or exec syscalls. Only its private IPC fds are usable.
   The parent also imposes a timeout on compilation and each render block. */
static int confine_worker(void) {
#if defined(__x86_64__)
#define CS_ARCH AUDIT_ARCH_X86_64
#elif defined(__aarch64__)
#define CS_ARCH AUDIT_ARCH_AARCH64
#else
    return 0;
#endif
#ifdef CS_ARCH
#define ALLOW_SYSCALL(n) BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,(n),0,1), BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW)
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,arch)),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,CS_ARCH,1,0),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_KILL_PROCESS),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,offsetof(struct seccomp_data,nr)),
        ALLOW_SYSCALL(SYS_recvfrom), ALLOW_SYSCALL(SYS_sendto),
#ifdef SYS_poll
        ALLOW_SYSCALL(SYS_poll),
#endif
        ALLOW_SYSCALL(SYS_ppoll),
        ALLOW_SYSCALL(SYS_brk), ALLOW_SYSCALL(SYS_mmap),
        ALLOW_SYSCALL(SYS_munmap), ALLOW_SYSCALL(SYS_mremap),
        ALLOW_SYSCALL(SYS_mprotect), ALLOW_SYSCALL(SYS_madvise),
        ALLOW_SYSCALL(SYS_futex), ALLOW_SYSCALL(SYS_clock_gettime),
        ALLOW_SYSCALL(SYS_getrandom), ALLOW_SYSCALL(SYS_getpid),
        ALLOW_SYSCALL(SYS_gettid), ALLOW_SYSCALL(SYS_rt_sigprocmask),
        ALLOW_SYSCALL(SYS_rt_sigreturn), ALLOW_SYSCALL(SYS_sched_yield),
        ALLOW_SYSCALL(SYS_exit), ALLOW_SYSCALL(SYS_exit_group),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|EPERM)
    };
    struct sock_fprog program = {(unsigned short)(sizeof(filter)/sizeof(filter[0])),filter};
    return !prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0) && !prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&program);
#undef ALLOW_SYSCALL
#endif
}

typedef struct { CSOUND *engine; atomic_int initialized; } WorkerCompiler;
static void *compile_worker(void *opaque) {
    WorkerCompiler *compiler=opaque;
    Reply reply={0}; worker_reply=&reply;
    /* This trusted thread is created before confinement. Orchestra code can
       neither create threads nor obtain file/network access from either one. */
    if (!confine_worker()) _exit(1);
    for (;;) {
        Message header;
        if (!receive_header(4,&header) ||
            header.command!=CS_UPDATE || !header.length || header.length>CS_CODE_MAX ||
            !atomic_load_explicit(&compiler->initialized,memory_order_acquire)) break;
        char *code=malloc((size_t)header.length+1);
        if (!code || !transfer(4,code,header.length,0)) { free(code); break; }
        code[header.length]=0; memset(&reply,0,sizeof(reply));
#ifdef HAVE_CSOUND7
        reply.status=csoundCompileOrc(compiler->engine,code,1);
#else
        reply.status=csoundCompileOrcAsync(compiler->engine,code);
#endif
        free(code);
        if (!transfer(4,&reply,sizeof(reply),1)) break;
    }
    _exit(0);
}
#endif

int csound_worker_main(void) {
#ifndef HAVE_CSOUND
    return 1;
#else
    Reply reply = {0};
    worker_reply=&reply;
    struct rlimit memory = {512u*1024u*1024u,512u*1024u*1024u};
    if (setrlimit(RLIMIT_AS,&memory)) return 1;
    if (csoundInitialize(CSOUNDINIT_NO_SIGNAL_HANDLER|CSOUNDINIT_NO_ATEXIT) < 0) return 1;
#ifdef HAVE_CSOUND7
    CSOUND *engine = csoundCreate(&reply,CSOUND7_OPCODEDIR);
#else
    CSOUND *engine = csoundCreate(&reply);
#endif
    if (!engine) return 1;
    csoundSetMessageStringCallback(engine,cs_message);
#ifdef HAVE_CSOUND7
    csoundSetHostAudioIO(engine);
#define COMPILE_ORC(e,s) csoundCompileOrc((e),(s),0)
#define CHANNELS(e) csoundGetChannels((e),0)
#else
    csoundSetHostImplementedAudioIO(engine,1,0);
#define COMPILE_ORC(e,s) csoundCompileOrc((e),(s))
#define CHANNELS(e) csoundGetNchnls(e)
#endif
    if (csoundSetOption(engine,"-n") || csoundSetOption(engine,"-d") ||
        csoundSetOption(engine,"-m0") || csoundSetOption(engine,"--sample-accurate")) return 1;
    WorkerCompiler compiler={.engine=engine}; atomic_init(&compiler.initialized,0);
    pthread_t compiler_thread;
    if (pthread_create(&compiler_thread,NULL,compile_worker,&compiler)) return 1;
    /* Retain only the two private transports after trusted initialization. */
    close(0); close(1); close(2);
    if (close_range(6,~0u,0)) return 1;
    if (clearenv()) return 1;
    if (!confine_worker()) return 1;
    int initialized = 0;
    for (;;) {
        Message header;
        if (!receive_header(3,&header) || header.length > CS_CODE_MAX) break;
        char *data = NULL;
        if (header.length) {
            data = malloc((size_t)header.length+1);
            if (!data || !transfer(3,data,header.length,0)) { free(data); break; }
            data[header.length] = 0;
        }
        if (header.command == CS_COMPILE && data && !initialized) {
            const char *settings = "sr=44100\nksmps=64\nnchnls=2\n0dbfs=1\n";
            reply.status = COMPILE_ORC(engine,settings);
            char *presets = malloc((size_t)strudel_csound_presets_len+1);
            if (!presets) { free(data); break; }
            memcpy(presets,strudel_csound_presets,strudel_csound_presets_len);
            presets[strudel_csound_presets_len] = 0;
            if (!reply.status) reply.status = COMPILE_ORC(engine,presets);
            free(presets);
            if (!reply.status && strspn(data," \t\r\n") != header.length)
                reply.status = COMPILE_ORC(engine,data);
            if (!reply.status) reply.status = csoundStart(engine);
            if (!reply.status && (csoundGetKsmps(engine) != CS_BLOCK || CHANNELS(engine) != 2 || csoundGetSr(engine) != SR))
                reply.status = -1;
            initialized = !reply.status;
            atomic_store_explicit(&compiler.initialized,initialized,memory_order_release);
            if (!transfer(3,&reply,sizeof(reply),1)) { free(data); break; }
            /* Later engine messages are runtime diagnostics, not compile output. */
            worker_reply=NULL;
        } else if (header.command == CS_EVENT && data && initialized) {
#ifdef HAVE_CSOUND7
            csoundEventString(engine,data,0);
#else
            csoundInputMessage(engine,data);
#endif
        } else if (header.command == CS_RENDER && !data && initialized) {
            float output[CS_BLOCK][2] = {{0}};
            int performed=csoundPerformKsmps(engine);
            if (csoundGetKsmps(engine)!=CS_BLOCK || CHANNELS(engine)!=2 || csoundGetSr(engine)!=SR) { free(data); break; }
            if (!performed) {
#ifdef HAVE_CSOUND7
                const cs_float *pcm = csoundGetSpout(engine);
#else
                MYFLT *pcm = csoundGetSpout(engine);
#endif
                for (int frame = 0; frame < CS_BLOCK; frame++)
                    for (int ch = 0; ch < 2; ch++) output[frame][ch] = isfinite(pcm[frame*2+ch]) ? (float)pcm[frame*2+ch] : 0;
            }
            if (!transfer(3,output,sizeof(output),1)) { free(data); break; }
        } else { free(data); break; }
        worker_diag_flush();
        free(data);
    }
    /* Exit tears down the confined instance without running plugin finalizers. */
    _exit(0);
#endif
}

#ifdef HAVE_CSOUND
/* Drain worker runtime diagnostics without blocking the render pump. The
   audio callback never touches IPC; this thread already owns the sockets. */
static void drain_worker_diags(CsoundProgram *program) {
    if (program->diag_fd<0) return;
    for (int i=0;i<16;i++) {
        struct pollfd ready={program->diag_fd,POLLIN,0};
        int status;
        do status=poll(&ready,1,0); while (status<0 && errno==EINTR);
        if (status<=0 || !(ready.revents&POLLIN)) return;
        Message header;
        if (!transfer(program->diag_fd,&header,sizeof(header),0) ||
            header.command!=CS_DIAG || !header.length || header.length>WORKER_DIAG_LENGTH) return;
        char text[WORKER_DIAG_LENGTH];
        if (!transfer(program->diag_fd,text,header.length,0)) return;
        text[sizeof(text)-1]=0;
        log_console("Csound: %s",text);
    }
}
static void *render_worker(void *opaque) {
    CsoundProgram *program = opaque;
    unsigned long frame = 0;
    while (!atomic_load(&program->stop)) {
        drain_worker_diags(program);
        unsigned long read_frame = atomic_load_explicit(&program->read_frame,memory_order_acquire);
        unsigned merge=atomic_load_explicit(&program->merge_requested,memory_order_acquire);
        int fence=merge!=atomic_load_explicit(&program->merge_completed,memory_order_relaxed);
        if ((!fence && (!atomic_load_explicit(&program->active,memory_order_acquire) || frame >= read_frame+512)) ||
            frame+CS_BLOCK>read_frame+CS_RING) {
            struct timespec pause = {0,1000000}; nanosleep(&pause,NULL); continue;
        }
        Pending pending[CS_QUEUE];
        pthread_mutex_lock(&program->lock);
        unsigned count = program->count;
        memcpy(pending,program->pending,count*sizeof(*pending));
        program->count = 0;
        pthread_mutex_unlock(&program->lock);
        for (unsigned i = 0; i < count; i++) {
            char score[2200];
            double offset = fmax(0,pending[i].time-program->origin-frame/SR);
            int n = snprintf(score,sizeof(score),"i %s %.9f %.9f %.9f %.9f \"%s\"",
                pending[i].instrument,offset,pending[i].duration,pending[i].frequency,pending[i].amplitude,pending[i].controls);
            if (n < 0 || (size_t)n >= sizeof(score) || !send_message(program->socket,CS_EVENT,score,(size_t)n)) goto failed;
        }
        float output[CS_BLOCK][2];
        if (!send_message(program->socket,CS_RENDER,NULL,0) || !transfer(program->socket,output,sizeof(output),0)) goto failed;
        for (unsigned i = 0; i < CS_BLOCK; i++) {
            read_frame = atomic_load_explicit(&program->read_frame,memory_order_acquire);
            if (frame+i >= read_frame) memcpy(program->pcm[(frame+i)%CS_RING],output[i],sizeof(output[i]));
        }
        frame += CS_BLOCK;
        atomic_store_explicit(&program->write_frame,frame,memory_order_release);
        if (fence) atomic_store_explicit(&program->merge_completed,merge,memory_order_release);
    }
    return NULL;
failed:
    kill(program->child,SIGKILL);
    log_console("Csound: renderer stopped (error or execution timeout)");
    atomic_store(&program->stop,1);
    return NULL;
}

typedef struct { char *code; size_t used; char *urls[16]; unsigned count; } OrchestraLoads;
static void finish_loads(CsoundProgram *program, OrchestraLoads *loads, int success) {
    for (unsigned i=0;i<loads->count;i++) {
        if (success) program->loaded_urls[program->url_count++]=loads->urls[i];
        else free(loads->urls[i]);
    }
    free(loads->code);
}
static int prepare_loads(CsoundProgram *program, const cJSON *items, OrchestraLoads *loads, char *error, size_t size) {
    memset(loads,0,sizeof(*loads));
    if (!cJSON_IsArray(items) || cJSON_GetArraySize(items)>16) return 0;
    loads->code=calloc(CS_CODE_MAX+1,1);
    if (!loads->code) return 0;
    const cJSON *item;
    cJSON_ArrayForEach(item,items) {
        const cJSON *inline_code=cJSON_GetObjectItemCaseSensitive(item,"code");
        const cJSON *url=cJSON_GetObjectItemCaseSensitive(item,"url");
        if (cJSON_IsString(inline_code) && !inline_code->valuestring[0]) continue;
        int remote=!cJSON_IsString(inline_code) && cJSON_IsString(url);
        char resolved[480];
        if (remote) {
            if (!websamp_resolve("",url->valuestring,resolved,sizeof(resolved))) {
                snprintf(error,size,"Csound: invalid orchestra URL"); goto failed;
            }
            int cached=0;
            for (unsigned i=0;i<program->url_count;i++) if (!strcmp(program->loaded_urls[i],resolved)) cached=1;
            for (unsigned i=0;i<loads->count;i++) if (!strcmp(loads->urls[i],resolved)) cached=1;
            if (cached) continue;
            if (orchestra_fetch_failed(resolved)) {
                snprintf(error,size,"Csound: orchestra fetch failed or source exceeds limit"); goto failed;
            }
            if (program->url_count+loads->count>=64) {
                snprintf(error,size,"Csound: orchestra URL cache limit exceeded"); goto failed;
            }
            char *key=strdup_safe(resolved);
            if (!key) goto failed;
            loads->urls[loads->count++]=key;
        }
        char *downloaded=remote?websamp_fetch_text(resolved):NULL;
        const char *text=cJSON_IsString(inline_code)?inline_code->valuestring:downloaded;
        if (remote && !downloaded) orchestra_record_fetch_failure(resolved);
        if (!text || strlen(text)+1>CS_CODE_MAX-loads->used) {
            snprintf(error,size,"Csound: orchestra fetch failed or source exceeds limit"); free(downloaded); goto failed;
        }
        size_t length=strlen(text);
        memcpy(loads->code+loads->used,text,length); loads->used+=length;
        loads->code[loads->used++]='\n'; free(downloaded);
    }
    return 1;
failed:
    finish_loads(program,loads,0); loads->code=NULL; return 0;
}
#endif

CsoundProgram *csound_program_create(const cJSON *loads, char *error, size_t size) {
#ifndef HAVE_CSOUND
    (void)loads;
    snprintf(error,size,"Csound support requires libcsound64 and its development headers; rebuild after installing csound");
    return NULL;
#else
    if (error && size) snprintf(error,size,"Csound: unable to initialize native renderer");
    if (!cJSON_IsArray(loads) || cJSON_GetArraySize(loads) > 16) return NULL;
    CsoundProgram *program = calloc(1,sizeof(*program));
    if (!program) return NULL;
    program->socket = program->update_socket = -1; program->child = -1;
    program->diag_fd = -1;
    atomic_init(&program->refs,1); atomic_init(&program->stop,0); atomic_init(&program->active,0);
    atomic_init(&program->read_frame,0); atomic_init(&program->write_frame,0);
    atomic_init(&program->merge_requested,0); atomic_init(&program->merge_completed,0);
    if (pthread_mutex_init(&program->lock,NULL)) { free(program); return NULL; }
    if (pthread_mutex_init(&program->update_lock,NULL)) { pthread_mutex_destroy(&program->lock); free(program); return NULL; }
    OrchestraLoads batch;
    if (!prepare_loads(program,loads,&batch,error,size)) { csound_program_free(program); return NULL; }
    if (!batch.used) batch.code[batch.used++]='\n';
    int sockets[2], updates[2], diags[2];
    if (socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,sockets)) goto failed;
    if (socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,updates)) { close(sockets[0]); close(sockets[1]); goto failed; }
    if (socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,diags)) {
        close(sockets[0]); close(sockets[1]); close(updates[0]); close(updates[1]); goto failed;
    }
    program->socket=sockets[0]; program->update_socket=updates[0]; program->diag_fd=diags[0];
    char executable[4096];
    ssize_t length = readlink("/proc/self/exe",executable,sizeof(executable)-1);
    if (length <= 0) { close(sockets[1]); close(updates[1]); close(diags[0]); close(diags[1]); goto failed; }
    executable[length] = 0;
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions)) {
        close(sockets[1]); close(updates[1]); close(diags[0]); close(diags[1]); goto failed;
    }
    int action_error = posix_spawn_file_actions_adddup2(&actions,sockets[1],3);
    if (!action_error) action_error = posix_spawn_file_actions_adddup2(&actions,updates[1],4);
    if (!action_error) action_error = posix_spawn_file_actions_adddup2(&actions,diags[1],5);
    if (!action_error) action_error = posix_spawn_file_actions_addopen(&actions,0,"/dev/null",O_RDONLY,0);
    if (!action_error) action_error = posix_spawn_file_actions_addopen(&actions,1,"/dev/null",O_WRONLY,0);
    if (!action_error) action_error = posix_spawn_file_actions_addopen(&actions,2,"/dev/null",O_WRONLY,0);
    int inherited[]={sockets[0],sockets[1],updates[0],updates[1],diags[0],diags[1]};
    for (int i=0;i<6 && !action_error;i++) if (inherited[i]>5)
        action_error=posix_spawn_file_actions_addclose(&actions,inherited[i]);
    char *args[] = {executable,"--csound-worker",NULL};
    extern char **environ;
    int spawned = action_error ? action_error : posix_spawn(&program->child,executable,&actions,NULL,args,environ);
    posix_spawn_file_actions_destroy(&actions);
    close(sockets[1]); close(updates[1]); close(diags[1]);
    if (spawned) goto failed;
    Reply reply = {0};
    if (!send_message(program->socket,CS_COMPILE,batch.code,batch.used) || !transfer(program->socket,&reply,sizeof(reply),0)) {
        snprintf(error,size,"Csound: compilation failed or exceeded execution budget"); goto failed;
    }
    if (reply.status) { snprintf(error,size,"Csound: %.1000s",reply.error[0] ? reply.error : "orchestra compilation failed"); goto failed; }
    if (pthread_create(&program->thread,NULL,render_worker,program)) goto failed;
    program->started = 1;
    finish_loads(program,&batch,1); return program;
failed:
    finish_loads(program,&batch,0); csound_program_free(program); return NULL;
#endif
}

CsoundUpdateResult csound_program_update(CsoundProgram *program, const cJSON *loads, char *error, size_t size) {
#ifndef HAVE_CSOUND
    (void)program; (void)loads; (void)error; (void)size; return 0;
#else
    if (!program || atomic_load(&program->stop)) return 0;
    pthread_mutex_lock(&program->update_lock);
    OrchestraLoads batch;
    int ok=prepare_loads(program,loads,&batch,error,size);
    if (!ok) { pthread_mutex_unlock(&program->update_lock); return 0; }
    int rejected=0;
    if (batch.used) {
        unsigned long read=atomic_load_explicit(&program->read_frame,memory_order_acquire);
        unsigned long written=atomic_load_explicit(&program->write_frame,memory_order_acquire);
        Reply reply={0};
        if (written>read+CS_RING-CS_BLOCK*2) {
            snprintf(error,size,"Csound: renderer must advance before another orchestra update"); ok=0;
        } else if (!send_message(program->update_socket,CS_UPDATE,batch.code,batch.used) ||
                   !transfer(program->update_socket,&reply,sizeof(reply),0)) {
            snprintf(error,size,"Csound: orchestra update failed or exceeded execution budget"); ok=0;
        } else if (reply.status) {
            snprintf(error,size,"Csound: %.1000s",reply.error[0]?reply.error:"orchestra compilation failed"); rejected=1;
        } else {
            unsigned merge=atomic_fetch_add_explicit(&program->merge_requested,1,memory_order_release)+1;
            struct timespec start,now; clock_gettime(CLOCK_MONOTONIC,&start);
            while (atomic_load_explicit(&program->merge_completed,memory_order_acquire)!=merge && !atomic_load(&program->stop)) {
                struct timespec pause={0,1000000}; nanosleep(&pause,NULL);
                clock_gettime(CLOCK_MONOTONIC,&now);
                if (now.tv_sec-start.tv_sec+(now.tv_nsec-start.tv_nsec)/1e9>3) break;
            }
            if (atomic_load_explicit(&program->merge_completed,memory_order_acquire)!=merge) {
                snprintf(error,size,"Csound: orchestra merge failed or exceeded execution budget"); ok=0;
            }
        }
    }
    finish_loads(program,&batch,ok);
    pthread_mutex_unlock(&program->update_lock);
    return !ok?CSOUND_UPDATE_FAILED:rejected?CSOUND_UPDATE_REJECTED:CSOUND_UPDATE_OK;
#endif
}

void csound_program_retain(CsoundProgram *program) {
    if (program) atomic_fetch_add(&program->refs,1);
}
void csound_program_free(CsoundProgram *program) {
    if (!program || atomic_fetch_sub(&program->refs,1) != 1) return;
    atomic_store(&program->stop,1);
    if (program->child > 0) kill(program->child,SIGKILL);
    if (program->started) pthread_join(program->thread,NULL);
    if (program->socket >= 0) close(program->socket);
    if (program->update_socket >= 0) close(program->update_socket);
    if (program->diag_fd >= 0) close(program->diag_fd);
    if (program->child > 0) { while (waitpid(program->child,NULL,0) < 0 && errno == EINTR) {} }
    for (unsigned i=0;i<program->url_count;i++) free(program->loaded_urls[i]);
    pthread_mutex_destroy(&program->update_lock); pthread_mutex_destroy(&program->lock); free(program);
}
void csound_program_activate(CsoundProgram *program, double origin) {
    if (!program) return;
    program->origin = origin;
    atomic_store_explicit(&program->active,1,memory_order_release);
}
int csound_program_schedule(CsoundProgram *program, const cJSON *value, double time, double duration, double frequency) {
    if (!program || atomic_load(&program->stop) || !isfinite(time) || !isfinite(duration) || !isfinite(frequency)) return 0;
    const cJSON *instrument = cJSON_GetObjectItemCaseSensitive(value,"csound");
    char token[68];
    if (cJSON_IsNumber(instrument)) {
        if (!isfinite(instrument->valuedouble) || instrument->valuedouble < 1 || instrument->valuedouble >= 65536) return 0;
        snprintf(token,sizeof(token),"%.9g",instrument->valuedouble);
    } else if (cJSON_IsString(instrument)) {
        if (!instrument->valuestring[0] || strlen(instrument->valuestring) > 63) return 0;
        for (const char *p = instrument->valuestring; *p; p++)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_')) return 0;
        snprintf(token,sizeof(token),"\"%s\"",instrument->valuestring);
    } else return 0;
    const cJSON *gain = cJSON_GetObjectItemCaseSensitive(value,"gain");
    double amplitude = cJSON_IsNumber(gain) && isfinite(gain->valuedouble) ? fmax(0,fmin(16,gain->valuedouble)) : .8;
    Pending pending = {.time=time,.duration=fmax(.00001,fmin(60,duration)),.frequency=round(frequency),.amplitude=amplitude*.2};
    snprintf(pending.instrument,sizeof(pending.instrument),"%s",token);
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(value,"_csoundMidi"))) {
        if (frequency <= 0) return 0;
        pending.frequency = fmax(0,fmin(127,69+12*log2(frequency/440)));
        const cJSON *velocity = cJSON_GetObjectItemCaseSensitive(value,"velocity");
        double v = cJSON_IsNumber(velocity) && isfinite(velocity->valuedouble) ? velocity->valuedouble : .9;
        pending.amplitude = fmax(0,fmin(127,127*(cJSON_IsNumber(gain) ? amplitude : 1)*v));
    }
    size_t used = 0;
    const cJSON *field;
    cJSON_ArrayForEach(field,value) {
        if (!field->string || !strcmp(field->string,"freq")) continue;
        char number[64];
        const char *text;
        if (cJSON_IsString(field)) text = field->valuestring;
        else if (cJSON_IsNumber(field) && isfinite(field->valuedouble)) {
            snprintf(number,sizeof(number),"%.9g",field->valuedouble); text = number;
        } else continue;
        const char *parts[] = {field->string,text};
        for (int part = 0; part < 2; part++)
            for (const unsigned char *p = (const unsigned char *)parts[part]; *p; p++)
                if (*p < 32 || *p == '"' || *p == '\\') return 0;
        int n = snprintf(pending.controls+used,sizeof(pending.controls)-used,"%s/%s/",field->string,text);
        if (n < 0 || (size_t)n >= sizeof(pending.controls)-used) return 0;
        used += (size_t)n;
    }
    int midi_mode = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(value,"_csoundMidi"));
    int n = snprintf(pending.controls+used,sizeof(pending.controls)-used,"%s/%.9f",midi_mode ? "frequency" : "freq",midi_mode ? frequency : pending.frequency);
    if (n < 0 || (size_t)n >= sizeof(pending.controls)-used) return 0;
    pthread_mutex_lock(&program->lock);
    int ok = program->count < CS_QUEUE;
    if (ok) program->pending[program->count++] = pending;
    pthread_mutex_unlock(&program->lock);
    return ok;
}
void csound_program_sample(CsoundProgram *program, double *left, double *right) {
    if (!program) return;
    unsigned long frame = atomic_load_explicit(&program->read_frame,memory_order_relaxed);
    unsigned long written = atomic_load_explicit(&program->write_frame,memory_order_acquire);
    if (frame < written) { *left += program->pcm[frame%CS_RING][0]; *right += program->pcm[frame%CS_RING][1]; }
    atomic_store_explicit(&program->read_frame,frame+1,memory_order_release);
}
