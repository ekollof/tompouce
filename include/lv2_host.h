#ifndef LV2_HOST_H
#define LV2_HOST_H
#include <stddef.h>
#include <stdint.h>

/* Confined LV2 discovery. Only metadata is read; no plugin is loaded, and no
   plugin code runs in the engine process. Bundles come from LV2_PATH and the
   user-owned ~/.config/tompouce/lv2 directory. */
typedef struct Lv2Catalog Lv2Catalog;

enum { LV2_NAME_MAX = 128, LV2_URI_MAX = 256,
       LV2_CONTROL_NAME_MAX = 64, LV2_CONTROLS_MAX = 16 };

/* Bounded control-port metadata for a discovered plugin. Values are the
   declared minimum, maximum and default; the index is the LV2 port index. */
typedef struct {
    uint32_t index;
    char name[LV2_CONTROL_NAME_MAX];
    float min, max, value;
} Lv2ControlPort;

/* Scan bundles and retain at most a bounded number of safe audio/MIDI plugins.
   Returns NULL with a message on failure. */
Lv2Catalog *lv2_catalog_scan(char *error, size_t error_size);
void lv2_catalog_free(Lv2Catalog *catalog);
int lv2_catalog_count(const Lv2Catalog *catalog);
/* Copies bounded metadata for one entry. Returns 0 for an invalid index. */
int lv2_catalog_entry(const Lv2Catalog *catalog, int index,
    char uri[LV2_URI_MAX], char name[LV2_NAME_MAX], char library[LV2_URI_MAX],
    int *audio, int *midi);
/* Copies up to max control-port descriptors; returns the total available. */
int lv2_catalog_controls(const Lv2Catalog *catalog, int index,
    Lv2ControlPort *out, int max);

#endif
