/* Confined LV2 discovery: metadata only, no plugin is loaded or executed.
   Bundles come from the system LV2 paths and the user's owned config dir. */
#include "lv2_host.h"
#include "common.h"
#if TOMPOUCE_HAVE_LV2
#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/midi/midi.h>
#endif
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LV2_MAX_PLUGINS 256

typedef struct {
    char uri[LV2_URI_MAX];
    char name[LV2_NAME_MAX];
    char library[LV2_URI_MAX];
    int audio, midi;
    Lv2ControlPort controls[LV2_CONTROLS_MAX];
    int ncontrols;
} Lv2Entry;

struct Lv2Catalog {
    Lv2Entry entries[LV2_MAX_PLUGINS];
    int count;
};

/* Refuse executable/download metadata: only a plain relative .so inside a
   scanned bundle is acceptable. No shell command or remote fetch is allowed. */
#if TOMPOUCE_HAVE_LV2
static const char *library_path(const LilvNode *library) {
    const char *uri=library?lilv_node_as_string(library):NULL;
    if (!uri) return NULL;
    /* file:// URIs are the normal form for a bundle's lv2:binary. */
    if (!strncmp(uri,"file://",7)) return uri+7;
    return uri;
}
#endif

#if TOMPOUCE_HAVE_LV2
static float node_float(const LilvNode *node, float fallback) {
    if (!node) return fallback;
    if (lilv_node_is_float(node)) return lilv_node_as_float(node);
    if (lilv_node_is_int(node)) return (float)lilv_node_as_int(node);
    return fallback;
}
#endif

static int safe_library(const char *path) {
    if (!path || !*path || strstr(path,"..") || strstr(path,"://")) return 0;
    size_t length=strlen(path);
    return length>3 && length<LV2_URI_MAX && !strcmp(path+length-3,".so");
}

Lv2Catalog *lv2_catalog_scan(char *error, size_t error_size) {
    if (error && error_size) error[0]=0;
#if !TOMPOUCE_HAVE_LV2
    if (error && error_size) snprintf(error,error_size,"LV2 support is unavailable");
    return NULL;
#else
    LilvWorld *world=lilv_world_new();
    if (!world) {
        if (error && error_size) snprintf(error,error_size,"cannot initialize LV2 world");
        return NULL;
    }
    Lv2Catalog *catalog=calloc(1,sizeof(*catalog));
    if (!catalog) { lilv_world_free(world); return NULL; }
    LilvNode *audio=lilv_new_uri(world,"http://lv2plug.in/ns/lv2core#AudioPort");
    LilvNode *event=lilv_new_uri(world,"http://lv2plug.in/ns/lv2core#EventPort");
    LilvNode *atom=lilv_new_uri(world,LV2_ATOM__AtomPort);
    LilvNode *midi=lilv_new_uri(world,LV2_MIDI__MidiEvent);
    LilvNode *supports=lilv_new_uri(world,LV2_ATOM__supports);
    LilvNode *input=lilv_new_uri(world,"http://lv2plug.in/ns/lv2core#InputPort");
    LilvNode *output=lilv_new_uri(world,"http://lv2plug.in/ns/lv2core#OutputPort");
    LilvNode *control=lilv_new_uri(world,"http://lv2plug.in/ns/lv2core#ControlPort");
    LilvNode *minimum=lilv_new_uri(world,"http://lv2plug.in/ns/lv2core#minimum");
    LilvNode *maximum=lilv_new_uri(world,"http://lv2plug.in/ns/lv2core#maximum");
    LilvNode *default_value=lilv_new_uri(world,"http://lv2plug.in/ns/lv2core#default");
    if (!audio || !event || !input || !output || !atom || !midi || !supports ||
        !control || !minimum || !maximum || !default_value) {
        lilv_node_free(default_value); lilv_node_free(maximum);
        lilv_node_free(minimum); lilv_node_free(control);
        lilv_node_free(supports); lilv_node_free(midi); lilv_node_free(atom);
        lilv_node_free(output); lilv_node_free(input);
        lilv_node_free(event); lilv_node_free(audio);
        lilv_world_free(world); free(catalog);
        if (error && error_size) snprintf(error,error_size,"cannot allocate LV2 port classes");
        return NULL;
    }
    lilv_world_load_all(world);
    const LilvPlugins *plugins=lilv_world_get_all_plugins(world);
    if (plugins) {
        LILV_FOREACH(plugins,iterator,plugins) {
            const LilvPlugin *plugin=lilv_plugins_get(plugins,iterator);
            if (!plugin || catalog->count>=LV2_MAX_PLUGINS) continue;
            const LilvNode *library=lilv_plugin_get_library_uri(plugin);
            const char *library_text=library_path(library);
            if (!safe_library(library_text)) continue;
            int audio_ok=0, midi_ok=0, controls_found=0;
            Lv2ControlPort controls[LV2_CONTROLS_MAX] = {{0}};
            unsigned ports=lilv_plugin_get_num_ports(plugin);
            for (unsigned i=0;i<ports;i++) {
                const LilvPort *port=lilv_plugin_get_port_by_index(plugin,i);
                if (!port) continue;
                int is_input=lilv_port_is_a(plugin,port,input);
                int is_output=lilv_port_is_a(plugin,port,output);
                if (!is_input && !is_output) continue;
                if (lilv_port_is_a(plugin,port,audio)) audio_ok=1;
                if (lilv_port_is_a(plugin,port,event)) midi_ok=1;
                if (lilv_port_is_a(plugin,port,atom) &&
                    lilv_port_supports_event(plugin,port,midi)) midi_ok=1;
                /* Metadata only: read declared control-port ranges and names. */
                if (is_input && controls_found<LV2_CONTROLS_MAX &&
                    lilv_port_is_a(plugin,port,control) &&
                    !lilv_port_is_a(plugin,port,audio) &&
                    !lilv_port_is_a(plugin,port,atom) &&
                    !lilv_port_is_a(plugin,port,event)) {
                    Lv2ControlPort *cp=&controls[controls_found];
                    cp->index=i;
                    LilvNode *name_node=lilv_port_get_name(plugin,port);
                    if (name_node) {
                        const char *text=lilv_node_as_string(name_node);
                        if (text) snprintf(cp->name,sizeof(cp->name),"%s",text);
                        lilv_node_free(name_node);
                    }
                    LilvNodes *low=lilv_port_get_value(plugin,port,minimum);
                    LilvNodes *high=lilv_port_get_value(plugin,port,maximum);
                    LilvNodes *value=lilv_port_get_value(plugin,port,default_value);
                    cp->min=node_float(low?lilv_nodes_get_first(low):NULL,0.0f);
                    cp->max=node_float(high?lilv_nodes_get_first(high):NULL,1.0f);
                    cp->value=node_float(value?lilv_nodes_get_first(value):NULL,cp->min);
                    lilv_nodes_free(low); lilv_nodes_free(high); lilv_nodes_free(value);
                    if (!isfinite(cp->min) || !isfinite(cp->max) ||
                        !isfinite(cp->value) || cp->max < cp->min) continue;
                    controls_found++;
                }
            }
            if (!audio_ok && !midi_ok) continue;
            const LilvNode *uri=lilv_plugin_get_uri(plugin);
            LilvNode *name=lilv_plugin_get_name(plugin);
            if (!uri || !name) { lilv_node_free(name); continue; }
            Lv2Entry *entry=&catalog->entries[catalog->count];
            const char *uri_text=lilv_node_as_string(uri), *name_text=lilv_node_as_string(name);
            if (!uri_text || strlen(uri_text)>=sizeof(entry->uri) ||
                !name_text || strlen(name_text)>=sizeof(entry->name)) {
                lilv_node_free(name); continue;
            }
            strcpy(entry->uri,uri_text); strcpy(entry->name,name_text);
            strcpy(entry->library,library_text);
            entry->audio=audio_ok; entry->midi=midi_ok;
            entry->ncontrols=controls_found;
            memcpy(entry->controls,controls,sizeof(controls));
            catalog->count++;
            lilv_node_free(name);
        }
    }
    lilv_node_free(default_value); lilv_node_free(maximum);
    lilv_node_free(minimum); lilv_node_free(control);
    lilv_node_free(supports); lilv_node_free(midi); lilv_node_free(atom);
    lilv_node_free(output); lilv_node_free(input);
    lilv_node_free(event); lilv_node_free(audio);
    lilv_world_free(world);
    return catalog;
#endif
}

void lv2_catalog_free(Lv2Catalog *catalog) { free(catalog); }

int lv2_catalog_count(const Lv2Catalog *catalog) { return catalog ? catalog->count : 0; }

int lv2_catalog_entry(const Lv2Catalog *catalog, int index,
    char uri[LV2_URI_MAX], char name[LV2_NAME_MAX], char library[LV2_URI_MAX],
    int *audio, int *midi) {
    if (!catalog || index<0 || index>=catalog->count) return 0;
    const Lv2Entry *entry=&catalog->entries[index];
    if (uri) strcpy(uri,entry->uri);
    if (name) strcpy(name,entry->name);
    if (library) strcpy(library,entry->library);
    if (audio) *audio=entry->audio;
    if (midi) *midi=entry->midi;
    return 1;
}

int lv2_catalog_controls(const Lv2Catalog *catalog, int index,
    Lv2ControlPort *out, int max) {
    if (!catalog || index<0 || index>=catalog->count) return 0;
    const Lv2Entry *entry=&catalog->entries[index];
    int count=entry->ncontrols;
    if (out && max>0) {
        int copy=count<max?count:max;
        memcpy(out,entry->controls,(size_t)copy*sizeof(*out));
    }
    return count;
}
