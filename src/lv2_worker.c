/* Confined LV2 plugin worker. The child loads and runs plugin code; the parent
   only sends/receives bounded PCM blocks and never dlopen()s a plugin. */
#define _GNU_SOURCE
#include "lv2_worker.h"
#include "common.h"
#if TOMPOUCE_HAVE_LV2
#include <lilv/lilv.h>
#include <lv2/core/lv2.h>
#include <lv2/urid/urid.h>
#include <lv2/options/options.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define LV2_WORKER_URI_MAX 256
#define LV2_WORKER_PORT_MAX 256

struct Lv2Worker {
    pid_t pid;
    int in_fd, out_fd;
    unsigned char pending_midi[24];
    int pending_bytes;
};

typedef struct {
    uint32_t frames;
    uint32_t bytes;
    int ok;
} Lv2FrameHeader;

static int read_all(int fd, void *data, size_t size) {
    unsigned char *cursor=data;
    while (size) {
        ssize_t got=read(fd,cursor,size);
        if (got<0 && errno==EINTR) continue;
        if (got<=0) return 0;
        cursor+=got; size-=(size_t)got;
    }
    return 1;
}

static int write_all(int fd, const void *data, size_t size) {
    const unsigned char *cursor=data;
    while (size) {
        ssize_t put=write(fd,cursor,size);
        if (put<0 && errno==EINTR) continue;
        if (put<=0) return 0;
        cursor+=put; size-=(size_t)put;
    }
    return 1;
}

#if TOMPOUCE_HAVE_LV2
typedef struct {
    uint32_t index;
    int audio, atom, input, output;
    float value;
} Lv2PortInfo;

#define LV2_URI_TABLE 512
typedef struct {
    char uris[LV2_URI_TABLE][128];
    uint32_t count;
} Lv2UriMapState;

static LV2_URID lv2_map_uri(LV2_URID_Map_Handle handle, const char *uri) {
    Lv2UriMapState *state=handle;
    if (!state || !uri || !*uri) return 0;
    for (uint32_t i=0;i<state->count;i++)
        if (!strcmp(state->uris[i],uri)) return i+1;
    if (state->count<LV2_URI_TABLE && strlen(uri)<sizeof(state->uris[0])) {
        strcpy(state->uris[state->count],uri);
        return ++state->count;
    }
    return 0;
}

static int child_main(int in_fd, int out_fd) {
    uint32_t uri_length=0;
    double rate=0;
    if (!read_all(in_fd,&uri_length,sizeof(uri_length)) ||
        !read_all(in_fd,&rate,sizeof(rate)) ||
        uri_length==0 || uri_length>LV2_WORKER_URI_MAX) return 1;
    char uri[LV2_WORKER_URI_MAX+1]={0};
    if (!read_all(in_fd,uri,uri_length)) return 1;
    LilvWorld *world=lilv_world_new();
    if (!world) return 1;
    lilv_world_load_all(world);
    LilvNode *node=lilv_new_uri(world,uri);
    const LilvPlugin *plugin=node?lilv_plugins_get_by_uri(lilv_world_get_all_plugins(world),node):NULL;
    if (!plugin) { lilv_node_free(node); lilv_world_free(world); return 1; }
    /* Minimal host features required by well-behaved plugins: a bounded URID
       map, basic instantiation options and the bounded-block-length flag. */
    Lv2UriMapState map_state={0};
    LV2_URID_Map urid_map={&map_state,lv2_map_uri};
    float rate_value=(float)rate;
    uint32_t block_value=LV2_WORKER_FRAMES;
    LV2_URID sample_rate_key=lv2_map_uri(&map_state,"http://lv2plug.in/ns/ext/parameters#sampleRate");
    LV2_URID block_key=lv2_map_uri(&map_state,"http://lv2plug.in/ns/ext/buf-size#blockLength");
    LV2_URID float_type=lv2_map_uri(&map_state,"http://lv2plug.in/ns/ext/atom#Float");
    LV2_URID uint_type=lv2_map_uri(&map_state,"http://lv2plug.in/ns/ext/atom#UInt");
    LV2_Options_Option options[]={
        {LV2_OPTIONS_INSTANCE,0,sample_rate_key,sizeof(rate_value),float_type,&rate_value},
        {LV2_OPTIONS_INSTANCE,0,block_key,sizeof(block_value),uint_type,&block_value},
        {0,0,0,0,0,NULL}
    };
    LV2_Feature features[]={
        {LV2_URID__map,&urid_map},
        {LV2_OPTIONS__options,options},
        {LV2_BUF_SIZE__boundedBlockLength,NULL},
        {NULL,NULL}
    };
    const LV2_Feature *feature_list[]={&features[0],&features[1],&features[2],NULL};
    LilvInstance *instance=lilv_plugin_instantiate(plugin,rate,feature_list);
    if (!instance) { lilv_node_free(node); lilv_world_free(world); return 1; }
    LilvNode *audio=lilv_new_uri(world,"http://lv2plug.in/ns/lv2core#AudioPort");
    LilvNode *input=lilv_new_uri(world,"http://lv2plug.in/ns/lv2core#InputPort");
    LilvNode *output=lilv_new_uri(world,"http://lv2plug.in/ns/lv2core#OutputPort");
    LilvNode *default_node=lilv_new_uri(world,"http://lv2plug.in/ns/lv2core#default");
    LilvNode *atom_port=lilv_new_uri(world,LV2_ATOM__AtomPort);
    LilvNode *midi_event=lilv_new_uri(world,LV2_MIDI__MidiEvent);
    Lv2PortInfo ports[LV2_WORKER_PORT_MAX];
    unsigned nports=lilv_plugin_get_num_ports(plugin);
    if (nports>LV2_WORKER_PORT_MAX) nports=LV2_WORKER_PORT_MAX;
    for (unsigned i=0;i<nports;i++) {
        const LilvPort *port=lilv_plugin_get_port_by_index(plugin,i);
        ports[i].index=i;
        ports[i].audio=ports[i].atom=ports[i].input=ports[i].output=0;
        ports[i].value=0;
        if (!port) continue;
        ports[i].audio=lilv_port_is_a(plugin,port,audio);
        ports[i].atom=lilv_port_is_a(plugin,port,atom_port);
        ports[i].input=lilv_port_is_a(plugin,port,input);
        ports[i].output=lilv_port_is_a(plugin,port,output);
        if (ports[i].audio || ports[i].atom) continue;
        LilvNodes *defaults=lilv_port_get_value(plugin,port,default_node);
        LilvNode *first=defaults?lilv_nodes_get_first(defaults):NULL;
        if (first && lilv_node_is_float(first)) ports[i].value=lilv_node_as_float(first);
        else if (first && lilv_node_is_int(first)) ports[i].value=lilv_node_as_int(first);
        lilv_nodes_free(defaults);
        lilv_instance_connect_port(instance,i,&ports[i].value);
    }
    if (!audio || !input || !output || !default_node || !atom_port || !midi_event) return 1;
    lilv_instance_activate(instance);
    Lv2FrameHeader ok={0,0,1};
    if (!write_all(out_fd,&ok,sizeof(ok))) return 1;
    float *in=malloc(LV2_WORKER_FRAMES*LV2_WORKER_CHANNELS*sizeof(float));
    float *out=malloc(LV2_WORKER_FRAMES*LV2_WORKER_CHANNELS*sizeof(float));
    if (!in || !out) { free(in); free(out); return 1; }
    /* One bounded atom sequence for MIDI input, shared by any atom ports. */
    enum { MIDI_CAPACITY=4096 };
    unsigned char *atom_storage=calloc(1,MIDI_CAPACITY);
    LV2_Atom_Sequence *sequence=(LV2_Atom_Sequence *)atom_storage;
    LV2_URID midi_urid=lv2_map_uri(&map_state,LV2_MIDI__MidiEvent);
    LV2_URID sequence_urid=lv2_map_uri(&map_state,LV2_ATOM__Sequence);
    int has_atom_input=0;
    for (unsigned i=0;i<nports;i++) if (ports[i].atom && ports[i].input) {
        has_atom_input=1;
        lilv_instance_connect_port(instance,ports[i].index,sequence);
    }
    for (;;) {
        Lv2FrameHeader header;
        if (!read_all(in_fd,&header,sizeof(header))) break;
        if (header.frames==0) {
            uint32_t port=0; float value=0;
            if (!read_all(in_fd,&port,sizeof(port)) || !read_all(in_fd,&value,sizeof(value))) break;
            if (port<nports && !ports[port].audio) ports[port].value=value;
            header.ok=1; header.bytes=0;
            if (!write_all(out_fd,&header,sizeof(header))) break;
            continue;
        }
        if (header.frames>LV2_WORKER_FRAMES || header.bytes>3*8) break;
        if (!read_all(in_fd,in,header.frames*LV2_WORKER_CHANNELS*sizeof(float))) break;
        unsigned char message[24]={0};
        if (header.bytes && !read_all(in_fd,message,header.bytes)) break;
        memset(out,0,header.frames*LV2_WORKER_CHANNELS*sizeof(float));
        if (has_atom_input) {
            sequence->atom.type=sequence_urid;
            sequence->atom.size=sizeof(sequence->body);
            sequence->body.unit=0;
            sequence->body.pad=0;
            if (header.bytes) {
                LV2_Atom_Event *event=(LV2_Atom_Event *)((unsigned char *)sequence+
                    sizeof(LV2_Atom_Sequence));
                event->time.frames=0;
                event->body.type=midi_urid;
                event->body.size=header.bytes;
                memcpy(event+1,message,header.bytes);
                sequence->atom.size+=sizeof(LV2_Atom_Event)+header.bytes;
            }
        }
        for (unsigned i=0;i<nports;i++) if (ports[i].audio) {
            lilv_instance_connect_port(instance,ports[i].index,
                ports[i].input?in:(ports[i].output?out:NULL));
        }
        lilv_instance_run(instance,header.frames);
        header.ok=1; header.bytes=0;
        if (!write_all(out_fd,&header,sizeof(header)) ||
            !write_all(out_fd,out,header.frames*LV2_WORKER_CHANNELS*sizeof(float))) break;
    }
    free(in); free(out); free(atom_storage);
    lilv_node_free(midi_event); lilv_node_free(atom_port);
    lilv_instance_free(instance);
    lilv_node_free(default_node); lilv_node_free(output);
    lilv_node_free(input); lilv_node_free(audio);
    lilv_node_free(node); lilv_world_free(world);
    return 0;
}
#endif

Lv2Worker *lv2_worker_start(const char *plugin_uri, double sample_rate,
    char *error, size_t error_size) {
    if (error && error_size) error[0]=0;
#if !TOMPOUCE_HAVE_LV2
    if (error && error_size) snprintf(error,error_size,"LV2 support is unavailable");
    return NULL;
#else
    if (!plugin_uri || strlen(plugin_uri)>LV2_WORKER_URI_MAX ||
        !isfinite(sample_rate) || sample_rate<=0) {
        if (error && error_size) snprintf(error,error_size,"invalid LV2 request");
        return NULL;
    }
    int to_child[2], from_child[2];
    if (pipe2(to_child,O_CLOEXEC) || pipe2(from_child,O_CLOEXEC)) {
        if (error && error_size) snprintf(error,error_size,"worker pipe failed");
        return NULL;
    }
    pid_t pid=fork();
    if (pid<0) {
        close(to_child[0]); close(to_child[1]); close(from_child[0]); close(from_child[1]);
        if (error && error_size) snprintf(error,error_size,"worker fork failed");
        return NULL;
    }
    signal(SIGPIPE,SIG_IGN);
    if (pid==0) {
        signal(SIGPIPE,SIG_IGN);
        setpgid(0,0);
        prctl(PR_SET_PDEATHSIG,SIGKILL,0,0,0);
        if (getppid()==1) _exit(127);
        prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0);
        struct rlimit core={0,0}, files={64,64};
        struct rlimit address={2ull*1024*1024*1024,2ull*1024*1024*1024};
        setrlimit(RLIMIT_CORE,&core); setrlimit(RLIMIT_NOFILE,&files);
        setrlimit(RLIMIT_AS,&address);
        dup2(to_child[0],STDIN_FILENO); dup2(from_child[1],STDOUT_FILENO);
        close(to_child[0]); close(to_child[1]); close(from_child[0]); close(from_child[1]);
        int devnull=open("/dev/null",O_WRONLY);
        if (devnull>=0) { dup2(devnull,STDERR_FILENO); close(devnull); }
        _exit(child_main(STDIN_FILENO,STDOUT_FILENO));
    }
    close(to_child[0]); close(from_child[1]);
    uint32_t uri_length=(uint32_t)strlen(plugin_uri);
    int ok=write_all(to_child[1],&uri_length,sizeof(uri_length)) &&
        write_all(to_child[1],&sample_rate,sizeof(sample_rate)) &&
        write_all(to_child[1],plugin_uri,uri_length);
    Lv2FrameHeader header;
    if (ok && read_all(from_child[0],&header,sizeof(header)) && header.ok) {
        Lv2Worker *worker=malloc(sizeof(*worker));
        if (worker) {
            worker->pid=pid; worker->in_fd=to_child[1]; worker->out_fd=from_child[0];
            return worker;
        }
    }
    kill(-pid,SIGKILL); kill(pid,SIGKILL); waitpid(pid,NULL,0);
    close(to_child[1]); close(from_child[0]);
    if (error && error_size) snprintf(error,error_size,"plugin instantiation failed");
    return NULL;
#endif
}

int lv2_worker_process(Lv2Worker *worker, const float *input, float *output,
    unsigned frames, char *error, size_t error_size) {
    if (error && error_size) error[0]=0;
    if (!worker || !input || !output || frames==0 || frames>LV2_WORKER_FRAMES) {
        if (error && error_size) snprintf(error,error_size,"invalid LV2 block");
        return 0;
    }
    Lv2FrameHeader header={.frames=frames,.bytes=(uint32_t)worker->pending_bytes,.ok=0};
    size_t bytes=frames*LV2_WORKER_CHANNELS*sizeof(float);
    int written=write_all(worker->in_fd,&header,sizeof(header)) &&
        write_all(worker->in_fd,input,bytes) &&
        (worker->pending_bytes==0 || write_all(worker->in_fd,worker->pending_midi,
            (size_t)worker->pending_bytes));
    worker->pending_bytes=0;
    if (!written ||
        !read_all(worker->out_fd,&header,sizeof(header)) ||
        !header.ok || !read_all(worker->out_fd,output,bytes)) {
        if (error && error_size) snprintf(error,error_size,"LV2 worker died");
        return 0;
    }
    return 1;
}

int lv2_worker_send_midi(Lv2Worker *worker, const unsigned char *message, int count,
    char *error, size_t error_size) {
    if (error && error_size) error[0]=0;
    if (!worker || !message || count<1 || count>3) {
        if (error && error_size) snprintf(error,error_size,"invalid MIDI message");
        return 0;
    }
    memcpy(worker->pending_midi,message,(size_t)count);
    worker->pending_bytes=count;
    return 1;
}

int lv2_worker_set_control(Lv2Worker *worker, uint32_t port, float value,
    char *error, size_t error_size) {
    if (error && error_size) error[0]=0;
    if (!worker || port>=LV2_WORKER_PORT_MAX || !isfinite(value)) {
        if (error && error_size) snprintf(error,error_size,"invalid control request");
        return 0;
    }
    Lv2FrameHeader header={.frames=0,.bytes=port,.ok=0};
    if (!write_all(worker->in_fd,&header,sizeof(header)) ||
        !write_all(worker->in_fd,&port,sizeof(port)) ||
        !write_all(worker->in_fd,&value,sizeof(value)) ||
        !read_all(worker->out_fd,&header,sizeof(header)) || !header.ok) {
        if (error && error_size) snprintf(error,error_size,"LV2 worker died");
        return 0;
    }
    return 1;
}

void lv2_worker_stop(Lv2Worker *worker) {
    if (!worker) return;
    if (worker->pid>0) {
        kill(-worker->pid,SIGKILL); kill(worker->pid,SIGKILL);
        waitpid(worker->pid,NULL,0);
    }
    close(worker->in_fd); close(worker->out_fd);
    free(worker);
}

pid_t lv2_worker_pid(const Lv2Worker *worker) { return worker?worker->pid:-1; }
