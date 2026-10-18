#ifndef WEBSAMP_H
#define WEBSAMP_H
#include "sounds.h"

/* Remote sample maps from Strudel samples(...).
   samples({ bd: 'bd/x.wav' }, 'https://host/pack/')
   samples({ bell: { b4: 'https://host/a.mp3' } })
   samples('github:user/repo')
   samples('https://host/strudel.json')
   The map is replaced on each code load. Audio files download
   into ~/.cache/tompouce/web the first time a name is played.
   http and https only; loopback and private addresses are refused. */

void websamp_load_code(const char *code);
/* JSON array of evaluated samples(...) argument arrays, from the JS host. */
void websamp_load_arguments(const char *json);
/* Bounded text fetch with the same public-URL and redirect policy. Owned. */
char *websamp_fetch_text(const char *url);

/* Sample id, or -1 if this name is not in the map (caller falls
   back to a local bank or the built-in drum). midi < 0 picks the
   nearest pitch to MIDI 36 for a note-keyed name. "bd:1" selects slot 1.
   Plain arrays use MIDI 36 as their playback root, matching Strudel. */
int websamp_acquire(const char *token, double midi);
int websamp_slot_for(const char *token, double midi);

/* 1 if path is a regular file inside the web cache. */
int websamp_is_cache_file(const char *path);

/* Test helpers. github spec includes the "github:" prefix.
   as_json appends strudel.json; otherwise the URL is a directory.
   websamp_default_url reads installed Dirt/piano/drum-machine catalogs
   overlaid with uzu-drumkit. It does not download, and a user
   samples() call does not change it. Index is the slot, not wrapped. */
int websamp_github_to_url(const char *spec, int as_json, char *out, int outlen);
int websamp_url_allowed(const char *url);
int websamp_resolve(const char *base, const char *rel, char *out, int outlen);
int websamp_name_count(void);
int websamp_slot_count(const char *name);
int websamp_slot_url(const char *name, int index, char *out, int outlen);
int websamp_slot_midi(const char *name, int index);
int websamp_default_url(const char *name, int index, char *out, int outlen);
/* Catalog snapshots only: never downloads audio. Explicit user maps win. */
int websamp_list_sounds(SoundInfo *out, int capacity);
unsigned websamp_revision(void);

#endif
