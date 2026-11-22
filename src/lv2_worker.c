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
#include <lv2/presets/presets.h>
#include <lv2/state/state.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define LV2_WORKER_URI_MAX 256
#define LV2_WORKER_PORT_MAX 256
#define LV2_WORKER_EVENT_BYTES 8
#define LV2_WORKER_MIDI_BYTES (LV2_WORKER_MIDI_EVENTS*LV2_WORKER_EVENT_BYTES)

typedef struct {
    uint32_t offset;
    unsigned char size;
    unsigned char data[3];
} Lv2MidiEvent;

typedef struct {
    uint32_t index;
    float min, max, value;
    char name[LV2_WORKER_CONTROL_NAME];
    char symbol[LV2_WORKER_CONTROL_NAME];
} Lv2ControlWire;
#define LV2_WORKER_PRESET 0xFFFFFFFFu
#define LV2_WORKER_PRESET_MAX 128
#define LV2_WORKER_STATE 0xFFFFFFFEu

struct Lv2Worker {
    pid_t pid;
    int in_fd, out_fd;
    Lv2MidiEvent midi[LV2_WORKER_MIDI_EVENTS];
    int midi_count;
    uint32_t caps;
    int control_count;
    Lv2ControlWire controls[LV2_WORKER_CONTROLS_MAX];
};

typedef struct {
    uint32_t frames;
    uint32_t bytes;
    uint32_t caps; /* startup: 1 atom MIDI input, 2 audio input, 4 audio output */
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
    int audio, atom, input, output, control, input_control;
    float value, min, max;
    char name[LV2_WORKER_CONTROL_NAME];
    char symbol[LV2_WORKER_CONTROL_NAME];
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

static int apply_preset(LilvWorld *world, const LilvPlugin *plugin,
                        Lv2PortInfo *ports, unsigned nports,
                        Lv2ControlWire *controls, int control_count,
                        const char *request, Lv2ControlWire *updates,
                        int *update_count) {
    *update_count=0;
    LilvNode *preset_type=lilv_new_uri(world,LV2_PRESETS__Preset);
    LilvNode *label_pred=lilv_new_uri(world,"http://www.w3.org/2000/01/rdf-schema#label");
    LilvNode *name_pred=lilv_new_uri(world,"http://usefulinc.com/ns/doap#name");
    LilvNodes *related=preset_type?lilv_plugin_get_related(plugin,preset_type):NULL;
    const LilvNode *selected=NULL;
    int by_uri=strstr(request,"://")!=NULL || !strncmp(request,"urn:",4);
    unsigned scanned=0;
    LILV_FOREACH(nodes,it,related) {
        const LilvNode *node=lilv_nodes_get(related,it);
        if (++scanned>256) break;
        if (!lilv_node_is_uri(node)) continue;
        if (by_uri) {
            if (!strcmp(lilv_node_as_uri(node),request)) { selected=node; break; }
        } else {
            /* Preset labels live in the resource's data file, which lilv does
               not parse until the resource is explicitly loaded. */
            if (lilv_world_load_resource(world,node)<0) continue;
            LilvNode *label=lilv_world_get(world,node,label_pred,NULL);
            LilvNode *name=lilv_world_get(world,node,name_pred,NULL);
            int match=(label && lilv_node_is_string(label) &&
                !strcasecmp(lilv_node_as_string(label),request)) ||
                (name && lilv_node_is_string(name) &&
                !strcasecmp(lilv_node_as_string(name),request));
            lilv_node_free(label); lilv_node_free(name);
            if (match) { selected=node; break; }
        }
    }
    int ok=0;
    if (selected) {
        if (by_uri) lilv_world_load_resource(world,selected);
        LilvNode *has_port=lilv_new_uri(world,
            "http://lv2plug.in/ns/ext/presets#hasPort");
        LilvNode *symbol_pred=lilv_new_uri(world,
            "http://lv2plug.in/ns/lv2core#symbol");
        LilvNode *value_pred=lilv_new_uri(world,LV2_PRESETS__value);
        LilvNodes *port_nodes=lilv_world_find_nodes(world,selected,has_port,NULL);
        int changed=0;
        LILV_FOREACH(nodes,pit,port_nodes) {
            if (changed>=LV2_WORKER_CONTROLS_MAX) break;
            const LilvNode *port_node=lilv_nodes_get(port_nodes,pit);
            LilvNode *symbol=lilv_world_get(world,port_node,symbol_pred,NULL);
            LilvNode *value=lilv_world_get(world,port_node,value_pred,NULL);
            if (symbol && value && lilv_node_is_string(symbol) &&
                (lilv_node_is_float(value)||lilv_node_is_int(value))) {
                const char *name=lilv_node_as_string(symbol);
                float parsed=lilv_node_is_float(value)?lilv_node_as_float(value):
                    (float)lilv_node_as_int(value);
                for (unsigned pi=0;pi<nports;pi++) {
                    if (!ports[pi].input_control || strcmp(ports[pi].symbol,name)) continue;
                    if (parsed<ports[pi].min) parsed=ports[pi].min;
                    if (parsed>ports[pi].max) parsed=ports[pi].max;
                    ports[pi].value=parsed;
                    for (int ci=0;ci<control_count;ci++)
                        if (controls[ci].index==pi) {
                            controls[ci].value=parsed;
                            updates[changed++]=controls[ci];
                            break;
                        }
                    break;
                }
            }
            lilv_node_free(symbol); lilv_node_free(value);
        }
        if (port_nodes) lilv_nodes_free(port_nodes);
        lilv_node_free(has_port); lilv_node_free(symbol_pred); lilv_node_free(value_pred);
        if (changed) { ok=1; *update_count=changed; }
    }
    if (related) lilv_nodes_free(related);
    lilv_node_free(preset_type); lilv_node_free(label_pred); lilv_node_free(name_pred);
    return ok;
}

static int state_name_safe(const char *name) {
    if (!name || !*name || strlen(name)>=LV2_WORKER_STATE_NAME) return 0;
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

/* State files live in a dedicated root. The resolved path must remain inside
   that root after symlink resolution; traversal components are rejected above. */
static int state_file_path(const char *name, char *out, size_t outlen) {
    if (!state_name_safe(name)) return 0;
    char base[2048];
    const char *root=getenv("TOMPOUCE_LV2_STATE");
    const char *config=getenv("XDG_CONFIG_HOME");
    const char *home=getenv("HOME");
    if (root && *root) {
        if (strlen(root)>=sizeof(base)) return 0;
        snprintf(base,sizeof(base),"%s",root);
    } else if (config && *config) {
        if (snprintf(base,sizeof(base),"%s/tompouce/lv2-state",config)>=(int)sizeof(base))
            return 0;
    } else if (home && *home) {
        if (snprintf(base,sizeof(base),"%s/.config/tompouce/lv2-state",home)>=(int)sizeof(base))
            return 0;
    } else {
        return 0;
    }
    char candidate[4096], resolved[4096], real_root[4096];
    if (snprintf(candidate,sizeof(candidate),"%s/%s",base,name)>=(int)sizeof(candidate) ||
        !realpath(base,real_root) || !realpath(candidate,resolved)) return 0;
    size_t prefix=strlen(real_root);
    if (strncmp(resolved,real_root,prefix) ||
        (resolved[prefix]!='/' && resolved[prefix]!=0)) return 0;
    struct stat st;
    if (stat(resolved,&st) || !(S_ISREG(st.st_mode) || S_ISDIR(st.st_mode))) return 0;
    if (S_ISREG(st.st_mode)) {
        if (st.st_size<0 || st.st_size>256*1024) return 0;
    } else {
        char manifest[4096];
        int length=snprintf(manifest,sizeof(manifest),"%s/manifest.ttl",resolved);
        if (length<0 || (size_t)length>=sizeof(manifest) ||
            stat(manifest,&st) || !S_ISREG(st.st_mode) ||
            st.st_size<0 || st.st_size>256*1024) return 0;
    }
    if (strlen(resolved)>=outlen) return 0;
    snprintf(out,outlen,"%s",resolved);
    return 1;
}

typedef struct {
    Lv2PortInfo *ports;
    unsigned nports;
    Lv2ControlWire *controls;
    int control_count;
    Lv2ControlWire *updates;
    int update_count;
    uint32_t float_type, int_type, bool_type, double_type, long_type;
} StateRestore;

/* Port values from a state file are mapped to the same bounded control metadata
   as live parameter changes; unknown types and unadvertised ports are ignored. */
static void state_port_value(const char *symbol, void *handle, const void *value,
                             uint32_t size, uint32_t type) {
    StateRestore *restore=handle;
    if (!restore || !symbol || !value) return;
    double parsed=0;
    if (size==4 && type==restore->float_type) {
        float number; memcpy(&number,value,sizeof(number)); parsed=number;
    } else if (size==4 && (type==restore->int_type || type==restore->bool_type)) {
        int32_t number; memcpy(&number,value,sizeof(number)); parsed=number;
    } else if (size==8 && type==restore->double_type) {
        double number; memcpy(&number,value,sizeof(number)); parsed=number;
    } else if (size==8 && type==restore->long_type) {
        int64_t number; memcpy(&number,value,sizeof(number)); parsed=(double)number;
    } else {
        return;
    }
    if (!isfinite(parsed)) return;
    for (unsigned pi=0;pi<restore->nports;pi++) {
        if (!restore->ports[pi].input_control ||
            strcmp(restore->ports[pi].symbol,symbol)) continue;
        float number=(float)parsed;
        if (number<restore->ports[pi].min) number=restore->ports[pi].min;
        if (number>restore->ports[pi].max) number=restore->ports[pi].max;
        restore->ports[pi].value=number;
        for (int ci=0;ci<restore->control_count;ci++) {
            if (restore->controls[ci].index!=pi) continue;
            restore->controls[ci].value=number;
            int existing=-1;
            for (int u=0;u<restore->update_count;u++)
                if (restore->updates[u].index==pi) { existing=u; break; }
            if (existing>=0) restore->updates[existing].value=number;
            else if (restore->update_count<LV2_WORKER_CONTROLS_MAX) {
                restore->updates[restore->update_count++]=restore->controls[ci];
            }
            break;
        }
        break;
    }
}

static int apply_state(LilvWorld *world, const LilvPlugin *plugin,
                       LilvInstance *instance, LV2_URID_Map *map,
                       Lv2PortInfo *ports, unsigned nports,
                       Lv2ControlWire *controls, int control_count,
                       const char *name, Lv2ControlWire *updates,
                       int *update_count) {
    *update_count=0;
    char path[4096];
    if (!state_file_path(name,path,sizeof(path))) return 0;
    LilvState *state=lilv_state_new_from_file(world,map,NULL,path);
    if (!state) return 0;
    const LilvNode *state_plugin=lilv_state_get_plugin_uri(state);
    const LilvNode *plugin_uri=lilv_plugin_get_uri(plugin);
    int ok=state_plugin && plugin_uri && lilv_node_is_uri(state_plugin) &&
        lilv_node_is_uri(plugin_uri) &&
        !strcmp(lilv_node_as_uri(state_plugin),lilv_node_as_uri(plugin_uri));
    if (ok) {
        StateRestore restore={.ports=ports,.nports=nports,
            .controls=controls,.control_count=control_count,
            .updates=updates,.update_count=0};
        restore.float_type=map->map(map->handle,LV2_ATOM__Float);
        restore.int_type=map->map(map->handle,LV2_ATOM__Int);
        restore.bool_type=map->map(map->handle,LV2_ATOM__Bool);
        restore.double_type=map->map(map->handle,LV2_ATOM__Double);
        restore.long_type=map->map(map->handle,LV2_ATOM__Long);
        lilv_state_restore(state,instance,state_port_value,&restore,0,NULL);
        *update_count=restore.update_count;
    }
    lilv_state_free(state);
    return ok;
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
    LilvNode *minimum=lilv_new_uri(world,"http://lv2plug.in/ns/lv2core#minimum");
    LilvNode *maximum=lilv_new_uri(world,"http://lv2plug.in/ns/lv2core#maximum");
    LilvNode *control_class=lilv_new_uri(world,"http://lv2plug.in/ns/lv2core#ControlPort");
    LilvNode *atom_port=lilv_new_uri(world,LV2_ATOM__AtomPort);
    LilvNode *midi_event=lilv_new_uri(world,LV2_MIDI__MidiEvent);
    Lv2PortInfo ports[LV2_WORKER_PORT_MAX];
    Lv2ControlWire control_wire[LV2_WORKER_CONTROLS_MAX];
    int control_count=0;
    unsigned nports=lilv_plugin_get_num_ports(plugin);
    if (nports>LV2_WORKER_PORT_MAX) nports=LV2_WORKER_PORT_MAX;
    for (unsigned i=0;i<nports;i++) {
        const LilvPort *port=lilv_plugin_get_port_by_index(plugin,i);
        ports[i].index=i;
        ports[i].audio=ports[i].atom=ports[i].input=ports[i].output=0;
        ports[i].control=0;
        ports[i].input_control=0;
        ports[i].value=0;
        ports[i].min=0;
        ports[i].max=1;
        ports[i].name[0]=0;
        ports[i].symbol[0]=0;
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
        if (ports[i].input && control_class &&
            lilv_port_is_a(plugin,port,control_class)) {
            ports[i].input_control=1;
            const LilvNode *symbol_node=lilv_port_get_symbol(plugin,port);
            if (symbol_node) {
                const char *text=lilv_node_as_string(symbol_node);
                if (text) snprintf(ports[i].symbol,sizeof(ports[i].symbol),"%s",text);
            }
            LilvNodes *low=minimum?lilv_port_get_value(plugin,port,minimum):NULL;
            LilvNodes *high=maximum?lilv_port_get_value(plugin,port,maximum):NULL;
            LilvNode *low_node=low?lilv_nodes_get_first(low):NULL;
            LilvNode *high_node=high?lilv_nodes_get_first(high):NULL;
            float min=low_node && lilv_node_is_float(low_node)?lilv_node_as_float(low_node):0.0f;
            float max=high_node && lilv_node_is_float(high_node)?lilv_node_as_float(high_node):1.0f;
            lilv_nodes_free(low); lilv_nodes_free(high);
            ports[i].min=isfinite(min) && isfinite(max) && max>=min?min:0;
            ports[i].max=isfinite(min) && isfinite(max) && max>=min?max:1;
            if (control_count<LV2_WORKER_CONTROLS_MAX) {
                ports[i].control=1;
                LilvNode *name_node=lilv_port_get_name(plugin,port);
                if (name_node) {
                    const char *text=lilv_node_as_string(name_node);
                    if (text) snprintf(ports[i].name,sizeof(ports[i].name),"%s",text);
                    lilv_node_free(name_node);
                }
                Lv2ControlWire *wire=&control_wire[control_count++];
                wire->index=i;
                wire->min=min;
                wire->max=max;
                wire->value=ports[i].value;
                snprintf(wire->name,sizeof(wire->name),"%s",ports[i].name);
                snprintf(wire->symbol,sizeof(wire->symbol),"%s",ports[i].symbol);
                if (!isfinite(wire->min) || !isfinite(wire->max) || wire->max<wire->min) {
                    control_count--;
                    ports[i].control=0;
                }
            }
        }
        lilv_instance_connect_port(instance,i,&ports[i].value);
    }
    if (!audio || !input || !output || !default_node || !atom_port || !midi_event) return 1;
    /* One bounded atom sequence for MIDI input, shared by any atom ports. */
    enum { MIDI_CAPACITY=4096 };
    unsigned char *atom_storage=calloc(1,MIDI_CAPACITY);
    LV2_Atom_Sequence *sequence=(LV2_Atom_Sequence *)atom_storage;
    if (!sequence) return 1;
    LV2_URID midi_urid=lv2_map_uri(&map_state,LV2_MIDI__MidiEvent);
    LV2_URID sequence_urid=lv2_map_uri(&map_state,LV2_ATOM__Sequence);
    int has_atom_input=0, has_audio_input=0, has_audio_output=0;
    for (unsigned i=0;i<nports;i++) {
        if (ports[i].atom && ports[i].input) {
            has_atom_input=1;
            lilv_instance_connect_port(instance,ports[i].index,sequence);
        }
        if (ports[i].audio) {
            if (ports[i].input) has_audio_input=1;
            if (ports[i].output) has_audio_output=1;
        }
    }
    lilv_instance_activate(instance);
    Lv2FrameHeader ok={.caps=(uint32_t)((has_atom_input?1u:0u)|
        (has_audio_input?2u:0u)|(has_audio_output?4u:0u)),.ok=1};
    uint32_t wire_count=(uint32_t)control_count;
    if (!write_all(out_fd,&ok,sizeof(ok)) ||
        !write_all(out_fd,&wire_count,sizeof(wire_count)) ||
        (control_count && !write_all(out_fd,control_wire,
            (size_t)control_count*sizeof(control_wire[0])))) return 1;
    float *in=malloc(LV2_WORKER_FRAMES*LV2_WORKER_CHANNELS*sizeof(float));
    float *out=malloc(LV2_WORKER_FRAMES*LV2_WORKER_CHANNELS*sizeof(float));
    if (!in || !out) { free(in); free(out); return 1; }
    for (;;) {
        Lv2FrameHeader header;
        if (!read_all(in_fd,&header,sizeof(header))) break;
        if (header.frames==LV2_WORKER_STATE) {
            if (header.bytes==0 || header.bytes>=LV2_WORKER_STATE_NAME) break;
            char request[LV2_WORKER_STATE_NAME]={0};
            if (!read_all(in_fd,request,header.bytes)) break;
            Lv2ControlWire updates[LV2_WORKER_CONTROLS_MAX];
            int update_count=0;
            int state_ok=apply_state(world,plugin,instance,&urid_map,ports,nports,
                control_wire,control_count,request,updates,&update_count);
            Lv2FrameHeader reply={.frames=0,.bytes=0,.caps=0,.ok=state_ok};
            uint32_t update_wire=(uint32_t)update_count;
            if (!write_all(out_fd,&reply,sizeof(reply)) ||
                !write_all(out_fd,&update_wire,sizeof(update_wire)) ||
                (update_count && !write_all(out_fd,updates,
                    (size_t)update_count*sizeof(updates[0])))) break;
            continue;
        }
        if (header.frames==LV2_WORKER_PRESET) {
            if (header.bytes==0 || header.bytes>LV2_WORKER_PRESET_MAX) break;
            char request[LV2_WORKER_PRESET_MAX+1]={0};
            if (!read_all(in_fd,request,header.bytes)) break;
            Lv2ControlWire updates[LV2_WORKER_CONTROLS_MAX];
            int update_count=0;
            int preset_ok=apply_preset(world,plugin,ports,nports,control_wire,
                control_count,request,updates,&update_count);
            Lv2FrameHeader reply={.frames=0,.bytes=0,.caps=0,.ok=preset_ok};
            uint32_t update_wire=(uint32_t)update_count;
            if (!write_all(out_fd,&reply,sizeof(reply)) ||
                !write_all(out_fd,&update_wire,sizeof(update_wire)) ||
                (update_count && !write_all(out_fd,updates,
                    (size_t)update_count*sizeof(updates[0])))) break;
            continue;
        }
        if (header.frames==0) {
            uint32_t port=0; float value=0;
            if (!read_all(in_fd,&port,sizeof(port)) || !read_all(in_fd,&value,sizeof(value))) break;
            if (port<nports && !ports[port].audio) ports[port].value=value;
            header.ok=1; header.bytes=0; header.caps=0;
            if (!write_all(out_fd,&header,sizeof(header))) break;
            continue;
        }
        if (header.frames>LV2_WORKER_FRAMES ||
            header.bytes>LV2_WORKER_MIDI_BYTES ||
            header.bytes%LV2_WORKER_EVENT_BYTES) break;
        if (!read_all(in_fd,in,header.frames*LV2_WORKER_CHANNELS*sizeof(float))) break;
        unsigned char events[LV2_WORKER_MIDI_BYTES]={0};
        if (header.bytes && !read_all(in_fd,events,header.bytes)) break;
        memset(out,0,header.frames*LV2_WORKER_CHANNELS*sizeof(float));
        if (has_atom_input) {
            sequence->atom.type=sequence_urid;
            sequence->atom.size=sizeof(sequence->body);
            sequence->body.unit=0;
            sequence->body.pad=0;
            size_t used=sizeof(LV2_Atom_Sequence);
            unsigned count=header.bytes/LV2_WORKER_EVENT_BYTES;
            for (unsigned e=0;e<count;e++) {
                const unsigned char *wire=events+e*LV2_WORKER_EVENT_BYTES;
                uint32_t offset=0;
                memcpy(&offset,wire,sizeof(offset));
                unsigned size=wire[4];
                if (size<1 || size>3) continue;
                uint32_t at=offset<header.frames?offset:header.frames-1;
                size_t padded=(size+7u)&~7u;
                size_t need=sizeof(LV2_Atom_Event)+padded;
                if (used+need>MIDI_CAPACITY) break;
                LV2_Atom_Event *event=(LV2_Atom_Event *)((unsigned char *)sequence+used);
                event->time.frames=at;
                event->body.type=midi_urid;
                event->body.size=size;
                memcpy(event+1,wire+5,size);
                memset((unsigned char *)(event+1)+size,0,padded-size);
                used+=need;
            }
            sequence->atom.size=(uint32_t)(used-sizeof(LV2_Atom));
        }
        for (unsigned i=0;i<nports;i++) if (ports[i].audio) {
            lilv_instance_connect_port(instance,ports[i].index,
                ports[i].input?in:(ports[i].output?out:NULL));
        }
        lilv_instance_run(instance,header.frames);
        header.ok=1; header.bytes=0; header.caps=0;
        if (!write_all(out_fd,&header,sizeof(header)) ||
            !write_all(out_fd,out,header.frames*LV2_WORKER_CHANNELS*sizeof(float))) break;
    }
    free(in); free(out); free(atom_storage);
    lilv_node_free(midi_event); lilv_node_free(atom_port);
    lilv_instance_free(instance);
    lilv_node_free(control_class); lilv_node_free(maximum); lilv_node_free(minimum);
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
        /* ASan reserves a large shadow address range; the test-only escape
           lets the sanitized worker exercise the host glue. Normal runs keep
           the 2 GiB confinement limit. */
        if (getenv("TOMPOUCE_LV2_WORKER_SANITIZE")) {
            struct rlimit unlimited={RLIM_INFINITY,RLIM_INFINITY};
            setrlimit(RLIMIT_AS,&unlimited);
        } else {
            setrlimit(RLIMIT_AS,&address);
        }
        dup2(to_child[0],STDIN_FILENO); dup2(from_child[1],STDOUT_FILENO);
        close(to_child[0]); close(to_child[1]); close(from_child[0]); close(from_child[1]);
        if (!getenv("TOMPOUCE_LV2_WORKER_SANITIZE")) {
            int devnull=open("/dev/null",O_WRONLY);
            if (devnull>=0) { dup2(devnull,STDERR_FILENO); close(devnull); }
        }
        _exit(child_main(STDIN_FILENO,STDOUT_FILENO));
    }
    close(to_child[0]); close(from_child[1]);
    uint32_t uri_length=(uint32_t)strlen(plugin_uri);
    int ok=write_all(to_child[1],&uri_length,sizeof(uri_length)) &&
        write_all(to_child[1],&sample_rate,sizeof(sample_rate)) &&
        write_all(to_child[1],plugin_uri,uri_length);
    Lv2FrameHeader header;
    uint32_t control_count=0;
    int control_ok=ok && read_all(from_child[0],&header,sizeof(header)) && header.ok &&
        read_all(from_child[0],&control_count,sizeof(control_count)) &&
        control_count<=LV2_WORKER_CONTROLS_MAX;
    Lv2ControlWire controls[LV2_WORKER_CONTROLS_MAX];
    if (control_ok && control_count &&
        !read_all(from_child[0],controls,(size_t)control_count*sizeof(controls[0])))
        control_ok=0;
    if (control_ok) {
        Lv2Worker *worker=calloc(1,sizeof(*worker));
        if (worker) {
            worker->pid=pid; worker->in_fd=to_child[1]; worker->out_fd=from_child[0];
            worker->caps=header.caps;
            worker->control_count=(int)control_count;
            memcpy(worker->controls,controls,(size_t)control_count*sizeof(controls[0]));
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
    unsigned char wire[LV2_WORKER_MIDI_BYTES];
    int wire_bytes=0;
    for (int i=0;i<worker->midi_count;i++) {
        Lv2MidiEvent *event=&worker->midi[i];
        memcpy(wire+wire_bytes,&event->offset,sizeof(event->offset));
        wire[wire_bytes+4]=event->size;
        memcpy(wire+wire_bytes+5,event->data,3);
        wire_bytes+=LV2_WORKER_EVENT_BYTES;
    }
    Lv2FrameHeader header={.frames=frames,.bytes=(uint32_t)wire_bytes,.ok=0};
    size_t bytes=frames*LV2_WORKER_CHANNELS*sizeof(float);
    int written=write_all(worker->in_fd,&header,sizeof(header)) &&
        write_all(worker->in_fd,input,bytes) &&
        (wire_bytes==0 || write_all(worker->in_fd,wire,(size_t)wire_bytes));
    worker->midi_count=0;
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
    return lv2_worker_send_midi_at(worker,0,message,count,error,error_size);
}

int lv2_worker_send_midi_at(Lv2Worker *worker, uint32_t offset,
    const unsigned char *message, int count, char *error, size_t error_size) {
    if (error && error_size) error[0]=0;
    if (!worker || !message || count<1 || count>3 || offset>=LV2_WORKER_FRAMES) {
        if (error && error_size) snprintf(error,error_size,"invalid MIDI message");
        return 0;
    }
    if (worker->midi_count>=LV2_WORKER_MIDI_EVENTS) {
        if (error && error_size) snprintf(error,error_size,"MIDI event queue full");
        return 0;
    }
    int at=worker->midi_count;
    while (at>0 && worker->midi[at-1].offset>offset) {
        worker->midi[at]=worker->midi[at-1];
        at--;
    }
    Lv2MidiEvent *event=&worker->midi[at];
    event->offset=offset; event->size=(unsigned char)count;
    memcpy(event->data,message,(size_t)count);
    worker->midi_count++;
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
int lv2_worker_has_midi(const Lv2Worker *worker) { return worker && (worker->caps&1u); }
int lv2_worker_is_instrument(const Lv2Worker *worker) {
    return worker && (worker->caps&1u) && !(worker->caps&2u) && (worker->caps&4u);
}
int lv2_worker_control_count(const Lv2Worker *worker) {
    return worker?worker->control_count:0;
}
int lv2_worker_load_preset(Lv2Worker *worker, const char *preset,
                           char *error, size_t error_size) {
    if (error && error_size) error[0]=0;
    size_t length=preset?strlen(preset):0;
    if (!worker || !preset || length==0 || length>LV2_WORKER_PRESET_MAX) {
        if (error && error_size) snprintf(error,error_size,"invalid LV2 preset");
        return 0;
    }
    Lv2FrameHeader header={.frames=LV2_WORKER_PRESET,.bytes=(uint32_t)length,.ok=0};
    if (!write_all(worker->in_fd,&header,sizeof(header)) ||
        !write_all(worker->in_fd,preset,length) ||
        !read_all(worker->out_fd,&header,sizeof(header))) {
        if (error && error_size) snprintf(error,error_size,"LV2 preset request failed");
        return 0;
    }
    /* The child always sends the update count after the reply header, even for
       a rejected preset; draining it keeps the control pipe in sync. */
    uint32_t count=0;
    Lv2ControlWire updates[LV2_WORKER_CONTROLS_MAX];
    if (!read_all(worker->out_fd,&count,sizeof(count)) ||
        count>LV2_WORKER_CONTROLS_MAX ||
        (count && !read_all(worker->out_fd,updates,
            (size_t)count*sizeof(updates[0])))) {
        if (error && error_size) snprintf(error,error_size,"LV2 preset response failed");
        return 0;
    }
    if (!header.ok) {
        if (error && error_size) snprintf(error,error_size,"LV2 preset rejected");
        return 0;
    }
    for (uint32_t u=0;u<count;u++)
        for (int i=0;i<worker->control_count;i++)
            if (worker->controls[i].index==updates[u].index) {
                worker->controls[i]=updates[u];
                break;
            }
    return 1;
}

int lv2_worker_load_state(Lv2Worker *worker, const char *name,
                          char *error, size_t error_size) {
    if (error && error_size) error[0]=0;
    size_t length=name?strlen(name):0;
    if (!worker || !name || length==0 || length>=LV2_WORKER_STATE_NAME) {
        if (error && error_size) snprintf(error,error_size,"invalid LV2 state name");
        return 0;
    }
    Lv2FrameHeader header={.frames=LV2_WORKER_STATE,.bytes=(uint32_t)length,.ok=0};
    if (!write_all(worker->in_fd,&header,sizeof(header)) ||
        !write_all(worker->in_fd,name,length) ||
        !read_all(worker->out_fd,&header,sizeof(header))) {
        if (error && error_size) snprintf(error,error_size,"LV2 state request failed");
        return 0;
    }
    uint32_t count=0;
    Lv2ControlWire updates[LV2_WORKER_CONTROLS_MAX];
    if (!read_all(worker->out_fd,&count,sizeof(count)) ||
        count>LV2_WORKER_CONTROLS_MAX ||
        (count && !read_all(worker->out_fd,updates,
            (size_t)count*sizeof(updates[0])))) {
        if (error && error_size) snprintf(error,error_size,"LV2 state response failed");
        return 0;
    }
    if (!header.ok) {
        if (error && error_size) snprintf(error,error_size,"LV2 state rejected");
        return 0;
    }
    for (uint32_t u=0;u<count;u++)
        for (int i=0;i<worker->control_count;i++)
            if (worker->controls[i].index==updates[u].index) {
                worker->controls[i]=updates[u];
                break;
            }
    return 1;
}

int lv2_worker_control(const Lv2Worker *worker, int index, uint32_t *port,
                       char name[LV2_WORKER_CONTROL_NAME], float *min,
                       float *max, float *value) {
    if (!worker || index<0 || index>=worker->control_count) return 0;
    const Lv2ControlWire *control=&worker->controls[index];
    if (port) *port=control->index;
    if (name) snprintf(name,LV2_WORKER_CONTROL_NAME,"%s",control->name);
    if (min) *min=control->min;
    if (max) *max=control->max;
    if (value) *value=control->value;
    return 1;
}
