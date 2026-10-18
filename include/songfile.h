#ifndef SONGFILE_H
#define SONGFILE_H
#include <stddef.h>

/* Whole-song persistence: every song-list track plus the editor buffer.
   Format v2: {"app":"tompouce","version":2,"bpm":N,"code":"...",
   "tracks":["..."],"active":i}. v1 files (no tracks) load as one track.
   editor_code is stored as "code" and in the active slot, so old
   readers still see the current buffer. */

/* Write path with editor_code + full song list (active slot = editor).
   Returns track count on success, -1 on failure (logs unless quiet). */
int songfile_save(const char *path, const char *editor_code, double bpm, int quiet);

/* Load path: replaces the song list, loads the active buffer into the
   engine (updates current_code). When editor_out is non-NULL it receives
   a malloc'd copy of the active track (caller frees) or NULL on failure.
   bpm_out gets the stored tempo (0 when absent). Returns track count,
   -1 on failure (logs unless quiet). */
int songfile_load(const char *path, char **editor_out, double *bpm_out,
    int quiet);

#endif
