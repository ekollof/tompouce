/* Managed Carla process and PipeWire MIDI link.

   Carla owns plugin code and its own audio graph. This host only validates a
   bounded project file, starts one owned process, and connects the engine's
   existing ALSA MIDI output (exposed through PipeWire's Midi-Bridge) to the
   project node's events-in port. Startup and link discovery happen on a
   background thread; the audio callback never waits for an external process. */
#define _GNU_SOURCE
#include "carla_host.h"
#include "common.h"
#include "midi.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CARLA_INSTANCES 4
#define CARLA_PROJECT_NAME 240
#define CARLA_TARGET_NAME 64
#define CARLA_PROJECT_MAX (512*1024)
#define CARLA_START_TIMEOUT_MS 20000
#define CARLA_LINK_INTERVAL_MS 200

enum {
    CARLA_EMPTY=0,
    CARLA_STARTING=1,
    CARLA_READY=2,
    CARLA_FAILED=3
};

typedef struct {
    char project[4096];
    char target[CARLA_TARGET_NAME];
    pid_t pid;
    pthread_t thread;
    int thread_started;
    atomic_int state;
    atomic_int stop;
} CarlaInstance;

static CarlaInstance instances[CARLA_INSTANCES];
static pthread_mutex_t carla_lock=PTHREAD_MUTEX_INITIALIZER;
static atomic_int carla_stopping;

extern char **environ;

static long long monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC,&now);
    return (long long)now.tv_sec*1000+now.tv_nsec/1000000;
}

static void sleep_ms(long milliseconds) {
    struct timespec pause={0,milliseconds*1000000L};
    while (nanosleep(&pause,&pause)!=0 && errno==EINTR) {}
}

/* A project name is a relative ASCII path below the owned root. */
static int carla_name_safe(const char *name) {
    if (!name || !*name || strlen(name)>=CARLA_PROJECT_NAME) return 0;
    const char *component=name;
    for (const char *p=name;;p++) {
        unsigned char c=(unsigned char)*p;
        if (c=='/' || c==0) {
            size_t length=(size_t)(p-component);
            if (length==0 || (length==1 && component[0]=='.') ||
                (length==2 && component[0]=='.' && component[1]=='.')) return 0;
            if (c==0) break;
            component=p+1;
        } else if (!((c>='A' && c<='Z') || (c>='a' && c<='z') ||
                     (c>='0' && c<='9') || c=='.' || c=='_' || c=='-')) {
            return 0;
        }
    }
    return 1;
}

static int carla_root(char *out, size_t outlen) {
    const char *root=getenv("TOMPOUCE_CARLA_ROOT");
    const char *config=getenv("XDG_CONFIG_HOME");
    const char *home=getenv("HOME");
    int length=-1;
    if (root && *root) {
        if (strlen(root)>=outlen) return 0;
        length=snprintf(out,outlen,"%s",root);
    } else if (config && *config) {
        length=snprintf(out,outlen,"%s/tompouce/carla",config);
    } else if (home && *home) {
        length=snprintf(out,outlen,"%s/.config/tompouce/carla",home);
    } else {
        return 0;
    }
    return length>0 && (size_t)length<outlen;
}

/* The real path must remain inside the owned root and be a regular, bounded
   project file. Symlinks that escape the root are rejected. */
static int carla_project_path(const char *name, char *out, size_t outlen) {
    if (!carla_name_safe(name)) return 0;
    char base[2048], candidate[4096], resolved[4096], real_root[4096];
    if (!carla_root(base,sizeof(base)) ||
        snprintf(candidate,sizeof(candidate),"%s/%s",base,name)>=(int)sizeof(candidate) ||
        !realpath(base,real_root) || !realpath(candidate,resolved)) return 0;
    size_t prefix=strlen(real_root);
    if (strncmp(resolved,real_root,prefix) ||
        (resolved[prefix]!='/' && resolved[prefix]!=0)) return 0;
    struct stat st;
    if (stat(resolved,&st) || !S_ISREG(st.st_mode) || st.st_size<0 ||
        st.st_size>CARLA_PROJECT_MAX) return 0;
    if (strlen(resolved)>=outlen) return 0;
    snprintf(out,outlen,"%s",resolved);
    return 1;
}

static int carla_target_safe(const char *target) {
    if (!target || !*target || strlen(target)>=CARLA_TARGET_NAME) return 0;
    for (const unsigned char *p=(const unsigned char *)target;*p;p++)
        if (*p<' ' || *p>126 || *p=='/' || *p=='\\' || *p==':') return 0;
    return 1;
}

static void xml_text(char *out, size_t outlen, const char *start, const char *end) {
    size_t used=0;
    for (const char *p=start;p<end && used+1<outlen;p++) {
        if (*p=='&') {
            const char *entity=p+1;
            if (!strncmp(entity,"amp;",4)) { out[used++]='&'; p+=4; }
            else if (!strncmp(entity,"lt;",3)) { out[used++]='<'; p+=3; }
            else if (!strncmp(entity,"gt;",3)) { out[used++]='>'; p+=3; }
            else if (!strncmp(entity,"quot;",5)) { out[used++]='"'; p+=5; }
            else if (!strncmp(entity,"apos;",5)) { out[used++]='\''; p+=5; }
            else out[used++]=*p;
        } else {
            out[used++]=*p;
        }
    }
    out[used]=0;
    while (used && (out[used-1]==' ' || out[used-1]=='\t' ||
                    out[used-1]=='\r' || out[used-1]=='\n')) out[--used]=0;
}

/* Carla writes the plugin name as the first <Name> in a <Plugin> block. This is
   only a fallback when the pattern does not name the target node explicitly. */
static int carla_default_target(const char *project, char *out, size_t outlen) {
    int fd=open(project,O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if (fd<0) return 0;
    struct stat st;
    if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<0 ||
        st.st_size>CARLA_PROJECT_MAX) { close(fd); return 0; }
    size_t size=(size_t)st.st_size;
    char *text=malloc(size+1);
    if (!text) { close(fd); return 0; }
    size_t used=0;
    while (used<size) {
        ssize_t got=read(fd,text+used,size-used);
        if (got<0 && errno==EINTR) continue;
        if (got<=0) break;
        used+=(size_t)got;
    }
    close(fd);
    text[used]=0;
    int ok=0;
    const char *plugin=strstr(text,"<Plugin");
    const char *plugin_end=plugin?strstr(plugin,"</Plugin>"):NULL;
    const char *name=plugin?strstr(plugin,"<Name>"):NULL;
    const char *close=name?strstr(name,"</Name>"):NULL;
    if (name && close && (!plugin_end || close<plugin_end+9)) {
        name+=6;
        char candidate[CARLA_TARGET_NAME];
        xml_text(candidate,sizeof(candidate),name,close);
        if (carla_target_safe(candidate)) {
            snprintf(out,outlen,"%s",candidate);
            ok=1;
        }
    }
    free(text);
    return ok;
}

static int carla_spawn(CarlaInstance *slot) {
    const char *program=getenv("TOMPOUCE_CARLA");
    if (!program || !*program) program="/usr/bin/carla";
    if (program[0]!='/' || access(program,X_OK)) return 0;
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    if (posix_spawn_file_actions_init(&actions)) return 0;
    if (posix_spawnattr_init(&attributes)) {
        posix_spawn_file_actions_destroy(&actions);
        return 0;
    }
    int devnull=open("/dev/null",O_RDWR|O_CLOEXEC);
    int ok=devnull>=0;
    if (ok) ok=!posix_spawn_file_actions_adddup2(&actions,devnull,STDIN_FILENO);
    if (ok) ok=!posix_spawn_file_actions_adddup2(&actions,devnull,STDOUT_FILENO);
    if (ok) ok=!posix_spawn_file_actions_adddup2(&actions,devnull,STDERR_FILENO);
#ifdef __GLIBC__
    if (ok) ok=!posix_spawn_file_actions_addclosefrom_np(&actions,3);
#endif
    short flags=POSIX_SPAWN_SETPGROUP;
    if (ok) ok=!posix_spawnattr_setflags(&attributes,flags);
    if (ok) ok=!posix_spawnattr_setpgroup(&attributes,0);
    char *argv[]={(char *)program,"-n",slot->project,NULL};
    pid_t pid=-1;
    if (ok) ok=!posix_spawn(&pid,program,&actions,&attributes,argv,environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (devnull>=0) close(devnull);
    if (!ok || pid<=0) return 0;
    slot->pid=pid;
    return 1;
}

static int run_pw_link(const char *source, const char *target, atomic_int *stop) {
    const char *program="/usr/bin/pw-link";
    if (access(program,X_OK)) program="pw-link";
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    if (posix_spawn_file_actions_init(&actions)) return 0;
    if (posix_spawnattr_init(&attributes)) {
        posix_spawn_file_actions_destroy(&actions);
        return 0;
    }
    int devnull=open("/dev/null",O_RDWR|O_CLOEXEC);
    if (devnull<0) {
        posix_spawnattr_destroy(&attributes);
        posix_spawn_file_actions_destroy(&actions);
        return 0;
    }
    posix_spawn_file_actions_adddup2(&actions,devnull,STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions,devnull,STDERR_FILENO);
    char *argv[]={(char *)program,(char *)source,(char *)target,NULL};
    pid_t pid=-1;
    int spawned=posix_spawnp(&pid,program,&actions,&attributes,argv,environ)==0;
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    close(devnull);
    if (!spawned || pid<=0) return 0;
    int status=0;
    long long deadline=monotonic_ms()+3000;
    for (;;) {
        pid_t got=waitpid(pid,&status,WNOHANG);
        if (got==pid) break;
        if (got<0 && errno!=EINTR) { status=-1; break; }
        if (atomic_load(stop) || monotonic_ms()>deadline) {
            kill(pid,SIGKILL);
            while (waitpid(pid,&status,0)<0 && errno==EINTR) {}
            return 0;
        }
        sleep_ms(20);
    }
    return status!=-1 && WIFEXITED(status) && WEXITSTATUS(status)==0;
}

static void carla_kill(CarlaInstance *slot, int force) {
    if (slot->pid<=0) return;
    kill(-slot->pid,force?SIGKILL:SIGTERM);
    kill(slot->pid,force?SIGKILL:SIGTERM);
}

static void *carla_thread(void *argument) {
    CarlaInstance *slot=argument;
    if (!carla_spawn(slot)) {
        atomic_store(&slot->state,CARLA_FAILED);
        log_console("carla: could not start %s",slot->project);
        return NULL;
    }
    char source[256], target[CARLA_TARGET_NAME+16];
    snprintf(target,sizeof(target),"%s:events-in",slot->target);
    int status=0, linked=0, reaped=0;
    long long deadline=monotonic_ms()+CARLA_START_TIMEOUT_MS;
    while (!atomic_load(&slot->stop) && monotonic_ms()<deadline) {
        pid_t got=waitpid(slot->pid,&status,WNOHANG);
        if (got==slot->pid) { reaped=1; break; }
        if (got<0 && errno!=EINTR) break;
        if (midi_ready() && midi_pw_source(source,sizeof(source)) &&
            run_pw_link(source,target,&slot->stop)) {
            linked=1;
            break;
        }
        sleep_ms(CARLA_LINK_INTERVAL_MS);
    }
    if (!linked || reaped) {
        atomic_store(&slot->state,CARLA_FAILED);
        if (!reaped) {
            carla_kill(slot,0);
            sleep_ms(500);
            carla_kill(slot,1);
            while (waitpid(slot->pid,&status,0)<0 && errno==EINTR) {}
        }
        if (!atomic_load(&slot->stop))
            log_console("carla: %s did not provide %s",slot->project,target);
        return NULL;
    }
    atomic_store(&slot->state,CARLA_READY);
    log_console("carla: linked %s -> %s (pid %d)",source,target,(int)slot->pid);
    while (!atomic_load(&slot->stop)) {
        pid_t got=waitpid(slot->pid,&status,WNOHANG);
        if (got==slot->pid) {
            reaped=1;
            atomic_store(&slot->state,CARLA_FAILED);
            log_console("carla: process for %s exited",slot->project);
            break;
        }
        if (got<0 && errno!=EINTR) break;
        sleep_ms(250);
    }
    if (!reaped) {
        carla_kill(slot,0);
        long long end=monotonic_ms()+2000;
        while (monotonic_ms()<end) {
            pid_t got=waitpid(slot->pid,&status,WNOHANG);
            if (got==slot->pid) { reaped=1; break; }
            if (got<0 && errno!=EINTR) { reaped=1; break; }
            sleep_ms(50);
        }
        if (!reaped) {
            carla_kill(slot,1);
            while (waitpid(slot->pid,&status,0)<0 && errno==EINTR) {}
        }
    }
    return NULL;
}

int carla_acquire(const char *project, const char *target,
                  char *error, size_t error_size) {
    if (error && error_size) error[0]=0;
    if (atomic_load(&carla_stopping) || !project || !*project) {
        if (error && error_size) snprintf(error,error_size,"Carla instance is stopping");
        return -1;
    }
    char path[4096], derived[CARLA_TARGET_NAME];
    if (!carla_project_path(project,path,sizeof(path))) {
        if (error && error_size) snprintf(error,error_size,
            "Carla project missing or outside the bounded project root");
        return -1;
    }
    const char *use_target=target;
    if (!use_target || !*use_target) {
        if (!carla_default_target(path,derived,sizeof(derived))) {
            if (error && error_size) snprintf(error,error_size,
                "Carla project needs an explicit target node");
            return -1;
        }
        use_target=derived;
    }
    if (!carla_target_safe(use_target)) {
        if (error && error_size) snprintf(error,error_size,"invalid Carla target node");
        return -1;
    }

    pthread_mutex_lock(&carla_lock);
    if (atomic_load(&carla_stopping)) {
        pthread_mutex_unlock(&carla_lock);
        if (error && error_size) snprintf(error,error_size,"Carla instance is stopping");
        return -1;
    }
    for (int i=0;i<CARLA_INSTANCES;i++) {
        CarlaInstance *slot=&instances[i];
        int state=atomic_load(&slot->state);
        if (state!=CARLA_EMPTY && !strcmp(slot->project,path) &&
            !strcmp(slot->target,use_target)) {
            pthread_mutex_unlock(&carla_lock);
            if (state==CARLA_FAILED) {
                if (error && error_size) snprintf(error,error_size,
                    "Carla project failed to start");
                return -1;
            }
            return i;
        }
    }
    int chosen=-1;
    for (int i=0;i<CARLA_INSTANCES;i++)
        if (atomic_load(&instances[i].state)==CARLA_EMPTY) { chosen=i; break; }
    if (chosen<0) {
        pthread_mutex_unlock(&carla_lock);
        if (error && error_size) snprintf(error,error_size,"managed Carla slots are full");
        return -1;
    }
    CarlaInstance *slot=&instances[chosen];
    snprintf(slot->project,sizeof(slot->project),"%s",path);
    snprintf(slot->target,sizeof(slot->target),"%s",use_target);
    slot->pid=0;
    slot->thread_started=0;
    atomic_store(&slot->stop,0);
    atomic_store(&slot->state,CARLA_STARTING);
    if (pthread_create(&slot->thread,NULL,carla_thread,slot)!=0) {
        atomic_store(&slot->state,CARLA_EMPTY);
        pthread_mutex_unlock(&carla_lock);
        if (error && error_size) snprintf(error,error_size,"managed Carla thread failed");
        return -1;
    }
    slot->thread_started=1;
    log_console("carla: starting %s -> %s",project,use_target);
    pthread_mutex_unlock(&carla_lock);
    return chosen;
}

void carla_stop_all(void) {
    if (atomic_exchange(&carla_stopping,1)) return;
    pthread_mutex_lock(&carla_lock);
    for (int i=0;i<CARLA_INSTANCES;i++) {
        CarlaInstance *slot=&instances[i];
        if (!slot->thread_started) continue;
        atomic_store(&slot->stop,1);
        if (slot->pid>0) carla_kill(slot,0);
    }
    pthread_mutex_unlock(&carla_lock);
    for (int i=0;i<CARLA_INSTANCES;i++) {
        CarlaInstance *slot=&instances[i];
        if (!slot->thread_started) continue;
        pthread_join(slot->thread,NULL);
        pthread_mutex_lock(&carla_lock);
        memset(slot,0,sizeof(*slot));
        pthread_mutex_unlock(&carla_lock);
    }
    atomic_store(&carla_stopping,0);
}

void carla_shutdown(void) {
    carla_stop_all();
}

int carla_active_count(void) {
    int count=0;
    pthread_mutex_lock(&carla_lock);
    for (int i=0;i<CARLA_INSTANCES;i++)
        if (instances[i].thread_started) count++;
    pthread_mutex_unlock(&carla_lock);
    return count;
}

int carla_ready_count(void) {
    int count=0;
    pthread_mutex_lock(&carla_lock);
    for (int i=0;i<CARLA_INSTANCES;i++)
        if (instances[i].thread_started &&
            atomic_load(&instances[i].state)==CARLA_READY) count++;
    pthread_mutex_unlock(&carla_lock);
    return count;
}
