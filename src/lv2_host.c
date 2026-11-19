/* Confined LV2 discovery: metadata only, no plugin is loaded or executed.
   Bundles come from the system LV2 paths and the user's owned config dir. */
#include "lv2_host.h"
#include "common.h"
#if TOMPOUCE_HAVE_LV2
#include <lilv/lilv.h>
#endif
#include <stdlib.h>
#include <string.h>

#define LV2_MAX_PLUGINS 256

typedef struct {
    char uri[LV2_URI_MAX];
    char name[LV2_NAME_MAX];
    char library[LV2_URI_MAX];
    int audio, midi;
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
    LilvNode *input=lilv_new_uri(world,"http://lv2plug.in/ns/lv2core#InputPort");
    LilvNode *output=lilv_new_uri(world,"http://lv2plug.in/ns/lv2core#OutputPort");
    if (!audio || !event || !input || !output) {
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
            int audio_ok=0, midi_ok=0;
            unsigned ports=lilv_plugin_get_num_ports(plugin);
            for (unsigned i=0;i<ports;i++) {
                const LilvPort *port=lilv_plugin_get_port_by_index(plugin,i);
                if (!port) continue;
                int is_input=lilv_port_is_a(plugin,port,input);
                int is_output=lilv_port_is_a(plugin,port,output);
                if (!is_input && !is_output) continue;
                if (lilv_port_is_a(plugin,port,audio)) audio_ok=1;
                if (lilv_port_is_a(plugin,port,event)) midi_ok=1;
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
            catalog->count++;
            lilv_node_free(name);
        }
    }
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
