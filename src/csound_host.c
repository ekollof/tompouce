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
enum { CS_COMPILE = 1, CS_EVENT, CS_RENDER };
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
    pthread_t thread;
    int socket, started;
    pid_t child;
    double origin;
    Pending pending[CS_QUEUE];
    unsigned count;
    float pcm[CS_RING][2];
};

/* Private socket protocol, never the application's MCP stdout. */
#ifdef HAVE_CSOUND
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

static void cs_message(CSOUND *engine, int attr, const char *text) {
    (void)attr;
    Reply *reply = csoundGetHostData(engine);
    size_t length = strlen(text), used = strlen(reply->error);
    if (length >= sizeof(reply->error)) { snprintf(reply->error,sizeof(reply->error),"%.1000s",text); return; }
    if (used+length >= sizeof(reply->error)) {
        size_t discard = used+length-(sizeof(reply->error)-1);
        memmove(reply->error,reply->error+discard,used-discard+1); used -= discard;
    }
    memcpy(reply->error+used,text,length+1);
}

/* Orchestra execution is isolated in a child with no file, network,
   process-creation or exec syscalls. Only its private IPC fd is usable.
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
#endif

int csound_worker_main(void) {
#ifndef HAVE_CSOUND
    return 1;
#else
    Reply reply = {0};
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
    if (csoundSetOption(engine,"-n") || csoundSetOption(engine,"-d") || csoundSetOption(engine,"-m0")) return 1;
    /* Retain only the private transport after trusted plugin initialization. */
    close(0); close(1); close(2);
    if (close_range(4,~0u,0)) return 1;
    if (clearenv()) return 1;
    if (!confine_worker()) return 1;
    int initialized = 0;
    for (;;) {
        Message header;
        if (!transfer(3,&header,sizeof(header),0) || header.length > CS_CODE_MAX) break;
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
            if (!transfer(3,&reply,sizeof(reply),1)) { free(data); break; }
        } else if (header.command == CS_EVENT && data && initialized) {
#ifdef HAVE_CSOUND7
            csoundEventString(engine,data,0);
#else
            csoundInputMessage(engine,data);
#endif
        } else if (header.command == CS_RENDER && !data && initialized) {
            float output[CS_BLOCK][2] = {{0}};
            if (!csoundPerformKsmps(engine)) {
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
        free(data);
    }
    /* Exit tears down the confined instance without running plugin finalizers. */
    _exit(0);
#endif
}

#ifdef HAVE_CSOUND
static void *render_worker(void *opaque) {
    CsoundProgram *program = opaque;
    unsigned long frame = 0;
    while (!atomic_load(&program->stop)) {
        unsigned long read_frame = atomic_load_explicit(&program->read_frame,memory_order_acquire);
        if (!atomic_load_explicit(&program->active,memory_order_acquire) || frame >= read_frame+512) {
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
    }
    return NULL;
failed:
    kill(program->child,SIGKILL);
    log_console("Csound: renderer stopped (error or execution timeout)");
    atomic_store(&program->stop,1);
    return NULL;
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
    char *code = calloc(CS_CODE_MAX+1,1);
    if (!code) return NULL;
    size_t used = 0;
    const cJSON *load;
    cJSON_ArrayForEach(load,loads) {
        const cJSON *inline_code = cJSON_GetObjectItemCaseSensitive(load,"code");
        const cJSON *url = cJSON_GetObjectItemCaseSensitive(load,"url");
        char *downloaded = cJSON_IsString(url) ? websamp_fetch_text(url->valuestring) : NULL;
        const char *text = cJSON_IsString(inline_code) ? inline_code->valuestring : downloaded;
        if (!text || strlen(text)+1 > CS_CODE_MAX-used) {
            snprintf(error,size,"Csound: orchestra fetch failed or source exceeds limit"); free(downloaded); free(code); return NULL;
        }
        size_t length = strlen(text);
        memcpy(code+used,text,length); used += length; code[used++] = '\n';
        free(downloaded);
    }
    if (!used) code[used++] = '\n';
    CsoundProgram *program = calloc(1,sizeof(*program));
    if (!program) { free(code); return NULL; }
    program->socket = -1; program->child = -1;
    atomic_init(&program->refs,1); atomic_init(&program->stop,0); atomic_init(&program->active,0);
    atomic_init(&program->read_frame,0); atomic_init(&program->write_frame,0);
    if (pthread_mutex_init(&program->lock,NULL)) { free(code); free(program); return NULL; }
    int sockets[2];
    if (socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,sockets)) goto failed;
    char executable[4096];
    ssize_t length = readlink("/proc/self/exe",executable,sizeof(executable)-1);
    if (length <= 0) { close(sockets[0]); close(sockets[1]); goto failed; }
    executable[length] = 0;
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions)) { close(sockets[0]); close(sockets[1]); goto failed; }
    int action_error = posix_spawn_file_actions_adddup2(&actions,sockets[1],3);
    if (!action_error) action_error = posix_spawn_file_actions_addopen(&actions,0,"/dev/null",O_RDONLY,0);
    if (!action_error) action_error = posix_spawn_file_actions_addopen(&actions,1,"/dev/null",O_WRONLY,0);
    if (!action_error) action_error = posix_spawn_file_actions_addopen(&actions,2,"/dev/null",O_WRONLY,0);
    if (!action_error && sockets[0] != 3) action_error = posix_spawn_file_actions_addclose(&actions,sockets[0]);
    if (!action_error && sockets[1] != 3) action_error = posix_spawn_file_actions_addclose(&actions,sockets[1]);
    char *args[] = {executable,"--csound-worker",NULL};
    extern char **environ;
    int spawned = action_error ? action_error : posix_spawn(&program->child,executable,&actions,NULL,args,environ);
    posix_spawn_file_actions_destroy(&actions);
    close(sockets[1]);
    if (spawned) { close(sockets[0]); goto failed; }
    program->socket = sockets[0];
    Reply reply = {0};
    if (!send_message(program->socket,CS_COMPILE,code,used) || !transfer(program->socket,&reply,sizeof(reply),0)) {
        snprintf(error,size,"Csound: compilation failed or exceeded execution budget"); goto failed;
    }
    if (reply.status) { snprintf(error,size,"Csound: %.1000s",reply.error[0] ? reply.error : "orchestra compilation failed"); goto failed; }
    if (pthread_create(&program->thread,NULL,render_worker,program)) goto failed;
    program->started = 1;
    free(code); return program;
failed:
    free(code); csound_program_free(program); return NULL;
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
    if (program->child > 0) { while (waitpid(program->child,NULL,0) < 0 && errno == EINTR) {} }
    pthread_mutex_destroy(&program->lock); free(program);
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
