#define _GNU_SOURCE
#include "companion.h"
#include "common.h"
#include "sound_store.h"
#include "mcp.h"
#include <spawn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <limits.h>

extern char **environ;
typedef struct { pid_t child; int output_fd, stopping; char pending[1024]; size_t pending_size; } Helper;
static Helper bridge = {.output_fd = -1}, importer = {.output_fd = -1};

static int helper_poll(Helper *helper, const char *label) {
    if (helper->output_fd >= 0) {
        char bytes[512];
        ssize_t count = -1;
        int bursts = 0;
        while (bursts++ < 8 && (count = read(helper->output_fd,bytes,sizeof(bytes))) > 0) {
            for (ssize_t i = 0; i < count; i++) {
                unsigned char c = (unsigned char)bytes[i];
                if (c == '\n') {
                    helper->pending[helper->pending_size] = 0;
                    if (helper->pending_size) log_console("%s",helper->pending);
                    helper->pending_size = 0;
                } else if (c >= 32 && c != 127 && helper->pending_size+1 < sizeof(helper->pending)) {
                    helper->pending[helper->pending_size++] = (char)c;
                }
            }
        }
        if (count == 0 || (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            close(helper->output_fd); helper->output_fd = -1;
        }
    }
    if (helper->child > 0) {
        int status = 0;
        pid_t result = waitpid(helper->child,&status,WNOHANG);
        if (result == helper->child || (result < 0 && errno == ECHILD)) {
            log_console("%s %s",label,helper->stopping ? "stopped" : "exited; see console for details");
            helper->child = 0; helper->stopping = 0;
            if (helper->output_fd >= 0) { close(helper->output_fd); helper->output_fd = -1; }
            helper->pending_size = 0;
            return result > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 1 : -1;
        }
    }
    return 0;
}

void companion_poll(void) {
    (void)helper_poll(&bridge,"companion");
    if (helper_poll(&importer,"sound import")) (void)user_sounds_reload();
}

static void helper_stop(Helper *helper) {
    if (helper->child > 0 && !helper->stopping) {
        if (kill(helper->child,SIGTERM) == 0) helper->stopping = 1;
        else log_console("could not stop helper: %s",strerror(errno));
    }
}

void companion_stop(void) { companion_poll(); helper_stop(&bridge); }
void companion_shutdown(void) { companion_poll(); helper_stop(&bridge); helper_stop(&importer); }

static int helper_script(const char *name, char *script, size_t size) {
    ssize_t length = readlink("/proc/self/exe",script,size-1);
    if (length < 0 || (size_t)length >= size-1) return 0;
    script[length] = 0;
    char *slash = strrchr(script,'/');
    if (!slash) return 0;
    size_t base = (size_t)(slash-script);
    int n = snprintf(script+base,size-base,"/scripts/%s",name);
    if (n < 0 || (size_t)n >= size-base || access(script,R_OK)) {
        log_console("helper scripts must be next to the executable's directory");
        return 0;
    }
    return 1;
}

static int helper_spawn(Helper *helper, char *const *argv, const char *secret) {
    int pipefd[2];
    if (pipe2(pipefd,O_CLOEXEC)) return 0;
    int flags = fcntl(pipefd[0],F_GETFL);
    if (flags < 0 || fcntl(pipefd[0],F_SETFL,flags|O_NONBLOCK) < 0) {
        close(pipefd[0]); close(pipefd[1]); return 0;
    }
    posix_spawn_file_actions_t actions;
    int credentials[2]={-1,-1};
    if (secret) {
        if (pipe2(credentials,O_CLOEXEC)) { close(pipefd[0]); close(pipefd[1]); return 0; }
        size_t length=strlen(secret);
        ssize_t written;
        do written=write(credentials[1],secret,length); while (written<0 && errno==EINTR);
        close(credentials[1]); credentials[1]=-1;
        if (written!=(ssize_t)length) { close(credentials[0]); close(pipefd[0]); close(pipefd[1]); return 0; }
    }
    int error = posix_spawn_file_actions_init(&actions);
    if (error) { if (credentials[0]>=0) close(credentials[0]); close(pipefd[0]); close(pipefd[1]); return 0; }
    error = secret ? posix_spawn_file_actions_adddup2(&actions,credentials[0],STDIN_FILENO) :
        posix_spawn_file_actions_addopen(&actions,STDIN_FILENO,"/dev/null",O_RDONLY,0);
    if (!error) error = posix_spawn_file_actions_adddup2(&actions,pipefd[1],STDOUT_FILENO);
    if (!error) error = posix_spawn_file_actions_adddup2(&actions,pipefd[1],STDERR_FILENO);
    if (!error) error = posix_spawn_file_actions_addclosefrom_np(&actions,3);
    pid_t launched = 0;
    if (!error) error = posix_spawnp(&launched,"python3",&actions,NULL,argv,environ);
    posix_spawn_file_actions_destroy(&actions);
    if (credentials[0]>=0) close(credentials[0]);
    close(pipefd[1]);
    if (error) {
        close(pipefd[0]); log_console("companion: launch failed: %s",strerror(error)); return 0;
    }
    helper->child = launched; helper->output_fd = pipefd[0]; helper->pending_size = 0;
    return 1;
}

int companion_start(int mcp_port) {
    companion_poll();
    if (bridge.child > 0) {
        log_console(bridge.stopping ? "companion is stopping; try again shortly" : "companion already running (use its console URL)");
        return !bridge.stopping;
    }
    if (mcp_port < 1 || mcp_port > 65535) return 0;
    char script[PATH_MAX], port[16], parent[32];
    if (!helper_script("companion.py",script,sizeof(script))) return 0;
    snprintf(port,sizeof(port),"%d",mcp_port); snprintf(parent,sizeof(parent),"%ld",(long)getpid());
    const char *secret=mcp_tcp_token();
    if (!secret) return 0;
    char *argv[] = {"python3",script,"--mcp-port",port,"--parent-pid",parent,"--mcp-token-stdin","--open",NULL};
    if (!helper_spawn(&bridge,argv,secret)) return 0;
    log_console("starting companion and Chromium...");
    return 1;
}

int companion_import_folder(const char *folder) {
    companion_poll();
    if (importer.child > 0 || !folder || !*folder || strlen(folder) >= PATH_MAX) return 0;
    for (const unsigned char *p = (const unsigned char *)folder; *p; p++) if (*p < 32 || *p == 127) return 0;
    char script[PATH_MAX], parent[32];
    if (!helper_script("sound_import.py",script,sizeof(script))) return 0;
    snprintf(parent,sizeof(parent),"%ld",(long)getpid());
    char *argv[] = {"python3",script,"--folder",(char *)folder,"--parent-pid",parent,NULL};
    if (!helper_spawn(&importer,argv,NULL)) return 0;
    log_console("importing local sounds in background..."); return 1;
}
