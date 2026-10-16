#ifndef BANKS_H
#define BANKS_H

/* Background index of playable sample files under assets/banks/.
   Thread-safe: call banks_start_index() once, poll banks_ready(). */
void banks_start_index(const char *root);
int banks_ready(void);      /* 1 when scan finished (or failed) */
int banks_count(void);      /* entries indexed so far */
int banks_failed(void);     /* 1 if root missing/unreadable */
/* Fill out[] with up to max matching relative paths (case-insensitive
   substring). Returns total match count (may exceed max). */
int banks_search(const char *query, const char **out, int max);

/* Distinct bank dirs matching query (case-insensitive substring on the
   dir or anything under it). Children with a matching parent are
   collapsed, so results are banks/kits, not kit pieces. Empty query
   lists top-level banks. dirs filled up to max (512B each), counts get
   subtree file totals. Returns total distinct count. */
int banks_search_dirs(const char *query, char dirs[][512], int *counts, int max);

/* List immediate subdirectories of a banks-relative dir ("" = root),
   basenames matching query (case-insensitive, "" matches all), sorted
   A-Z. out gets full rel paths (up to max). Returns count, -1 if the
   dir is missing/unreadable. */
int banks_list_dir(const char *reldir, const char *query, char out[][512], int max);

/* Start background index using STRUDELTERM_BANKS, ./assets/banks, or the
   repo checkout path (first existing dir wins). Safe to call repeatedly. */
void banks_ensure_default(void);

/* Re-scan from scratch (picks up added/removed banks). Returns 1 if
   a scan was started, 0 if one is already running. */
int banks_rescan(void);

/* Resolved root in use ("" if none). Valid after banks_ensure_default. */
const char *banks_root_dir(void);

#endif
