#ifndef PREBAKE_STORE_H
#define PREBAKE_STORE_H
#include <stddef.h>

#define PREBAKE_STORE_MAX 64
#define PREBAKE_STORE_NAME 256

typedef struct {
    char name[PREBAKE_STORE_NAME];
    int enabled;
} PrebakeEntry;

/* List top-level .strudel/.js/.mjs setup files in the owned prebakes
   directory, filtered by a case-insensitive query. Returns the count, or -1
   when the directory is unsafe/unreadable. */
int prebake_store_list(const char *query, PrebakeEntry *out, int capacity);
/* Enable or disable one named file in the owned enabled list. Returns 1 on
   success, 0 when unchanged, -1 on an unsafe directory or write failure. */
int prebake_store_set_enabled(const char *name, int enabled);
/* Build the JS setup source that loads every enabled file in order. Returns
   1 with a malloc-owned string, 0 when nothing is enabled, -1 on failure. */
int prebake_store_enabled_source(char **source, size_t *length);

#endif
