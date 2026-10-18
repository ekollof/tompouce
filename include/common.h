#ifndef COMMON_H
#define COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <stdbool.h>

#define MAX_TRACKS 32
#define MAX_LINE 512
/* Editor text, song snapshots, and the JS source grow until malloc fails.
   Byte offsets are int, so one buffer stops near 2 GiB. The audio callback
   does not allocate and does not hold that text. */
#define MAX_CONSOLE 256
#define MAX_VOICES 128
#define SR 44100.0

typedef struct { double re, im; } complex_t;

double now_seconds(void);
void log_console(const char *fmt, ...);
void log_console_stderr(int on); /* 0 = ring buffer only (for fullscreen UI) */
char *trim(char *s);
char *strdup_safe(const char *s);

int console_count_get(void);
int console_head_get(void);
const char *console_line(int idx);

/* Create every component of an absolute path (mode 0755). Best effort. */
void mkdir_p(const char *path);

/* Directory under $xdg_env/<xdg_leaf>, or $HOME/<home_leaf> when that env
   is missing. Rejects relative bases, control bytes, and ".." segments.
   Tompouce roots migrate an existing strudelterm directory when the new root
   is absent. Writes "" when no safe base exists or the result does not fit. */
void user_dir(char *out, size_t outlen, const char *xdg_env,
              const char *xdg_leaf, const char *home_leaf);

#endif
