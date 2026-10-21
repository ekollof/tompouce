#include "tui.h"
#include "track.h"
#include "scheduler.h"
#include "slider.h"
#include "audio.h"
#include "convolution_mix.h"
#include "common.h"
#include "ai.h"
#include "mcp.h"
#include "companion.h"
#include "sounds.h"
#include "wavetable.h"
#include "banks.h"
#include "sampler.h"
#include "midi.h"
#include "songfile.h"
#include "terminal_graphics.h"
#include <cjson/cJSON.h>
#include <poll.h>
#include "tui_terminal.h"
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <termios.h>
#include <errno.h>
#include <stdint.h>
#include <limits.h>
#include <tree_sitter/api.h>
const TSLanguage *tree_sitter_javascript(void);
extern int mcp_input_fd;
extern FILE *mcp_out_stream;

/* One malloc'd NUL string per line. A failed edit leaves the buffer as it was.
   Offsets are int, so the flat text stops near 2 GiB. */
static char **code_lines;
static int code_line_cap;
static char *last_synced_code;
static char *editor_clean;
static char *editor_written; /* File/initial-buffer baseline, independent of F5. */
static int editor_wrap = 1; /* 1 = wrap long lines, 0 = scroll with the cursor */
static int view_col;
/* Hardware cursor, in screen cells, set by draw_code. 0 when the editor is not showing it. */
static int editor_cur_r, editor_cur_c, editor_cur_on;
/* 1 = a typing run (insert, delete, enter). 0 = its own history step. */
#define UNDO_TYPE 1
struct undo_snap {
    char *text;
    int line, col, top, vcol;
};
static struct undo_snap *undo_s, *redo_s;
static int undo_n, undo_cap, redo_n, redo_cap;
static int undo_group, undo_suspend, undo_pushed;
static unsigned undo_tick, undo_group_tick;
static double undo_group_time;
static int vi_mode = 0; /* 0 = insert, 1 = vi-ish normal */
static int vi_normal = 0;
static int vi_pending = 0;  /* for dd/yy */
static void command_prompt_open(void);
static int command_path(const char *arg, char *path, size_t capacity);
static int command_modified(void);
static void vi_reset(void);
static int vi_prev(const char *s, int at);
static int vi_next(const char *s, int at, int n);
static char yank_blank[] = "";
static char *vi_yank = yank_blank;
static int yank_lines;
static int sel_active, sel_linewise, sel_line, sel_col, sel_inclusive;
static int hl_line;
static int mouse_enabled = 1, pane_focus, mouse_drag, mouse_slider = -1;
static int mouse_x, mouse_y;
static int tty_active, smg_active, saved_tty_valid;
static struct termios saved_tty;
static volatile sig_atomic_t quit_requested;
static void handle_quit(int sig) { quit_requested = sig; }
static int selected_byte(int line, int col);
static int do_evaluate(void);
static void do_perform(void);
static int show_docs = 0;
static int show_console = 1;
static int show_state = 0;
static int show_tracks = 0;
static int show_viz = 0;
static int editor_scope = 1;
static int inline_visuals = 1;
static unsigned char *inline_lines;
static int inline_capacity, inline_count;
typedef struct { int offset[VISUAL_KINDS]; unsigned mask[VISUAL_KINDS], generation; } InlineFeed;
static InlineFeed *inline_feeds;
static char *inline_source;
static unsigned inline_bound_generation;
static unsigned inline_bound_revision;
static int viz_h = 0;
static int viz_utf8 = 0;
static int graphics_disabled, graphics_supported;
static int graphics_redraw, graphics_cell_width = 8, graphics_cell_height = 16;
typedef struct {
    int line, kind, row, col, width, first, count, cell_width, cell_height;
    unsigned feed, generation;
} InlineGraphic;
static InlineGraphic graphics_pending[GRAPHICS_IMAGES], graphics_previous[GRAPHICS_IMAGES];
static int graphics_pending_count, graphics_previous_count;
static double graphics_last_frame;
#define GRAPHICS_FRAME_SECONDS (1.0/60.0)
static void inline_graphics_clear(void);

static void graphics_read_palette(void) {
    /* Notcurses owns OSC input. Import its startup snapshot rather than racing
       its input thread for terminal replies. Optional theme-file watching can
       replace this baseline; backgrounds retain terminal transparency. */
    uint32_t rgb;
    char reply[48];
    if (!notcurses_default_foreground(terminal_nc,&rgb)) {
        snprintf(reply,sizeof(reply),"10;#%06x",rgb&0xffffffu);
        terminal_graphics_palette_reply(reply);
    }
    if (!notcurses_default_background(terminal_nc,&rgb)) {
        snprintf(reply,sizeof(reply),"11;#%06x",rgb&0xffffffu);
        terminal_graphics_palette_reply(reply);
    }
    ncpalette *palette=ncpalette_new(terminal_nc);
    if (palette) {
        for (int i=0;i<16;i++) {
            ncpalette_get(palette,i,&rgb);
            if (!rgb && i) continue;
            snprintf(reply,sizeof(reply),"4;%d;#%06x",i,rgb);
            terminal_graphics_palette_reply(reply);
        }
        ncpalette_free(palette);
    }
}

static void graphics_probe(void) {
    /* Our transport sends Kitty images; Notcurses owns capability replies. */
    ncpixelimpl_e pixels=notcurses_check_pixel_support(terminal_nc);
    graphics_supported=!graphics_disabled && !getenv("TMUX") && !getenv("STY") &&
        (pixels==NCPIXEL_KITTY_STATIC || pixels==NCPIXEL_KITTY_ANIMATED || pixels==NCPIXEL_KITTY_SELFREF);
    graphics_redraw=1;
}
static double split_ratio = 0.6; /* code pane fraction; F10/F11 adjust */
static int *line_err;
static int line_err_cap;
static char first_err[256] = {0};
static int docs_scroll = 0;
static int console_scroll;
static int search_mode = 0;
static char search_buf[256] = {0};
static int search_len = 0;
static int search_sel = 0;
static int search_dirty = 1;
static int search_lastcount = -1;
static char search_dirs[256][512];
static int search_dircounts[256];
static int search_ndirs = 0;       /* shown */
static int search_ndirs_total = 0; /* all matches */
static char search_path[1024] = {0}; /* "" = global flat search */
static int search_category = -1;
static unsigned search_revision;
static SoundBrowserRow search_rows[256];
/* Mouse hits use the rendered layout, including wrapped tabs and scroll. */
static int search_list_row, search_list_top, search_list_visible, search_list_count;
static int file_mode = 0; /* 0 off, 1 save picker, 2 load picker */
static char file_buf[256] = {0};
static int file_len = 0;
static char file_names[256][256]; /* filtered *.json basenames */
static int file_n = 0;
static int file_sel = 0;
static int file_dirty = 1;
static char project_path[1024] = "tompouce-song.json";
static volatile sig_atomic_t resize_flag = 0;
static void handle_winch(int sig) { (void)sig; resize_flag = 1; }
static int code_line_count = 1;
static int cur_line = 0, cur_col = 0;
static int view_top = 0;
static char input_buf[1024];
static int input_len = 0;
static int input_mode = 0;
static char status_msg[256] = "Ready";
#define AI_PROMPT_MAX 240

static int rows, cols;

#define SOURCE_MAX ((size_t)INT_MAX)

static const char *cstr(const char *s) { return s ? s : ""; }

static int owned_set(char **slot, const char *s) {
    size_t n = strlen(cstr(s));
    if (n > SOURCE_MAX) return 0;
    char *p = malloc(n + 1);
    if (!p) return 0;
    memcpy(p, cstr(s), n + 1);
    free(*slot);
    *slot = p;
    return 1;
}

static char *xdup_n(const char *s, size_t n) {
    if (n > SOURCE_MAX) return NULL;
    char *p = malloc(n + 1);
    if (!p) return NULL;
    if (n) memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static char *xdup(const char *s) {
    s = cstr(s);
    return xdup_n(s, strlen(s));
}

static int lines_reserve(int n) {
    if (n <= code_line_cap) return 1;
    if (n < 1 || (size_t)n > SIZE_MAX / sizeof(char *)) return 0;
    int cap = code_line_cap > 0 ? code_line_cap : 8;
    while (cap < n) {
        if (cap > INT_MAX / 2) { cap = n; break; }
        cap *= 2;
    }
    char **np = realloc(code_lines, (size_t)cap * sizeof(char *));
    if (!np) return 0;
    for (int i = code_line_cap; i < cap; i++) np[i] = NULL;
    code_lines = np;
    code_line_cap = cap;
    return 1;
}

static int lines_ready(void) {
    if (code_line_count < 1) code_line_count = 1;
    if (!lines_reserve(code_line_count)) return 0;
    for (int i = 0; i < code_line_count; i++) {
        if (!code_lines[i]) {
            code_lines[i] = xdup("");
            if (!code_lines[i]) return 0;
        }
    }
    return 1;
}

static int line_insert(int at, const char *s) {
    if (!lines_ready()) return 0;
    if (at < 0) at = 0;
    if (at > code_line_count) at = code_line_count;
    char *neu = xdup(s);
    if (!neu) return 0;
    if (!lines_reserve(code_line_count + 1)) { free(neu); return 0; }
    memmove(&code_lines[at + 1], &code_lines[at],
        (size_t)(code_line_count - at) * sizeof(char *));
    code_lines[at] = neu;
    code_line_count++;
    return 1;
}

static int line_delete(int at) {
    if (!lines_ready() || at < 0 || at >= code_line_count) return 0;
    if (code_line_count == 1) {
        char *p = xdup("");
        if (!p) return 0;
        free(code_lines[0]);
        code_lines[0] = p;
        return 1;
    }
    free(code_lines[at]);
    memmove(&code_lines[at], &code_lines[at + 1],
        (size_t)(code_line_count - at - 1) * sizeof(char *));
    code_line_count--;
    code_lines[code_line_count] = NULL;
    return 1;
}

static int line_splice(int i, int col, int remove, const char *add, int addlen) {
    if (!lines_ready() || i < 0 || i >= code_line_count) return 0;
    size_t L = strlen(code_lines[i]);
    if (col < 0) col = 0;
    if ((size_t)col > L) col = (int)L;
    if (remove < 0) remove = 0;
    if ((size_t)col + (size_t)remove > L) remove = (int)(L - (size_t)col);
    if (!add || addlen < 0) addlen = 0;
    if ((size_t)addlen > SOURCE_MAX || L - (size_t)remove > SOURCE_MAX - (size_t)addlen)
        return 0;
    size_t nlen = L - (size_t)remove + (size_t)addlen;
    char *p = malloc(nlen + 1);
    if (!p) return 0;
    memcpy(p, code_lines[i], (size_t)col);
    if (addlen) memcpy(p + col, add, (size_t)addlen);
    memcpy(p + col + addlen, code_lines[i] + col + remove,
        L - (size_t)col - (size_t)remove + 1);
    free(code_lines[i]);
    code_lines[i] = p;
    return 1;
}

static int line_join_next(int i) {
    if (!lines_ready() || i < 0 || i + 1 >= code_line_count) return 0;
    size_t a = strlen(code_lines[i]);
    size_t b = strlen(code_lines[i + 1]);
    if (a > SOURCE_MAX - b) return 0;
    char *p = malloc(a + b + 1);
    if (!p) return 0;
    memcpy(p, code_lines[i], a);
    memcpy(p + a, code_lines[i + 1], b + 1);
    char *old = code_lines[i];
    code_lines[i] = p;
    if (!line_delete(i + 1)) {
        code_lines[i] = old;
        free(p);
        return 0;
    }
    free(old);
    return 1;
}

static int line_split(int i, int col) {
    if (!lines_ready() || i < 0 || i >= code_line_count) return 0;
    size_t L = strlen(code_lines[i]);
    if (col < 0) col = 0;
    if ((size_t)col > L) col = (int)L;
    char *rest = xdup(code_lines[i] + col);
    char *head = xdup_n(code_lines[i], (size_t)col);
    if (!rest || !head || !lines_reserve(code_line_count + 1)) {
        free(rest); free(head); return 0;
    }
    memmove(&code_lines[i + 2], &code_lines[i + 1],
        (size_t)(code_line_count - i - 1) * sizeof(char *));
    free(code_lines[i]);
    code_lines[i] = head;
    code_lines[i + 1] = rest;
    code_line_count++;
    return 1;
}

static int editor_offset(int line, int col) {
    if (line < 0 || col < 0) return -1;
    size_t at = (size_t)col;
    int lim = line < code_line_count ? line : code_line_count;
    for (int i = 0; i < lim; i++) {
        size_t n = code_lines && code_lines[i] ? strlen(code_lines[i]) : 0;
        if (at > SOURCE_MAX - n - 1) return -1;
        at += n + 1;
    }
    if (at > SOURCE_MAX) return -1;
    return (int)at;
}

static char *editor_flatten(void) {
    if (!lines_ready()) return NULL;
    size_t n = 0;
    for (int i = 0; i < code_line_count; i++) {
        size_t L = strlen(code_lines[i]);
        if (n > SOURCE_MAX - L - 1) return NULL;
        n += L + 1;
    }
    char *out = malloc(n + 1);
    if (!out) return NULL;
    size_t pos = 0;
    for (int i = 0; i < code_line_count; i++) {
        size_t L = strlen(code_lines[i]);
        memcpy(out + pos, code_lines[i], L);
        pos += L;
        out[pos++] = '\n';
    }
    out[pos] = 0;
    return out;
}

static void undo_hist_clear(struct undo_snap **a, int *n) {
    for (int i = 0; i < *n; i++) free((*a)[i].text);
    *n = 0;
}

static int undo_hist_push(struct undo_snap **a, int *n, int *cap) {
    char *flat = editor_flatten();
    if (!flat) return 0;
    if (*n >= *cap) {
        if (*cap > INT_MAX / 2) { free(flat); return 0; }
        int ncap = *cap > 0 ? *cap * 2 : 16;
        struct undo_snap *p = realloc(*a, (size_t)ncap * sizeof(*p));
        if (!p) { free(flat); return 0; }
        *a = p;
        *cap = ncap;
    }
    (*a)[*n].text = flat;
    (*a)[*n].line = cur_line;
    (*a)[*n].col = cur_col;
    (*a)[*n].top = view_top;
    (*a)[*n].vcol = view_col;
    (*n)++;
    return 1;
}

/* Snapshot the buffer before a change. A typing run inside two seconds,
   with no other key in between, keeps the first snapshot. */
static void undo_begin(int group) {
    undo_pushed = 0;
    if (undo_suspend) return;
    if (group && group == undo_group && undo_tick == undo_group_tick + 1 &&
        now_seconds() - undo_group_time < 2.0) {
        undo_group_tick = undo_tick;
        undo_group_time = now_seconds();
        return;
    }
    if (!undo_hist_push(&undo_s, &undo_n, &undo_cap)) {
        undo_group = 0;
        if (status_msg[0] == 0)
            snprintf(status_msg, sizeof(status_msg), "undo not recorded");
        return;
    }
    undo_pushed = 1;
    undo_group = group;
    undo_group_tick = undo_tick;
    undo_group_time = now_seconds();
}

static void undo_commit(void) {
    if (undo_pushed) undo_hist_clear(&redo_s, &redo_n);
    undo_pushed = 0;
}

static void undo_abort(void) {
    if (undo_pushed && undo_n > 0) free(undo_s[--undo_n].text);
    undo_pushed = 0;
    undo_group = 0;
}

static void mark_editor_clean(void) {
    char *flat = editor_flatten();
    if (!flat) return;
    free(editor_clean);
    editor_clean = flat;
}

static void note_synced(int clean) {
    if (!owned_set(&last_synced_code, cstr(current_code)) && status_msg[0] == 0)
        snprintf(status_msg, sizeof(status_msg), "out of memory");
    if (clean) mark_editor_clean();
}

static void selection_bounds(int *a, int *b) {
    int l0 = sel_line, l1 = cur_line, c0 = sel_col, c1 = cur_col;
    if (l0 > l1 || (l0 == l1 && c0 > c1)) {
        int t = l0; l0 = l1; l1 = t;
        t = c0; c0 = c1; c1 = t;
    }
    *a = editor_offset(l0, sel_linewise ? 0 : c0);
    *b = editor_offset(l1, sel_linewise ? (int)strlen(code_lines[l1]) : c1);
    if (sel_linewise && l1 + 1 < code_line_count) (*b)++;
    else if (!sel_linewise && sel_inclusive && code_lines[l1][c1]) {
        (*b)++;
        while ((code_lines[l1][++c1] & 0xc0) == 0x80) (*b)++;
    }
}

static int selected_byte(int line, int col) {
    if (!sel_active) return 0;
    if (sel_linewise) {
        int lo = sel_line < cur_line ? sel_line : cur_line;
        int hi = sel_line > cur_line ? sel_line : cur_line;
        return line >= lo && line <= hi;
    }
    int a, b;
    selection_bounds(&a, &b);
    int at = editor_offset(line, col);
    if (at < 0) return 0;
    return at >= a && at < b;
}

static void selection_start(int linewise) {
    sel_active = 1; sel_linewise = linewise;
    sel_inclusive = 0;
    sel_line = cur_line; sel_col = cur_col;
}

static void editor_toggle_mode(void) {
    vi_mode = !vi_mode;
    vi_normal = vi_mode;
    vi_pending = 0;
    vi_reset();
    if (vi_normal && cur_line>=0 && cur_line<code_line_count) {
        int length=(int)strlen(code_lines[cur_line]);
        if (length && cur_col>=length) cur_col=vi_prev(code_lines[cur_line],length);
    }
    /* A regular editor selection must not become vi visual mode implicitly.
       Selection paint takes precedence over the moving note highlights. */
    sel_active = 0;
    sel_inclusive = 0;
}

static void editor_copy_range(char *dst, size_t from, size_t n) {
    size_t pos = 0, got = 0;
    for (int i = 0; i < code_line_count && got < n; i++) {
        if (i) {
            if (pos >= from && got < n) dst[got++] = '\n';
            pos++;
        }
        const char *s = code_lines[i] ? code_lines[i] : "";
        size_t L = strlen(s);
        if (pos + L > from && got < n) {
            size_t skip = from > pos ? from - pos : 0;
            size_t take = L - skip;
            if (take > n - got) take = n - got;
            memcpy(dst + got, s + skip, take);
            got += take;
        }
        pos += L;
    }
}

/* Build the whole edit before touching the buffer. Failure leaves it unchanged. */
static int editor_replace(int a, int b, const char *text) {
    if (!text) text = "";
    if (!lines_ready()) {
        snprintf(status_msg, sizeof(status_msg), "out of memory");
        return 0;
    }
    size_t len = 0;
    for (int i = 0; i < code_line_count; i++) {
        if (i) {
            if (len > SOURCE_MAX - 1) goto big;
            len++;
        }
        size_t L = strlen(code_lines[i]);
        if (L > SOURCE_MAX - len) goto big;
        len += L;
    }
    size_t add = strlen(text);
    if (a < 0 || b < a || (size_t)b > len) {
        snprintf(status_msg, sizeof(status_msg), "edit failed");
        return 0;
    }
    size_t removed = (size_t)(b - a);
    if (add > SOURCE_MAX || len - removed > SOURCE_MAX - add) goto big;
    undo_begin(0);
    size_t nlen = len - removed + add;
    char *next = malloc(nlen + 1);
    if (!next) goto oom;
    if (a) editor_copy_range(next, 0, (size_t)a);
    if (add) memcpy(next + a, text, add);
    if (len > (size_t)b) editor_copy_range(next + a + add, (size_t)b, len - (size_t)b);
    next[nlen] = 0;
    int nlines = 1;
    for (size_t i = 0; i < nlen; i++) if (next[i] == '\n') nlines++;
    char **rows = calloc((size_t)nlines, sizeof(char *));
    if (!rows) { free(next); goto oom; }
    size_t start = 0;
    int row = 0, ok = 1;
    for (size_t i = 0; i <= nlen; i++) {
        if (i != nlen && next[i] != '\n') continue;
        rows[row] = xdup_n(next + start, i - start);
        if (!rows[row]) { ok = 0; break; }
        row++;
        start = i + 1;
    }
    free(next);
    if (!ok || row != nlines) {
        for (int i = 0; i < row; i++) free(rows[i]);
        free(rows);
        goto oom;
    }
    size_t endpos = (size_t)a + add;
    if (endpos > nlen) endpos = nlen;
    int cl = 0, cc = 0;
    size_t pos = 0;
    for (int i = 0; i < nlines; i++) {
        size_t L = strlen(rows[i]);
        if (endpos <= pos + L || i + 1 == nlines) {
            cl = i;
            cc = (int)(endpos > pos ? endpos - pos : 0);
            if ((size_t)cc > L) cc = (int)L;
            break;
        }
        pos += L + 1;
    }
    for (int i = 0; i < code_line_cap; i++) free(code_lines[i]);
    free(code_lines);
    code_lines = rows;
    code_line_cap = nlines;
    code_line_count = nlines;
    cur_line = cl;
    cur_col = cc;
    sel_active = 0;
    undo_commit();
    return 1;
big:
    snprintf(status_msg, sizeof(status_msg), "edit exceeds the 2 GiB offset limit");
    return 0;
oom:
    undo_abort();
    snprintf(status_msg, sizeof(status_msg), "out of memory");
    return 0;
}

static void selection_copy(int cut) {
    if (!sel_active || !lines_ready()) return;
    int a, b, pos = 0;
    selection_bounds(&a, &b);
    if (a < 0 || b < a) { sel_active = 0; return; }
    size_t need = 0;
    pos = 0;
    for (int i = 0; i < code_line_count; i++) {
        for (const char *p = code_lines[i]; *p; p++, pos++)
            if (pos >= a && pos < b) need++;
        if (i + 1 < code_line_count) {
            if (pos >= a && pos < b) need++;
            pos++;
        }
    }
    int lines = sel_linewise;
    if (lines) need++; /* trailing newline, whether or not the slice has one */
    if (need > SOURCE_MAX) {
        snprintf(status_msg, sizeof(status_msg), "out of memory");
        sel_active = 0;
        return;
    }
    char *buf = malloc(need + 1);
    if (!buf) {
        snprintf(status_msg, sizeof(status_msg), "out of memory");
        sel_active = 0;
        return;
    }
    size_t out = 0;
    pos = 0;
    for (int i = 0; i < code_line_count; i++) {
        for (const char *p = code_lines[i]; *p; p++, pos++)
            if (pos >= a && pos < b) buf[out++] = *p;
        if (i + 1 < code_line_count) {
            if (pos >= a && pos < b) buf[out++] = '\n';
            pos++;
        }
    }
    if (lines && (out == 0 || buf[out - 1] != '\n')) buf[out++] = '\n';
    buf[out] = 0;
    if (vi_yank != yank_blank) free(vi_yank);
    vi_yank = buf;
    yank_lines = lines;
    if (cut) {
        /* Removing the final line also removes its preceding separator. */
        if (lines && b == editor_offset(code_line_count - 1,
            (int)strlen(code_lines[code_line_count - 1])) && a > 0) a--;
        (void)editor_replace(a, b, "");
    }
    sel_active = 0;
}

static void editor_paste(int before) {
    if (!vi_yank[0]) return;
    int a = editor_offset(cur_line, cur_col), b = a;
    if (sel_active) selection_bounds(&a, &b);
    else if (vi_mode && vi_normal && yank_lines) {
        if (before) a = b = editor_offset(cur_line, 0);
        else if (cur_line + 1 < code_line_count) a = b = editor_offset(cur_line + 1, 0);
        else {
            char *text = malloc(strlen(vi_yank) + 2);
            if (!text) return;
            text[0] = '\n';
            size_t n = strlen(vi_yank);
            memcpy(text + 1, vi_yank, n + 1);
            if (n && text[n] == '\n') text[n] = 0;
            a = b = editor_offset(cur_line, (int)strlen(code_lines[cur_line]));
            (void)editor_replace(a, b, text);
            free(text); return;
        }
    }
    int characterwise=vi_mode && vi_normal && !yank_lines && !sel_active;
    if (characterwise && !before)
        a=b=editor_offset(cur_line,vi_next(code_lines[cur_line],cur_col,(int)strlen(code_lines[cur_line])));
    if (editor_replace(a, b, vi_yank) && characterwise && cur_col>0)
        cur_col=vi_prev(code_lines[cur_line],cur_col);
}

static int grow_add(char **buf, size_t *len, size_t *cap, char c) {
    if (*len + 2 > *cap) {
        if (*cap > SOURCE_MAX / 2) return 0;
        size_t ncap = *cap ? *cap * 2 : 64;
        if (ncap < *len + 2) ncap = *len + 2;
        if (ncap > SOURCE_MAX) return 0;
        char *n = realloc(*buf, ncap);
        if (!n) return 0;
        *buf = n;
        *cap = ncap;
    }
    (*buf)[(*len)++] = c;
    return 1;
}

static int rows_add(char ***rows, int *n, int *cap, char *s) {
    if (*n >= *cap) {
        if (*cap > INT_MAX / 2) return 0;
        int ncap = *cap ? *cap * 2 : 8;
        char **nr = realloc(*rows, (size_t)ncap * sizeof(char *));
        if (!nr) return 0;
        for (int i = *cap; i < ncap; i++) nr[i] = NULL;
        *rows = nr;
        *cap = ncap;
    }
    (*rows)[(*n)++] = s;
    return 1;
}

static int replace_code_with(const char *txt) {
    vi_reset();
    const char *p = txt ? txt : "";
    undo_begin(0);
    int rcap = 8, n = 0, oom = 0, ended_nl = 0;
    char **rows = calloc((size_t)rcap, sizeof(char *));
    size_t lcap = 64, li = 0;
    char *line = malloc(lcap);
    if (!rows || !line) { free(rows); free(line); goto oom; }
    while (*p) {
        unsigned char c = (unsigned char)*p++;
        if (c == '\n') {
            line[li] = 0;
            if (!rows_add(&rows, &n, &rcap, line)) { oom = 1; break; }
            line = malloc(64);
            lcap = 64;
            li = 0;
            ended_nl = 1;
            if (!line) { oom = 1; break; }
        } else if (c == '\t') {
            ended_nl = 0;
            if (!grow_add(&line, &li, &lcap, ' ') || !grow_add(&line, &li, &lcap, ' ')) {
                oom = 1; break;
            }
        } else if (c < 0x20) {
            /* drop CR and other controls; a trailing newline still opens a line */
        } else {
            ended_nl = 0;
            if (!grow_add(&line, &li, &lcap, (char)c)) { oom = 1; break; }
        }
    }
    if (!oom && (li > 0 || n == 0 || ended_nl)) {
        if (!line) oom = 1;
        else {
            line[li] = 0;
            if (!rows_add(&rows, &n, &rcap, line)) oom = 1;
            else line = NULL;
        }
    }
    if (oom || n < 1) {
        if (line) free(line);
        for (int i = 0; i < n; i++) free(rows[i]);
        free(rows);
        goto oom;
    }
    free(line);
    for (int i = 0; i < code_line_cap; i++) free(code_lines[i]);
    free(code_lines);
    code_lines = rows;
    code_line_cap = rcap;
    code_line_count = n;
    cur_line = 0;
    cur_col = 0;
    view_top = 0;
    view_col = 0;
    sel_active = 0;
    undo_commit();
    return 1;
oom:
    undo_abort();
    snprintf(status_msg, sizeof(status_msg), "out of memory");
    return 0;
}

static int undo_apply(const struct undo_snap *s) {
    char *text=strdup_safe(s->text);
    if (!text) return 0;
    size_t length=strlen(text);
    /* editor_flatten adds a terminator newline beyond the editor's lines. */
    if (length && text[length-1]=='\n') text[length-1]=0;
    undo_suspend++;
    int end=editor_offset(code_line_count-1,(int)strlen(code_lines[code_line_count-1]));
    int ok = editor_replace(0,end,text);
    undo_suspend--;
    free(text);
    if (!ok) return 0;
    vi_reset();
    cur_line = s->line;
    if (cur_line < 0) cur_line = 0;
    if (cur_line >= code_line_count) cur_line = code_line_count - 1;
    cur_col = s->col;
    if (cur_col < 0) cur_col = 0;
    if (code_line_count > 0) {
        int L = (int)strlen(code_lines[cur_line]);
        if (cur_col > L) cur_col = L;
    }
    view_top = s->top < 0 ? 0 : s->top;
    view_col = s->vcol < 0 ? 0 : s->vcol;
    sel_active = 0;
    undo_group = 0;
    return 1;
}

static int editor_undo(void) {
    if (undo_n <= 0) {
        snprintf(status_msg, sizeof(status_msg), "nothing to undo");
        return 0;
    }
    if (!undo_hist_push(&redo_s, &redo_n, &redo_cap)) {
        snprintf(status_msg, sizeof(status_msg), "out of memory");
        return 0;
    }
    struct undo_snap s = undo_s[--undo_n];
    if (!undo_apply(&s)) {
        undo_s[undo_n++] = s;
        free(redo_s[--redo_n].text);
        return 0;
    }
    free(s.text);
    snprintf(status_msg, sizeof(status_msg), "undo");
    return 1;
}

static int editor_redo(void) {
    if (redo_n <= 0) {
        snprintf(status_msg, sizeof(status_msg), "nothing to redo");
        return 0;
    }
    if (!undo_hist_push(&undo_s, &undo_n, &undo_cap)) {
        snprintf(status_msg, sizeof(status_msg), "out of memory");
        return 0;
    }
    struct undo_snap s = redo_s[--redo_n];
    if (!undo_apply(&s)) {
        redo_s[redo_n++] = s;
        free(undo_s[--undo_n].text);
        return 0;
    }
    free(s.text);
    snprintf(status_msg, sizeof(status_msg), "redo");
    return 1;
}

/* Unknown or malformed settings leave the defaults intact. */
#include "tui_theme.h"

static void load_editor_config(void) {
    char base[1024], path[1200], text[4097];
    user_dir(base, sizeof(base), "XDG_CONFIG_HOME", "tompouce", ".config/tompouce");
    if (!base[0]) return;
    int n = snprintf(path, sizeof(path), "%s/config.json", base);
    if (n < 0 || (size_t)n >= sizeof(path)) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    size_t len = fread(text, 1, sizeof(text) - 1, f);
    int bad = ferror(f) || !feof(f);
    fclose(f);
    if (bad) return;
    text[len] = 0;
    cJSON *cfg = cJSON_Parse(text);
    if (!cJSON_IsObject(cfg)) { cJSON_Delete(cfg); return; }
    cJSON *v = cJSON_GetObjectItemCaseSensitive(cfg, "vi_mode");
    if (cJSON_IsBool(v)) vi_mode = vi_normal = cJSON_IsTrue(v);
    v = cJSON_GetObjectItemCaseSensitive(cfg, "mouse");
    if (cJSON_IsBool(v)) mouse_enabled = cJSON_IsTrue(v);
    v = cJSON_GetObjectItemCaseSensitive(cfg, "split_ratio");
    if (cJSON_IsNumber(v) && isfinite(v->valuedouble) &&
        v->valuedouble >= .3 && v->valuedouble <= .85) split_ratio = v->valuedouble;
    v = cJSON_GetObjectItemCaseSensitive(cfg, "wrap");
    if (cJSON_IsBool(v)) editor_wrap = cJSON_IsTrue(v);
    v = cJSON_GetObjectItemCaseSensitive(cfg, "editor_scope");
    if (cJSON_IsBool(v)) editor_scope = cJSON_IsTrue(v);
    v = cJSON_GetObjectItemCaseSensitive(cfg, "inline_visuals");
    if (cJSON_IsBool(v)) inline_visuals = cJSON_IsTrue(v);
    v = cJSON_GetObjectItemCaseSensitive(cfg, "kitty_graphics");
    if (cJSON_IsBool(v)) graphics_disabled = !cJSON_IsTrue(v);
    v = cJSON_GetObjectItemCaseSensitive(cfg, "theme_source");
    if (cJSON_IsString(v)) for (int i=0;i<4;i++)
        if (!strcmp(v->valuestring,theme_modes[i])) theme_mode=i;
    cJSON_Delete(cfg);
}

/* Rewrite config.json, preserving keys this loader does not understand. */
static void save_editor_config(void) {
    char base[1024], path[1200], text[4097];
    user_dir(base, sizeof(base), "XDG_CONFIG_HOME", "tompouce", ".config/tompouce");
    if (!base[0]) return;
    mkdir_p(base);
    int n = snprintf(path, sizeof(path), "%s/config.json", base);
    if (n < 0 || (size_t)n >= sizeof(path)) return;
    cJSON *cfg = NULL;
    FILE *f = fopen(path, "r");
    if (f) {
        size_t len = fread(text, 1, sizeof(text) - 1, f);
        int bad = ferror(f) || !feof(f);
        fclose(f);
        if (!bad) {
            text[len] = 0;
            cfg = cJSON_Parse(text);
        }
    }
    if (!cJSON_IsObject(cfg)) { cJSON_Delete(cfg); cfg = cJSON_CreateObject(); }
    if (!cfg) return;
    cJSON_DeleteItemFromObject(cfg, "vi_mode");
    if (!cJSON_AddBoolToObject(cfg, "vi_mode", vi_mode ? 1 : 0)) { cJSON_Delete(cfg); return; }
    cJSON_DeleteItemFromObject(cfg, "wrap");
    cJSON_AddBoolToObject(cfg, "wrap", editor_wrap ? 1 : 0);
    cJSON_DeleteItemFromObject(cfg, "editor_scope");
    cJSON_AddBoolToObject(cfg, "editor_scope", editor_scope ? 1 : 0);
    cJSON_DeleteItemFromObject(cfg, "inline_visuals");
    cJSON_AddBoolToObject(cfg, "inline_visuals", inline_visuals ? 1 : 0);
    cJSON_DeleteItemFromObject(cfg, "kitty_graphics");
    cJSON_AddBoolToObject(cfg, "kitty_graphics", !graphics_disabled);
    cJSON_DeleteItemFromObject(cfg, "theme_source");
    cJSON_AddStringToObject(cfg, "theme_source", theme_modes[theme_mode]);
    char *txt = cJSON_Print(cfg);
    cJSON_Delete(cfg);
    if (!txt) return;
    f = fopen(path, "w");
    if (!f) { free(txt); return; }
    size_t len = strlen(txt);
    int ok = fwrite(txt, 1, len, f) == len;
    if (fclose(f) != 0) ok = 0;
    free(txt);
    if (!ok) unlink(path);
}

void tui_init(void) {
    saved_tty_valid = tcgetattr(fileno(stdin), &saved_tty) == 0;
    if (atexit(tui_shutdown) != 0) { fprintf(stderr, "cannot register terminal cleanup\n"); exit(1); }
    load_editor_config();
    signal(SIGWINCH, handle_winch);
    signal(SIGINT, handle_quit);
    signal(SIGTERM, handle_quit);
    signal(SIGHUP, handle_quit);
    signal(SIGQUIT, handle_quit);
    terminal_init_locale(); /* unicode when the locale supports it */
    log_console_stderr(0); /* stderr shares the terminal: keep it quiet */
    if (terminal_startup() == -1) {
        fprintf(stderr, "Notcurses terminal initialization failed\n");
        exit(1);
    }
    smg_active = 1;
    if (notcurses_linesigs_disable(terminal_nc) < 0) {
        fprintf(stderr, "cannot configure Notcurses input\n");
        exit(1);
    }
    tty_active = 1;
    /* Editor shortcuts own Ctrl+C/S/Q; preserve the original modes above. */
    struct termios edit_tty;
    if (tcgetattr(fileno(stdin), &edit_tty) == 0) {
        edit_tty.c_iflag &= ~(IXON | IXOFF);
        edit_tty.c_lflag &= ~ISIG;
        if (tcsetattr(fileno(stdin), TCSANOW, &edit_tty) != 0) {
            fprintf(stderr, "cannot configure editor terminal\n"); exit(1);
        }
    }
    terminal_bind_key("\033OP", ((int)NCKEY_F00+(1)));
    terminal_bind_key("\033OQ", ((int)NCKEY_F00+(2)));
    terminal_bind_key("\033OR", ((int)NCKEY_F00+(3)));
    terminal_bind_key("\033OS", ((int)NCKEY_F00+(4)));
    terminal_bind_key("\033[11~", ((int)NCKEY_F00+(1)));
    terminal_bind_key("\033[12~", ((int)NCKEY_F00+(2)));
    terminal_bind_key("\033[13~", ((int)NCKEY_F00+(3)));
    terminal_bind_key("\033[14~", ((int)NCKEY_F00+(4)));
    terminal_bind_key("\033[15~", ((int)NCKEY_F00+(5)));
    terminal_bind_key("\033[17~", ((int)NCKEY_F00+(6)));
    terminal_bind_key("\033[18~", ((int)NCKEY_F00+(7)));
    terminal_bind_key("\033[19~", ((int)NCKEY_F00+(8)));
    terminal_bind_key("\033[20~", ((int)NCKEY_F00+(9)));
    terminal_bind_key("\033[21~", ((int)NCKEY_F00+(10)));
    terminal_bind_key("\033[23~", ((int)NCKEY_F00+(11)));
    terminal_bind_key("\033[24~", ((int)NCKEY_F00+(12)));
    terminal_bind_key("\033[1;5D", 2001); /* Ctrl+Left  nudge slider */
    terminal_bind_key("\033[1;5C", 2002); /* Ctrl+Right */
    terminal_bind_key("\033[5D", 2001);
    terminal_bind_key("\033[5C", 2002);
    terminal_bind_key("\033[<", 2100); /* SGR mouse report prefix */
    terminal_bind_key("\033[200~", 2101); /* bracketed paste */
    terminal_bind_key("\033[Z", 2102); /* Shift+Tab */
    terminal_bind_key("\033[1;2A", 2110);
    terminal_bind_key("\033[1;2B", 2111);
    terminal_bind_key("\033[1;2D", 2112);
    terminal_bind_key("\033[1;2C", 2113);
    terminal_bind_key("\033[1;2H", 2114);
    terminal_bind_key("\033[1;2F", 2115);
    fputs("\033[?2004h", terminal_output);
    /* Notcurses must select the wire protocol to match its coordinate decoder
       (cell SGR or pixel SGR on terminals advertising DEC mode 1016). */
    if (mouse_enabled && notcurses_mice_enable(terminal_nc,NCMICE_BUTTON_EVENT|NCMICE_DRAG_EVENT)<0)
        log_console("cannot enable terminal mouse tracking");
    fflush(terminal_output);
    viz_utf8 = terminal_utf8();
    graphics_read_palette();
    graphics_probe();
    /* editor colors (degrade to inverse video on mono terminals) */
    terminal_define_role(2, NULL, "yellow", "black");        /* keyword */
    terminal_define_role(3, NULL, "brightcyan", "black");   /* note */
    terminal_define_role(4, NULL, "brightmagenta", "black");/* drum */
    terminal_define_role(5, NULL, "brightblue", "black");   /* transform */
    terminal_define_role(6, NULL, "gray", "black");          /* comment */
    terminal_define_role(7, NULL, "brown", "black");        /* rest */
    terminal_define_role(8, NULL, "brightgreen", "black");  /* number */
    terminal_define_role(9, NULL, "red", "black");          /* error marker */
    terminal_define_role(10, NULL, "black", "yellow");      /* sounding now */
    terminal_define_role(11, NULL, "black", "brightcyan"); /* selection */
    theme_init();
    rows = terminal_rows;
    cols = terminal_cols;
    if (!lines_ready()) {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }
}

void tui_shutdown(void) {
    companion_shutdown();
    if (!smg_active && !tty_active) return;
    inline_graphics_clear();
    if (smg_active && notcurses_mice_disable(terminal_nc)<0)
        log_console("cannot disable terminal mouse tracking");
    fflush(terminal_output);
    if (smg_active) { terminal_shutdown(); smg_active = 0; }
    tty_active = 0;
    fputs("\033[?2004l\033[0m\033[?25h", stdout);
    fflush(stdout);
    if (saved_tty_valid) (void)tcsetattr(fileno(stdin), TCSANOW, &saved_tty);
    log_console_stderr(1);
}

/* blank w cells (bounded) for flicker-free redraw without cls */
static void blank_inner(int r, int c, int w) {
    static char sp[513];
    static int init = 0;
    if (!init) { memset(sp, ' ', sizeof(sp)-1); sp[512] = 0; init = 1; }
    if (w <= 0) return;
    terminal_move(r, c);
    while (w > 0) {
        int n = w > 512 ? 512 : w;
        terminal_write_padded(sp, (unsigned)n);
        w -= n;
    }
}

/* decode one UTF-8 sequence; invalid -> U+FFFD, len 1 */
static unsigned utf8_decode(const char *s, int *len) {
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *len = 1; return c; }
    if ((c & 0xE0) == 0xC0) {
        if ((s[1] & 0xC0) != 0x80) { *len = 1; return 0xFFFD; }
        unsigned cp = ((unsigned)(c & 0x1F) << 6) | (unsigned)(s[1] & 0x3F);
        if (cp < 0x80) { *len = 1; return 0xFFFD; }
        *len = 2;
        return cp;
    }
    if ((c & 0xF0) == 0xE0) {
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) { *len = 1; return 0xFFFD; }
        unsigned cp = ((unsigned)(c & 0x0F) << 12) | ((unsigned)(s[1] & 0x3F) << 6) | (unsigned)(s[2] & 0x3F);
        if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) { *len = 1; return 0xFFFD; }
        *len = 3;
        return cp;
    }
    if ((c & 0xF8) == 0xF0) {
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) {
            *len = 1;
            return 0xFFFD;
        }
        unsigned cp = ((unsigned)(c & 0x07) << 18) | ((unsigned)(s[1] & 0x3F) << 12) |
                      ((unsigned)(s[2] & 0x3F) << 6) | (unsigned)(s[3] & 0x3F);
        if (cp < 0x10000 || cp > 0x10FFFF) { *len = 1; return 0xFFFD; }
        *len = 4;
        return cp;
    }
    *len = 1;
    return 0xFFFD;
}

/* terminal cells for a codepoint (wide CJK/emoji = 2) */
static int cell_width(unsigned cp) {
    if (cp < 0x20 || (cp >= 0x7f && cp < 0xa0)) return 0;
    if ((cp >= 0x1100 && cp <= 0x115f) || cp == 0x2329 || cp == 0x232a ||
        (cp >= 0x2e80 && cp <= 0x303e) || (cp >= 0x3041 && cp <= 0x33ff) ||
        (cp >= 0x3400 && cp <= 0x4dbf) || (cp >= 0x4e00 && cp <= 0x9fff) ||
        (cp >= 0xa000 && cp <= 0xa4cf) || (cp >= 0xac00 && cp <= 0xd7a3) ||
        (cp >= 0xf900 && cp <= 0xfaff) || (cp >= 0xfe30 && cp <= 0xfe4f) ||
        (cp >= 0xff00 && cp <= 0xff60) || (cp >= 0xffe0 && cp <= 0xffe6) ||
        (cp >= 0x1f300 && cp <= 0x1faff) || (cp >= 0x20000 && cp <= 0x3fffd))
        return 2;
    if (cp >= 0x300 && cp <= 0x36f) return 0;
    return 1;
}

static int editor_cell_advance(unsigned cp) {
    if (cp == '\t') return 2;
    if (cp < 0x20 || cp == 0x7f) return 1;
    int w = cell_width(cp);
    return w > 0 ? w : 0;
}

/* Quote and comment state carried across visual rows of one logical line.
   Each editor_place call starts from a clear state: wrap does not look at the previous line. */
struct wrap_st {
    char quote;
    int comment; /* // or a leading #, through end of line */
    int block;   /* slash-star, through this line only */
    int saw;     /* a non-space token has been seen (so a later # is not a comment) */
};

static int step_cp(const char *s, int i, int *n_out) {
    int n = 0;
    unsigned cp = utf8_decode(s + i, &n);
    if (n < 1) n = 1;
    if (n_out) *n_out = n;
    return editor_cell_advance(cp);
}

static int span_cells(const char *s, int a, int b) {
    int col = 0, i = a;
    if (!s || a < 0) return 0;
    while (s[i] && i < b) {
        int n = 0, w = step_cp(s, i, &n);
        if (i + n > b) break;
        col += w;
        i += n;
    }
    return col;
}

/* Apply the bytes [i, end) to wrap state. A whole string (open and close) leaves quote clear. */
static void wrap_take(const char *s, int i, int end, struct wrap_st *st) {
    if (!st || end <= i || !s) return;
    if (st->quote) {
        if (s[i] == '\\' && end > i + 1) return;
        if (s[i] == st->quote) st->quote = 0;
        return;
    }
    if (st->block) {
        if (s[i] == '*' && i + 1 < end && s[i + 1] == '/') st->block = 0;
        if (s[i] != ' ' && s[i] != '\t') st->saw = 1;
        return;
    }
    if (st->comment) {
        if (s[i] != ' ' && s[i] != '\t') st->saw = 1;
        return;
    }
    if (s[i] == ' ' || s[i] == '\t') return;
    if (s[i] == '#' && !st->saw) { st->comment = 1; st->saw = 1; return; }
    if (s[i] == '/' && i + 1 < end && s[i + 1] == '/') { st->comment = 1; st->saw = 1; return; }
    if (s[i] == '/' && i + 1 < end && s[i + 1] == '*') {
        st->block = 1;
        st->saw = 1;
        for (int k = i + 2; k + 1 < end; k++) {
            if (s[k] == '*' && s[k + 1] == '/') { st->block = 0; break; }
        }
        return;
    }
    if ((s[i] == '"' || s[i] == '\'' || s[i] == '`') && end == i + 1) {
        st->quote = s[i];
        st->saw = 1;
        return;
    }
    st->saw = 1;
}

/* Tree-sitter JavaScript tokens for one line. Strings and comments are one span;
   a method dot is glued to the property name. The hand scanner below still
   breaks a span that is wider than the pane, including mini-notation in quotes. */
enum { TS_OTHER = 0, TS_STR = 1, TS_CMT = 2, TS_DOT = 3, TS_IDENT = 4 };
struct ts_span { int start, end; unsigned char kind; };
static struct ts_span *ts_spans;
static int ts_n, ts_cap_n;
static const char *ts_ptr;
static int ts_len;
static unsigned ts_hash;
static int ts_ok;
static TSParser *ts_parser;

static unsigned line_fnv(const char *s, int n) {
    unsigned h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h;
}

static int ts_kind(const char *t) {
    if (!strcmp(t, "string") || !strcmp(t, "template_string")) return TS_STR;
    if (!strcmp(t, "comment")) return TS_CMT;
    if (!strcmp(t, ".") || !strcmp(t, "?.") || !strcmp(t, "optional_chain")) return TS_DOT;
    if (!strcmp(t, "identifier") || !strcmp(t, "property_identifier") ||
        !strcmp(t, "shorthand_property_identifier")) return TS_IDENT;
    return TS_OTHER;
}

static int ts_push(int start, int end, unsigned char kind) {
    if (end <= start) return 1;
    if (ts_n >= ts_cap_n) {
        int ncap = ts_cap_n ? ts_cap_n * 2 : 32;
        struct ts_span *p;
        if (ts_cap_n > 1000000 || ncap < ts_cap_n) return 0;
        p = realloc(ts_spans, (size_t)ncap * sizeof *p);
        if (!p) return 0;
        ts_spans = p;
        ts_cap_n = ncap;
    }
    ts_spans[ts_n].start = start;
    ts_spans[ts_n].end = end;
    ts_spans[ts_n].kind = kind;
    ts_n++;
    return 1;
}

static int ts_walk(TSNode node, int depth) {
    const char *t;
    int k;
    uint32_t nc;
    if (ts_node_is_null(node)) return 1;
    t = ts_node_type(node);
    k = ts_kind(t);
    nc = ts_node_child_count(node);
    if (k == TS_STR || k == TS_CMT || depth > 48 || nc == 0) {
        uint32_t a = ts_node_start_byte(node);
        uint32_t b = ts_node_end_byte(node);
        if (a > (uint32_t)INT_MAX || b > (uint32_t)INT_MAX) return 1;
        return ts_push((int)a, (int)b, (unsigned char)k);
    }
    for (uint32_t i = 0; i < nc; i++) {
        if (!ts_walk(ts_node_child(node, i), depth + 1)) return 0;
    }
    return 1;
}

static void ts_merge_dots(void) {
    int r = 0, w = 0;
    while (r < ts_n) {
        struct ts_span s = ts_spans[r++];
        if (s.kind == TS_DOT && r < ts_n && ts_spans[r].kind == TS_IDENT &&
            ts_spans[r].start == s.end) {
            s.end = ts_spans[r].end;
            s.kind = TS_OTHER;
            r++;
        }
        ts_spans[w++] = s;
    }
    ts_n = w;
}

static int ts_ensure(const char *s) {
    int n = 0;
    unsigned h;
    TSTree *tree;
    while (s[n]) {
        if (n == INT_MAX) return 0;
        n++;
    }
    h = line_fnv(s, n);
    if (ts_ok == 1 && s == ts_ptr && n == ts_len && h == ts_hash) return 1;
    if (ts_ok < 0) return 0;
    if (!ts_parser) {
        const TSLanguage *lang = tree_sitter_javascript();
        ts_parser = ts_parser_new();
        if (!ts_parser || !lang || !ts_parser_set_language(ts_parser, lang)) {
            if (ts_parser) ts_parser_delete(ts_parser);
            ts_parser = NULL;
            ts_ok = -1;
            return 0;
        }
    }
    tree = ts_parser_parse_string(ts_parser, NULL, s, (uint32_t)n);
    if (!tree) return 0;
    ts_n = 0;
    if (!ts_walk(ts_tree_root_node(tree), 0)) {
        ts_tree_delete(tree);
        return 0;
    }
    ts_tree_delete(tree);
    ts_merge_dots();
    ts_ptr = s;
    ts_len = n;
    ts_hash = h;
    ts_ok = 1;
    return 1;
}

/* Whole-source syntax keeps calls inside comments and strings out of the layout.
   This runs only when the source changes, on the UI thread. */
static void inline_walk(TSNode node, const char *source, int depth) {
    if (depth > 128 || ts_node_is_null(node)) return;
    const char *type = ts_node_type(node);
    if (!strcmp(type,"comment") || !strcmp(type,"string") || !strcmp(type,"template_string")) return;
    if (!strcmp(type,"call_expression")) {
        TSNode function = ts_node_child_by_field_name(node,"function",8);
        if (!ts_node_is_null(function)) {
            TSNode property = !strcmp(ts_node_type(function),"member_expression")
                ? ts_node_child_by_field_name(function,"property",8) : function;
            uint32_t fa=ts_node_start_byte(function),fb=ts_node_end_byte(function);
            if ((fb-fa==3 && !memcmp(source+fa,"all",3)) || (fb-fa==4 && !memcmp(source+fa,"each",4))) {
                TSNode args=ts_node_child_by_field_name(node,"arguments",9);
                property=ts_node_named_child(args,0);
                if (strcmp(ts_node_type(property),"identifier")) property=function;
            }
            if (!ts_node_is_null(property)) {
                uint32_t a = ts_node_start_byte(property), b = ts_node_end_byte(property);
                int visual = visual_kind(source+a,b-a);
                int mask = visual ? 1<<(visual-1) : 0;
                uint32_t row = ts_node_end_point(node).row;
                if (mask && row < (uint32_t)code_line_count) {
                    if (!inline_lines[row]) inline_count++;
                    inline_lines[row] |= (unsigned char)mask;
                    int kind = visual-1;
                    if (inline_feeds[row].offset[kind] < 0) inline_feeds[row].offset[kind] = (int)a;
                }
            }
        }
    }
    uint32_t children = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < children; i++) inline_walk(ts_node_named_child(node,i),source,depth+1);
}

static void inline_refresh(void) {
    inline_count = 0;
    inline_bound_generation = 0;
    inline_bound_revision = UINT_MAX;
    InlineFeed *feeds = calloc((size_t)code_line_count,sizeof(*feeds));
    if (!feeds) { inline_capacity = 0; return; }
    if (code_line_count > inline_capacity) {
        unsigned char *next = realloc(inline_lines,(size_t)code_line_count);
        if (!next) { free(feeds); inline_capacity = 0; return; }
        inline_lines = next; inline_capacity = code_line_count;
    }
    if (inline_lines) memset(inline_lines,0,(size_t)inline_capacity);
    free(inline_feeds); inline_feeds = feeds;
    for (int i = 0; i < code_line_count; i++)
        for (int j=0;j<VISUAL_KINDS;j++) feeds[i].offset[j]=-1;
    char *source = editor_flatten();
    free(inline_source); inline_source = source;
    if (!source) return;
    if (!ts_parser) (void)ts_ensure("");
    if (ts_parser) {
        size_t size = strlen(source);
        TSTree *tree = size <= UINT32_MAX ? ts_parser_parse_string(ts_parser,NULL,source,(uint32_t)size) : NULL;
        if (tree) { inline_walk(ts_tree_root_node(tree),source,0); ts_tree_delete(tree); }
    }
}

static void inline_bind_feeds(void) {
    if (!inline_feeds || !inline_source) return;
    pthread_mutex_lock(&tracks_lock);
    Track *track = track_count > 0 ? &tracks[0] : NULL;
    unsigned generation = track && track->javascript ? track->compile_generation : 0;
    JsProgram *program=generation ? track->javascript : NULL;
    if (program) javascript_retain(program);
    pthread_mutex_unlock(&tracks_lock);
    javascript_request_visuals(program,inline_source);
    unsigned revision=javascript_visual_revision(program);
    if (!generation || generation != inline_bound_generation || revision != inline_bound_revision) {
        for (int i = 0; i < code_line_count && i < inline_capacity; i++) {
            inline_feeds[i].generation = 0;
            for (int kind = 0; kind < VISUAL_KINDS; kind++) {
                inline_feeds[i].mask[kind] = javascript_live_scope_mask(program,inline_source,inline_feeds[i].offset[kind],kind+1);
                if (inline_feeds[i].mask[kind]) inline_feeds[i].generation=generation;
            }
        }
        inline_bound_generation = generation;
        inline_bound_revision = revision;
    }
    javascript_free(program);
}

static int inline_mask(int line) {
    return inline_visuals && line >= 0 && line < code_line_count && line < inline_capacity ? inline_lines[line] : 0;
}

static int inline_piano_rows(void) {
    return graphics_supported && !graphics_disabled ? 8 : 4;
}

static int inline_rows(int line) {
    int mask = inline_mask(line);
    int rows=0;
    for (int kind=1;kind<=VISUAL_KINDS;kind++) if (mask&(1<<(kind-1)))
        rows+=kind==VISUAL_SCOPE?4:inline_piano_rows()+1;
    return rows;
}

static int editor_footer(int height) {
    return editor_scope && height >= 14 && !(inline_visuals && inline_count) ? 2 : 0;
}

/* Span covering byte i, if the parser produced one. */
static int ts_cover(const char *s, int i, int *start, int *endb, int *kind) {
    int lo = 0, hi;
    if (!s || !ts_ensure(s)) return 0;
    hi = ts_n;
    while (lo < hi) {
        int mid = lo + (hi - lo) / 2;
        if (ts_spans[mid].end <= i) lo = mid + 1;
        else hi = mid;
    }
    if (lo >= ts_n || i < ts_spans[lo].start) return 0;
    *start = ts_spans[lo].start;
    *endb = ts_spans[lo].end;
    *kind = ts_spans[lo].kind;
    return 1;
}

static int measure_capped(const char *s, int i, int endb, int cap, int *out) {
    int total = 0;
    if (endb < i) endb = i;
    while (s[i] && i < endb && total < cap) {
        int n = 0, w = step_cp(s, i, &n);
        if (i + n > endb) break;
        if (w == 0) { i += n; continue; }
        if (total > 0 && total + w > cap) break;
        total += w;
        i += n;
    }
    while (s[i] && i < endb) {
        int n = 0, w = step_cp(s, i, &n);
        if (w != 0 || i + n > endb) break;
        i += n;
    }
    *out = i;
    return total;
}

/* 0 means "not a parser token here" (*end unchanged). A string or comment
   wider than the pane is left to the hand scanner so spaces still break. */
static int ts_token_width(const char *s, int i, int cap, int *end) {
    int start = 0, endb = 0, kind = 0, out = i, w;
    if (!ts_cover(s, i, &start, &endb, &kind)) return 0;
    w = measure_capped(s, i, endb, cap, &out);
    if ((kind == TS_STR || kind == TS_CMT) && !(out == endb && w > 0 && w < cap)) return 0;
    if (out <= i) return 0;
    *end = out;
    return w;
}

/* Cell width of the token at i, capped near `cap`. *end is the byte after the token. */
static int token_width(const char *s, int i, const struct wrap_st *st, int cap, int *end) {
    int n = 0, w, total = 0;
    char quote = st ? st->quote : 0;
    int comment = st && (st->comment || st->block);
    *end = i;
    if (!s || !s[i] || cap < 1) return 0;

    if (comment) {
        if (st->block && s[i] == '*' && s[i + 1] == '/') { *end = i + 2; return 2; }
        if (s[i] == ' ' || s[i] == '\t') {
            while ((s[i] == ' ' || s[i] == '\t') && total < cap) {
                w = (s[i] == '\t') ? 2 : 1;
                if (total > 0 && total + w > cap) break;
                total += w;
                i++;
                *end = i;
            }
            return total;
        }
        while (s[i] && s[i] != ' ' && s[i] != '\t' && total < cap) {
            if (st->block && s[i] == '*' && s[i + 1] == '/') break;
            w = step_cp(s, i, &n);
            if (w == 0) { i += n; *end = i; continue; }
            if (total > 0 && total + w > cap) break;
            total += w;
            i += n;
            *end = i;
        }
        return total;
    }

    if (quote) {
        if (s[i] == '\\' && s[i + 1]) {
            int n2 = 0;
            w = step_cp(s, i, &n);
            int w2 = step_cp(s, i + n, &n2);
            *end = i + n + n2;
            return w + w2;
        }
        if (s[i] == quote) { *end = i + 1; return 1; }
        while (s[i] && s[i] != ' ' && s[i] != '\t' && s[i] != quote && s[i] != '\\' && total < cap) {
            w = step_cp(s, i, &n);
            if (w == 0) { i += n; *end = i; continue; }
            if (total > 0 && total + w > cap) break;
            total += w;
            i += n;
            *end = i;
        }
        return total;
    }

    {
        int tws = ts_token_width(s, i, cap, end);
        if (tws > 0 || *end > i) return tws;
    }

    if (s[i] == ' ' || s[i] == '\t') {
        while ((s[i] == ' ' || s[i] == '\t') && total < cap) {
            w = (s[i] == '\t') ? 2 : 1;
            if (total > 0 && total + w > cap) break;
            total += w;
            i++;
            *end = i;
        }
        return total;
    }
    if (s[i] == '"' || s[i] == '\'' || s[i] == '`') {
        char q = s[i];
        int j = i + 1;
        int tw = 1;
        int closed = 0;
        while (s[j]) {
            if (s[j] == '\\' && s[j + 1]) {
                int n1 = 0, n2 = 0;
                int w1 = step_cp(s, j, &n1);
                int w2 = step_cp(s, j + n1, &n2);
                if (tw > cap - (w1 + w2) || tw + w1 + w2 > cap) break;
                tw += w1 + w2;
                j += n1 + n2;
                continue;
            }
            if (s[j] == q) { tw += 1; j++; closed = 1; break; }
            w = step_cp(s, j, &n);
            if (w == 0) { j += n; continue; }
            if (tw > cap - w || tw + w > cap) break;
            tw += w;
            j += n;
        }
        if (closed && tw <= cap) { *end = j; return tw; }
        *end = i + 1;
        return 1;
    }
    if (s[i] == '/' && (s[i + 1] == '/' || s[i + 1] == '*')) { *end = i + 2; return 2; }
    if (isdigit((unsigned char)s[i]) || (s[i] == '.' && isdigit((unsigned char)s[i + 1]))) {
        int j = i;
        if (s[j] == '.') j++;
        while (isdigit((unsigned char)s[j])) j++;
        while (s[j] == '.' && isdigit((unsigned char)s[j + 1])) {
            j++;
            while (isdigit((unsigned char)s[j])) j++;
        }
        while (i < j && total < cap) {
            w = step_cp(s, i, &n);
            if (w == 0) { i += n; continue; }
            if (total > 0 && total + w > cap) break;
            total += w;
            i += n;
        }
        *end = i;
        return total;
    }
    if (s[i] == '.' && !isdigit((unsigned char)s[i + 1])) {
        int j = i + 1;
        total = 1;
        while (s[j] && (isalnum((unsigned char)s[j]) || s[j] == '_') && total < cap) {
            total++;
            j++;
        }
        *end = j;
        return total;
    }
    if (isalnum((unsigned char)s[i]) || s[i] == '_') {
        while (s[i] && (isalnum((unsigned char)s[i]) || s[i] == '_') && total < cap) {
            w = step_cp(s, i, &n);
            if (w == 0) { i += n; *end = i; continue; }
            if (total > 0 && total + w > cap) break;
            total += w;
            i += n;
            *end = i;
        }
        return total;
    }
    w = step_cp(s, i, &n);
    *end = i + n;
    return w > 0 ? w : 0;
}

/* Byte where the visual row starting at `from` ends. Token edges come from
   tree-sitter JavaScript; whitespace, a too-wide string, and a too-wide
   comment fall back to the hand scanner. A token wider than textw breaks
   mid-token. Always advances when `from` is not the end. */
/* GCC 16 IPA constant propagation mis-specializes this helper to width 1
   when the renderer is included in the cell-level regression harness.
   Keep its width argument dynamic; the same helper drives cursor placement. */
static int __attribute__((noipa)) editor_row_end(const char *line, int from, int textw, struct wrap_st *st) {
    struct wrap_st local = {0};
    int i, col, cap;
    if (!st) st = &local;
    if (!line) return from > 0 ? from : 0;
    if (from < 0) from = 0;
    if (textw < 1) textw = 1;
    cap = (textw < INT_MAX) ? textw + 1 : textw;
    i = from;
    col = 0;
    while (line[i] && col < textw) {
        int end = i;
        int tw = token_width(line, i, st, cap, &end);
        if (end <= i) { i++; continue; }
        if (tw <= 0) { i = end; continue; }
        if (col > 0 && tw <= textw && col + tw > textw) break;
        if (tw <= textw - col) {
            wrap_take(line, i, end, st);
            col += tw;
            i = end;
            continue;
        }
        while (line[i] && col < textw) {
            int n = 0, w = step_cp(line, i, &n);
            int un = n, uw = w;
            if (st->quote && line[i] == '\\' && line[i + n]) {
                int n2 = 0, w2 = step_cp(line, i + n, &n2);
                un = n + n2;
                uw = w + w2;
            }
            if (un < 1) un = 1;
            if (uw == 0) { i += un; continue; }
            if (col > 0 && col + uw > textw) goto row_done;
            wrap_take(line, i, i + un, st);
            col += uw;
            i += un;
        }
        break;
    }
row_done:
    if (i == from && line[from]) {
        int n = 0;
        step_cp(line, i, &n);
        if (n < 1) n = 1;
        wrap_take(line, i, i + n, st);
        i += n;
    }
    return i;
}

/* Screen row and cell of the gap before byte index `byte`. A cursor that
   lands on a full row, or on a soft break with more text after it, sits
   at column 0 of the next row. */
static void editor_place(const char *line, int byte, int textw, int *row, int *cell) {
    int r = 0, i = 0;
    struct wrap_st st = {0};
    if (textw < 1) textw = 1;
    if (!line) line = "";
    if (byte < 0) byte = 0;
    while (line[i] && i < byte) {
        int end = editor_row_end(line, i, textw, &st);
        if (end <= i) end = i + 1;
        if (byte < end) {
            if (row) *row = r;
            if (cell) *cell = span_cells(line, i, byte);
            return;
        }
        if (byte == end) {
            if (line[end]) { i = end; r++; continue; }
            int width = span_cells(line, i, end);
            if (width >= textw) {
                if (row) *row = r + 1;
                if (cell) *cell = 0;
                return;
            }
            if (row) *row = r;
            if (cell) *cell = width;
            return;
        }
        i = end;
        r++;
    }
    if (row) *row = r;
    if (cell) *cell = 0;
}

static int editor_content_rows(const char *line, int textw) {
    int rows = 1, i = 0;
    struct wrap_st st = {0};
    if (textw < 1) textw = 1;
    if (!line || !line[0]) return 1;
    while (line[i]) {
        int end = editor_row_end(line, i, textw, &st);
        if (end <= i) break;
        i = end;
        if (line[i]) rows++;
    }
    return rows;
}

static int editor_visual_col(const char *line, int byte) {
    int cells = 0, i = 0;
    if (!line || byte <= 0) return 0;
    while (line[i] && i < byte) {
        int n = 0;
        unsigned cp = utf8_decode(line + i, &n);
        if (i + n > byte) break;
        cells += editor_cell_advance(cp);
        i += n;
    }
    return cells;
}

static void editor_row_span(const char *line, int textw, int sub, int *b0, int *b1) {
    int row = 0, i = 0;
    struct wrap_st st = {0};
    if (textw < 1) textw = 1;
    if (sub < 0) sub = 0;
    if (!line) { *b0 = *b1 = 0; return; }
    while (line[i]) {
        int start = i;
        int end = editor_row_end(line, i, textw, &st);
        if (end <= i) end = i + 1;
        if (row == sub) { *b0 = start; *b1 = end; return; }
        if (!line[end]) {
            if (sub == row + 1) { *b0 = *b1 = end; return; }
            break;
        }
        i = end;
        row++;
    }
    *b0 = *b1 = i;
}

static void editor_scroll_span(const char *line, int view, int textw, int *b0, int *b1) {
    int i = 0, col = 0, started = 0;
    *b0 = *b1 = 0;
    if (textw < 1) textw = 1;
    if (view < 0) view = 0;
    if (!line) return;
    while (line[i]) {
        int n = 0;
        unsigned cp = utf8_decode(line + i, &n);
        int w = editor_cell_advance(cp);
        int next = col + (w > 0 ? w : 0);
        if (w > 0 && next > view && col < view + textw) {
            if (!started) { *b0 = i; started = 1; }
            *b1 = i + n;
        }
        if (col >= view + textw) break;
        col = next;
        i += n;
    }
    if (!started) *b0 = *b1 = i;
}

static int editor_byte_at(const char *line, int sub, int cells, int textw) {
    int row = 0, i = 0;
    struct wrap_st st = {0};
    if (!line) return 0;
    if (textw < 1) textw = 1;
    if (sub < 0) sub = 0;
    if (cells < 0) cells = 0;
    while (line[i]) {
        int end = editor_row_end(line, i, textw, &st);
        if (end <= i) return i;
        if (row == sub) {
            int col = 0, k = i;
            while (k < end) {
                int n = 0, w = step_cp(line, k, &n);
                if (k + n > end) return k;
                if (w == 0) { k += n; continue; }
                if (col >= cells || col + w > cells) return k;
                col += w;
                k += n;
            }
            return end;
        }
        if (!line[end]) return end;
        i = end;
        row++;
    }
    return i;
}

/* Byte index of the gap at absolute visual column `cells` (no wrapping). */
static int editor_byte_at_col(const char *line, int cells) {
    int col = 0, i = 0;
    if (!line) return 0;
    if (cells < 0) cells = 0;
    while (line[i]) {
        int n = 0;
        unsigned cp = utf8_decode(line + i, &n);
        int w = editor_cell_advance(cp);
        if (w == 0) { i += n; continue; }
        if (col >= cells || col + w > cells) return i;
        col += w;
        i += n;
    }
    return i;
}

/* write text clipped to maxcells cells (controls -> space, tabs -> 2sp);
   color 0 = keep current. Returns cells written. */
static int draw_text(int r, int c, const char *text, int maxcells, int color) {
    if (maxcells <= 0 || !text) return 0;
    terminal_move(r, c);
    if (color) terminal_use_role(color);
    int cells = 0;
    const char *p = text;
    while (*p && cells < maxcells) {
        int len = 0;
        unsigned cp = utf8_decode(p, &len);
        if (cp == '\t') {
            if (cells + 2 > maxcells) break;
            terminal_write_padded((char *)"  ", 2);
            cells += 2;
            p += len;
        } else if (cp < 0x20 || cp == 0x7f) {
            terminal_write_padded((char *)" ", 1);
            cells += 1;
            p += len;
        } else {
            int w = cell_width(cp);
            if (w == 0) { p += len; continue; }
            if (cells + w > maxcells) break;
            terminal_write_n((char *)p, (unsigned)len);
            p += len;
            cells += w;
        }
    }
    if (color) terminal_style_reset();
    return cells;
}

static void draw_status(void) {
    rows = terminal_rows; cols = terminal_cols;
    terminal_move(0, 0);
    terminal_style_reverse();
    const char *mode_str = input_mode==2 ? "COMMAND" : input_mode==1 ? "PROMPT" : (vi_mode ? (vi_normal ? "VI:NORMAL" : "VI:INSERT") : "INSERT");
    if (!input_mode && vi_mode && vi_normal && sel_active)
        mode_str = sel_linewise ? "VI:VISUAL LINE" : "VI:VISUAL";
    char songstr[64] = {0};
    {
        int n = scheduler_song_count();
        int si = scheduler_song_pos();
        if (scheduler_song_on() && si >= 0)
            snprintf(songstr, sizeof(songstr), " SONG %d/%d%s", si + 1, n,
                scheduler_song_hold() ? " HOLD" : "");
        else if (n > 0)
            snprintf(songstr, sizeof(songstr), " TRK %d/%d%s", si + 1, n,
                scheduler_song_hold() ? " HOLD" : "");
        else if (scheduler_song_hold())
            snprintf(songstr, sizeof(songstr), " HOLD");
    }
    char buf[512];
    if (first_err[0])
        snprintf(buf, sizeof(buf), "Tompouce [%s] BPM:%.0f %s trk:%d%s | ! %.300s",
            mode_str, audio_get_bpm(), audio_is_playing()?"PLAY":"STOP", track_count, songstr,
            first_err);
    else
        snprintf(buf, sizeof(buf), "Tompouce [%s] BPM:%.0f %s trk:%d%s | %s | F1:help F2:AI F3:vi F4:state F5:eval F6:find F7:song F8:hold F9:tracks ^P:play ^Q:quit F12:viz",
            mode_str, audio_get_bpm(), audio_is_playing()?"PLAY":"STOP", track_count, songstr,
            status_msg[0] ? status_msg : "Ready");
    terminal_move(0, 0);
    terminal_style_reverse();
    int used = draw_text(0, 0, buf, cols, 0);
    /* draw_text already positioned; reverse was set before it */
    terminal_style_reset();
    if (used < cols) blank_inner(0, used, cols - used);
}

/* ---- syntax highlight: token classes, terminal color roles ----
   0 normal 2 keyword 3 note 4 drum 5 transform 6 comment 7 rest 8 number */

static int is_transform_tok(const char *t) {
    static const char *ts[] = {
        "rev","fast","slow","gain","pan","inst","s","sound","bank","mute","solo","midi",
        "att","attack","rel","release","dec","decay","sus","sustain","adsr",
        "scale","transpose","add","arp","lfo","vib","pw","lpf","cutoff",
        "shuffle","degrade","room","delay","struct","mask","off","jux","drive","verb",
        "lpq","resonance","lpenv","lpa","speed","end","delaytime","delayfeedback", NULL
    };
    for (int i = 0; ts[i]; i++)
        if (strcmp(t, ts[i]) == 0) return 1;
    return 0;
}
static int is_number_tok(const char *t) {
    if (!*t) return 0;
    for (const char *p = t; *p; p++)
        if (!isdigit((unsigned char)*p) && *p != '.' && *p != '-') return 0;
    return 1;
}
static int is_rest_tok(const char *t) {
    return strcmp(t, "~") == 0 || strcmp(t, "-") == 0 || strcmp(t, "rest") == 0;
}

/* 1 where the byte is part of a note sounding in the current cycle. */
static unsigned char **play_rows;
static int play_ink[192];
static uint32_t play_color[192];
static unsigned play_flags[192];
static unsigned play_theme_revision[192];
static int *play_cap;
static int play_n;
static const char *hl_src;
static const unsigned char *hl_play;
static int hl_play_len;
static int hl_bias;
static char hl_quote;
static int hl_block_comment;
static int hl_eol_comment; /* remainder of this line is a // or # comment */
static int hl_force_comment;

/* Advance quote and comment state over the first n bytes. A // or a leading # sets hl_eol_comment. */
static void highlight_scan(const char *line, int n) {
    hl_eol_comment = 0;
    if (!line || n <= 0) return;
    int s = 0;
    while (s < n && (line[s] == ' ' || line[s] == '\t')) s++;
    if (!hl_quote && !hl_block_comment && s < n && line[s] == '#') {
        hl_eol_comment = 1;
        return;
    }
    for (int p = 0; p < n; p++) {
        if (hl_block_comment) {
            if (p + 1 < n && line[p] == '*' && line[p + 1] == '/') { hl_block_comment = 0; p++; }
            continue;
        }
        if (hl_quote) {
            if (line[p] == '\\' && p + 1 < n) p++;
            else if (line[p] == hl_quote) hl_quote = 0;
        } else if (p + 1 < n && line[p] == '/' && line[p + 1] == '/') {
            hl_eol_comment = 1;
            break;
        } else if (p + 1 < n && line[p] == '/' && line[p + 1] == '*') {
            hl_block_comment = 1;
            p++;
        } else if (line[p] == '"' || line[p] == '\'' || line[p] == '`') hl_quote = line[p];
    }
}

static void highlight_context(const char *line) {
    if (!line) return;
    size_t n = strlen(line);
    if (n > (size_t)INT_MAX) n = (size_t)INT_MAX;
    highlight_scan(line, (int)n);
    /* Ordinary JS strings cannot span an unescaped newline. During typing an
       unfinished quote must not recolor the rest of the buffer as a string. */
    if (hl_quote == '\'' || hl_quote == '"') {
        size_t slashes = 0;
        while (slashes < n && line[n-1-slashes] == '\\') slashes++;
        if (!(slashes&1)) hl_quote = 0;
    }
}

static int play_fit(int n) {
    if (n <= play_n) return 1;
    if (n < 1) return 0;
    unsigned char **np = realloc(play_rows, (size_t)n * sizeof(*np));
    if (!np) return 0;
    play_rows = np;
    int *nl = realloc(play_cap, (size_t)n * sizeof(*nl));
    if (!nl) return 0;
    play_cap = nl;
    for (int i = play_n; i < n; i++) { play_rows[i] = NULL; play_cap[i] = 0; }
    play_n = n;
    return 1;
}

static void fill_play_mask(double now) {
    if (!lines_ready() || !play_fit(code_line_count)) return;
    for (int i = 0; i < code_line_count; i++)
        if (play_rows[i] && play_cap[i] > 0) memset(play_rows[i], 0, (size_t)play_cap[i]);
    if (!audio_is_playing()) return;
    char *flat = editor_flatten();
    if (!flat) return;
    int off[192], len[192];
    uint32_t colors[192]; unsigned flags[192];
    int n = scheduler_editor_play_styles(now, flat, off, len, colors, flags, 192);
    free(flat);
    for (int s=0;s<n;s++) {
        if (!(flags[s]&(VISUAL_COLOR|VISUAL_BOLD|VISUAL_UNDERLINE|VISUAL_ITALIC))) { play_ink[s]=10; continue; }
        if (play_ink[s]!=16+s || play_color[s]!=colors[s] || play_flags[s]!=flags[s] || play_theme_revision[s]!=theme_revision) {
            play_theme_revision[s]=theme_revision;
            char background[16]; char *foreground="black";
            if (flags[s]&VISUAL_COLOR) {
                snprintf(background,sizeof(background),"#%06x",(unsigned)(colors[s]>>8));
                unsigned r=colors[s]>>24,g=(colors[s]>>16)&255,b=(colors[s]>>8)&255;
                if (r*299+g*587+b*114<128000) foreground="white";
            } else snprintf(background,sizeof(background),"yellow");
            terminal_define_role(16+s,NULL,foreground,background);
            unsigned attributes=0;
            if (flags[s]&VISUAL_BOLD) attributes|=NCSTYLE_BOLD;
            if (flags[s]&VISUAL_UNDERLINE) attributes|=NCSTYLE_UNDERLINE;
            if (flags[s]&VISUAL_ITALIC) attributes|=NCSTYLE_ITALIC;
            terminal_role_attr(16+s,attributes);
            play_color[s]=colors[s]; play_flags[s]=flags[s];
        }
        play_ink[s]=16+s;
    }
    int base = 0;
    for (int i = 0; i < code_line_count; i++) {
        size_t raw = strlen(code_lines[i]);
        if (raw > (size_t)INT_MAX) break;
        int L = (int)raw;
        if (play_cap[i] < L + 1) {
            unsigned char *p = realloc(play_rows[i], (size_t)L + 1);
            if (!p) { play_rows[i] = NULL; play_cap[i] = 0; continue; }
            play_rows[i] = p;
            play_cap[i] = L + 1;
            memset(p, 0, (size_t)L + 1);
        }
        if (play_rows[i]) {
            for (int s = 0; s < n; s++) {
                int a = off[s];
                int b = off[s] + len[s];
                int x0 = a > base ? a : base;
                int x1 = b < base + L ? b : base + L;
                for (int x = x0; x < x1; x++) play_rows[i][x - base] = (unsigned char)(s+1);
            }
        }
        if (L > INT_MAX - base - 1) break;
        base += L + 1;
    }
}

/* write seg (n chars) in color obj, tracking printed columns against maxw */
/* write at most maxw-*col cells; controls become spaces. Advances *col. */
static void hl_put(const char *seg, int n, int obj, int *col, int maxw) {
    if (n <= 0 || *col >= maxw) return;
    int i = 0;
    while (i < n && seg[i] && *col < maxw) {
        int len = 0;
        unsigned cp = utf8_decode(seg + i, &len);
        if (i + len > n) break; /* don't split caller's segment */
        uintptr_t addr = (uintptr_t)seg, base = (uintptr_t)hl_src;
        int at = (hl_src && addr >= base && addr - base < strlen(hl_src))
            ? (int)(addr - base) + i : -1;
        int absat = at < 0 ? -1 : hl_bias + at;
        int sounding = hl_play && absat >= 0 && absat < hl_play_len ? hl_play[absat] : 0;
        int chosen = at >= 0 && selected_byte(hl_line, absat);
        int ink = chosen ? 11 : (sounding ? (sounding<=192 && play_ink[sounding-1]?play_ink[sounding-1]:10) : obj);
        if (cp == '\t') {
            if (*col + 2 > maxw) break;
            if (ink) terminal_use_role(ink);
            terminal_write_padded((char *)"  ", 2);
            *col += 2;
        } else if (cp < 0x20 || cp == 0x7f) {
            if (ink) terminal_use_role(ink);
            terminal_write_padded((char *)" ", 1);
            *col += 1;
        } else {
            int w = cell_width(cp);
            if (w == 0) { i += len; continue; }
            if (*col + w > maxw) break;
            if (ink) terminal_use_role(ink);
            terminal_write_n((char *)(seg + i), (unsigned)len);
            *col += w;
        }
        if (ink) terminal_style_reset();
        i += len;
    }
}

/* Color one quoted mini-notation. drum=1 paints sample names as drums. */
static void hl_quoted_pattern(const char *s, int n, int drum, int *col, int maxw) {
    int i = 0;
    while (i < n && *col < maxw) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '[' || c == ']' || c == '<' || c == '>' ||
            c == '(' || c == ')' || c == ',' || c == '{' || c == '}' || c == '|') {
            hl_put(s + i, 1, 0, col, maxw);
            i++;
            continue;
        }
        if (c == '*' || c == '!' || c == '@' || c == '/' || c == '?' ||
            c == ':' || c == '%' || c == '^') {
            hl_put(s + i, 1, 8, col, maxw);
            i++;
            int di = 0;
            while (i + di < n && (isdigit((unsigned char)s[i + di]) || s[i + di] == '.') && di < 15)
                di++;
            if (di) hl_put(s + i, di, 8, col, maxw);
            i += di;
            continue;
        }
        int ti = 0;
        while (i + ti < n && ti < 63) {
            char ch = s[i + ti];
            if (ch == ' ' || ch == '\t' || ch == '|' || ch == '[' || ch == ']' ||
                ch == '<' || ch == '>' || ch == '(' || ch == ')' || ch == ',' ||
                ch == '{' || ch == '}' || ch == '*' || ch == '!' || ch == '@' ||
                ch == '/' || ch == '?' || ch == ':' || ch == '%' || ch == '^') break;
            ti++;
        }
        if (ti == 0) { i++; continue; }
        char t[64];
        memcpy(t, s + i, (size_t)ti);
        t[ti] = 0;
        int obj = is_rest_tok(t) ? 7 : (is_number_tok(t) ? 8 : (drum ? 4 : 3));
        hl_put(s + i, ti, obj, col, maxw);
        i += ti;
    }
}

/* note("c e").s("saw").lpf(800) and $: s("hh*4").bank("tr909") */
static int draw_hl_statement(const char *line, int maxw) {
    int col = 0;
    const char *p = line;
    int seen_call = 0;
    int call_drum = 0;
    int first_string = 1;
    char lastname[32] = {0};
    while (*p && col < maxw) {
        if (p[0] == '/' && p[1] == '/') {
            hl_put(p, (int)strlen(p), 6, &col, maxw);
            break;
        }
        if (p[0] == '/' && p[1] == '*') {
            const char *end = strstr(p+2,"*/");
            int n = end ? (int)(end+2-p) : (int)strlen(p);
            hl_put(p,n,6,&col,maxw); p += n; continue;
        }
        if (*p == ' ' || *p == '\t') { hl_put(p, 1, 0, &col, maxw); p++; continue; }
        if (*p == '$' && p[1] == ':') { hl_put(p, 2, 2, &col, maxw); p += 2; continue; }
        if (*p == '"' || *p == '\'' || *p == '`') {
            char qch = *p;
            hl_put(p, 1, 0, &col, maxw);
            p++;
            const char *start = p;
            while (*p && *p != qch) { if (*p == '\\' && p[1]) p++; p++; }
            int n = (int)(p - start);
            int plain = strcmp(lastname, "bank") == 0 || strcmp(lastname, "scale") == 0 ||
                strcmp(lastname, "samples") == 0;
            if (plain)
                hl_put(start, n, 5, &col, maxw);
            else
                hl_quoted_pattern(start, n, (call_drum && first_string) ||
                    strcmp(lastname, "s") == 0 || strcmp(lastname, "sound") == 0, &col, maxw);
            first_string = 0;
            if (*p == qch) { hl_put(p, 1, 0, &col, maxw); p++; }
            continue;
        }
        if (isalpha((unsigned char)*p) || *p == '_') {
            const char *st = p;
            while (isalnum((unsigned char)*p) || *p == '_') p++;
            int n = (int)(p - st);
            char name[32];
            int nc = n < 31 ? n : 31;
            memcpy(name, st, (size_t)nc);
            name[nc] = 0;
            int obj = 0;
            const char *keywords[] = {"const","let","var","function","return","if","else","for","while",
                "class","new","import","export","from","as","await","async","true","false","null",
                "undefined","throw","try","catch","switch","case","break","continue","yield",NULL};
            for (int i = 0; keywords[i]; i++) if (!strcmp(name,keywords[i])) { obj = 2; break; }
            const char *next = p;
            while (*next == ' ' || *next == '\t') next++;
            if (*next == '(') {
                if (!seen_call) {
                    obj = st > line && st[-1] == '.' ? 5 : 2;
                    seen_call = 1;
                    call_drum = (strcmp(name, "s") == 0 || strcmp(name, "sound") == 0);
                } else {
                    obj = 5;
                }
                snprintf(lastname, sizeof(lastname), "%s", name);
            }
            if (!obj && (strcmp(name, "sine") == 0 || strcmp(name, "saw") == 0 ||
                strcmp(name, "tri") == 0 || strcmp(name, "square") == 0 ||
                strcmp(name, "rand") == 0 || strcmp(name, "perlin") == 0 ||
                strcmp(name, "cosine") == 0 || strcmp(name, "silence") == 0)) obj = 5;
            if (!obj && is_transform_tok(name)) obj = 5;
            hl_put(st, n, obj, &col, maxw);
            continue;
        }
        if (isdigit((unsigned char)*p) || (*p == '.' && isdigit((unsigned char)p[1]))) {
            const char *st = p;
            if (*p == '.') p++;
            while (isdigit((unsigned char)*p) || *p == '.') p++;
            hl_put(st, (int)(p - st), 8, &col, maxw);
            continue;
        }
        if (*p == '+' || *p == '-' || *p == '*' || *p == '/' || *p == '%') {
            hl_put(p, 1, 8, &col, maxw);
            p++;
            continue;
        }
        int bytes = 1;
        if ((unsigned char)*p >= 0x80) (void)utf8_decode(p,&bytes);
        hl_put(p, bytes, 0, &col, maxw);
        p += bytes;
    }
    return col;
}

/* highlighted code line (without line-number prefix); maxw = pane width-2 */
static int draw_hl_line(const char *line, int maxw) {
    int col = 0;
    const char *p = line;
    hl_src = line;
    if (hl_force_comment) {
        size_t L = line ? strlen(line) : 0;
        if (L > (size_t)INT_MAX) L = (size_t)INT_MAX;
        hl_put(line, (int)L, 6, &col, maxw);
        return col;
    }
    if (hl_block_comment) {
        const char *end = strstr(line,"*/");
        int n = end ? (int)(end+2-line) : (int)strlen(line);
        hl_put(line,n,6,&col,maxw);
        if (end) col += draw_hl_statement(end+2,maxw-col);
        return col;
    }
    if (hl_quote) {
        const char *end = line;
        while (*end && *end != hl_quote) {
            if (*end == '\\' && end[1]) end++;
            end++;
        }
        hl_quoted_pattern(line, (int)(end - line), 0, &col, maxw);
        if (*end) {
            hl_put(end, 1, 0, &col, maxw);
            int rest = draw_hl_statement(end + 1, maxw - col);
            col += rest;
        }
        return col;
    }
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '#' || (p[0] == '/' && p[1] == '/')) { hl_put(line, (int)strlen(line), 6, &col, maxw); return col; }
    if (*p == 0) { hl_put(line, (int)strlen(line), 0, &col, maxw); return col; }
    return draw_hl_statement(line,maxw);
}

/* FNV-1a over editor text; recheck patterns only when it changes */
static unsigned hash_code(void) {
    unsigned h = 2166136261u;
    for (int i = 0; i < code_line_count; i++) {
        const char *s = code_lines[i];
        while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
        h ^= 0x9e3779b9u;
    }
    return h;
}

static int line_err_clear(void) {
    if (code_line_count < 1) return 1;
    if (code_line_count > line_err_cap) {
        int *p = realloc(line_err, (size_t)code_line_count * sizeof(int));
        if (!p) return 0;
        line_err = p;
        line_err_cap = code_line_count;
    }
    memset(line_err, 0, (size_t)code_line_count * sizeof(int));
    return 1;
}

static void recheck_patterns(void) {
    first_err[0] = 0;
    if (!line_err_clear()) return;
    char *code = editor_flatten();
    if (!code) return;
    int line=1;
    char error[256]={0};
    if (!javascript_check(code,error,sizeof(error),&line)) {
        if (line<1) line=1;
        if (line>code_line_count) line=code_line_count;
        line_err[line-1]=1;
        snprintf(first_err,sizeof(first_err),"L%d: %.200s",line,error);
    }
    free(code);
}


static int scope_level(double v);

/* Slider index in source order. Cursor inside slider(...) wins;
   otherwise the first slider on the line. -1 when the line has none. */
static char *slider_document;
static unsigned slider_document_generation;
static int slider_offsets[64];
static struct { int index, a, b, call_a, call_b, line; } slider_sites[64];
static int slider_sites_count;

static void slider_record_site(TSNode node, const char *text) {
    TSNode fn=ts_node_child_by_field_name(node,"function",8);
    if (ts_node_is_null(fn) || strcmp(ts_node_type(fn),"identifier")) return;
    int a=(int)ts_node_start_byte(fn), b=(int)ts_node_end_byte(fn);
    if (b-a!=6 || memcmp(text+a,"slider",6)) return;
    TSNode args=ts_node_child_by_field_name(node,"arguments",9);
    if (ts_node_is_null(args)) return;
    TSNode arg=ts_node_named_child(args,0);
    /* Empty and partially typed calls are ordinary editor states. Tree-sitter
       byte/point accessors must never receive a null child. */
    if (ts_node_is_null(arg)) return;
    const char *kind=ts_node_type(arg);
    if (strcmp(kind,"number") && strcmp(kind,"unary_expression")) return;
    int start=(int)ts_node_start_byte(arg), end=(int)ts_node_end_byte(arg);
    for (int i=0;i<slider_count() && i<64;i++) if (slider_offsets[i]==start && slider_sites_count<64) {
        int n=slider_sites_count++;
        slider_sites[n].index=i; slider_sites[n].a=start; slider_sites[n].b=end;
        slider_sites[n].call_a=a; slider_sites[n].call_b=(int)ts_node_end_byte(node);
        slider_sites[n].line=(int)ts_node_start_point(node).row;
        break;
    }
}

static void slider_walk(TSNode node, const char *text, int depth) {
    if (depth>128 || ts_node_is_null(node)) return;
    const char *type=ts_node_type(node);
    if (!strcmp(type,"comment") || !strcmp(type,"string") || !strcmp(type,"template_string")) return;
    if (!strcmp(type,"call_expression")) slider_record_site(node,text);
    for (uint32_t i=0;i<ts_node_named_child_count(node);i++) slider_walk(ts_node_named_child(node,i),text,depth+1);
}

static void slider_refresh_sites(void) {
    static unsigned cached_hash;
    unsigned hash=hash_code();
    pthread_mutex_lock(&tracks_lock);
    unsigned generation=track_count && tracks[0].javascript?tracks[0].compile_generation:0;
    if (slider_document && generation==slider_document_generation && hash==cached_hash) {
        pthread_mutex_unlock(&tracks_lock); return;
    }
    char *text=editor_flatten();
    if (!text) { pthread_mutex_unlock(&tracks_lock); return; }
    if (generation!=slider_document_generation || !slider_document) {
        free(slider_document);
        const char *source=track_count?javascript_source(tracks[0].javascript):NULL;
        slider_document=strdup_safe(source?source:text);
        slider_document_generation=generation;
        slider_sites_count=0;
        for (int i=0;i<64;i++) slider_offsets[i]=slider_source_offset(i);
    }
    pthread_mutex_unlock(&tracks_lock);
    if (!slider_document) { free(text); return; }
    if (strcmp(text,slider_document)) {
        int old=(int)strlen(slider_document), size=(int)strlen(text), prefix=0, suffix=0;
        while (prefix<old && prefix<size && slider_document[prefix]==text[prefix]) prefix++;
        while (suffix<old-prefix && suffix<size-prefix && slider_document[old-1-suffix]==text[size-1-suffix]) suffix++;
        for (int i=0;i<64;i++) {
            int at=slider_offsets[i];
            if (at<0) continue;
            if (at>=old-suffix) slider_offsets[i]+=size-old;
            else if (at>prefix) slider_offsets[i]=-1;
        }
    }
    cached_hash=hash;
    free(slider_document); slider_document=text; slider_sites_count=0;
    if (!ts_parser) (void)ts_ensure("");
    if (!ts_parser) return;
    TSTree *tree=ts_parser_parse_string(ts_parser,NULL,text,(uint32_t)strlen(text));
    if (tree) { slider_walk(ts_tree_root_node(tree),text,0); ts_tree_delete(tree); }
}

static int slider_write_value(int index) {
    slider_refresh_sites();
    double value;
    if (!slider_info(index,&value,NULL,NULL,NULL)) return 0;
    for (int i=0;i<slider_sites_count;i++) if (slider_sites[i].index==index) {
        char number[48]; snprintf(number,sizeof(number),"%.15g",value);
        int a=slider_sites[i].a, b=slider_sites[i].b, line=slider_sites[i].line;
        int old_line=cur_line, old_col=cur_col, selected=sel_active, sl=sel_line, sc=sel_col;
        int column=a-editor_offset(line,0), delta=(int)strlen(number)-(b-a);
        if (!editor_replace(a,b,number)) return 0;
        cur_line=old_line; cur_col=old_col;
        if (cur_line==line && cur_col>column) cur_col=cur_col>=column+b-a?cur_col+delta:column+(int)strlen(number);
        sel_active=selected; sel_line=sl; sel_col=sc;
        if (sl==line && sc>column) sel_col=sc>=column+b-a?sc+delta:column+(int)strlen(number);
        slider_refresh_sites();
        return 1;
    }
    return 0;
}

static int slider_on_line(int line, int col) {
    slider_refresh_sites();
    if (slider_document_generation) {
        int first=-1, at=editor_offset(line,col);
        for (int i=0;i<slider_sites_count;i++) if (slider_sites[i].line==line) {
            if (first<0) first=slider_sites[i].index;
            if (at>=slider_sites[i].call_a && at<=slider_sites[i].call_b) return slider_sites[i].index;
        }
        return first;
    }
    int idx = 0, first = -1, hit = -1;
    if (line < 0 || line >= code_line_count) return -1;
    for (int ln = 0; ln < code_line_count; ln++) {
        const char *s = code_lines[ln];
        for (const char *p = s; (p = strstr(p, "slider(")) != NULL; ) {
            const char *end = strchr(p, ')');
            int a = (int)(p - s);
            int b = end ? (int)(end - s) : a + 6;
            if (ln == line) {
                if (first < 0) first = idx;
                if (col >= a && col <= b) hit = idx;
            }
            idx++;
            p += 7;
        }
    }
    return hit >= 0 ? hit : first;
}

/* Index under the cursor, or -1. Bind order matches left-to-right slider(. */
static int slider_at_cursor(void) {
    slider_refresh_sites();
    if (slider_document_generation) return slider_on_line(cur_line,cur_col);
    int idx = 0;
    for (int ln = 0; ln < code_line_count; ln++) {
        const char *s = code_lines[ln];
        for (const char *p = s; (p = strstr(p, "slider(")) != NULL; ) {
            const char *end = strchr(p, ')');
            int a = (int)(p - s);
            int b = end ? (int)(end - s) : (int)strlen(s);
            if (ln == cur_line && cur_col >= a && cur_col <= b) return idx;
            idx++;
            p += 7;
        }
    }
    return -1;
}

static void draw_slider_chip(int row, int col, int width, int idx) {
    double v = 0, mn = 0, mx = 1;
    if (width < 12 || !slider_info(idx, &v, &mn, &mx, NULL)) return;
    double span = mx - mn;
    double t = (span > 1e-12) ? (v - mn) / span : 0;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    char bar[12];
    int knob = (int)(t * 9.0);
    if (knob > 9) knob = 9;
    for (int i = 0; i < 10; i++) bar[i] = (i == knob) ? '*' : '-';
    bar[10] = 0;
    char lab[24];
    snprintf(lab, sizeof(lab), "%s %.2f", bar, v);
    draw_text(row, col, lab, width, 8);
}

/* Recent accepted notes; never parse or evaluate user code on the UI thread. */
static void draw_line_roll(int row, int c0, int w) {
    if (w<8) return;
    int rw=w>240?240:w;
    char strip[241]; memset(strip,'.',(size_t)rw); strip[rw]=0;
    NoteHist notes[NOTE_HIST_N];
    int count=note_hist_copy(notes,NOTE_HIST_N);
    double now=audio_time_now(),span=audio_sec_per_cycle();
    if (!isfinite(span) || span<=0) span=2;
    double from=now-span;
    for (int i=0;i<count;i++) {
        if (notes[i].t0>now || notes[i].t0+notes[i].dur<from) continue;
        int first=(int)(fmax(0,(notes[i].t0-from)/span)*rw);
        int last=(int)(fmin(1,(notes[i].t0+notes[i].dur-from)/span)*rw);
        if (last<=first) last=first+1;
        if (last>rw) last=rw;
        for (int x=first;x<last && x<rw;x++) strip[x]='#';
    }
    terminal_move(row,c0); terminal_write_padded(strip,(unsigned)rw);
    if (rw<w) blank_inner(row,c0+rw,w-rw);
}

static void draw_mini_scope(int row, int c0, int w) {
    if (w < 8) return;
    int rw = w;
    if (rw > 240) rw = 240;
    float sbuf[480];
    int sn = rw * 2;
    if (sn > 480) sn = 480;
    if (sn < 2) sn = 2;
    audio_scope_copy(sbuf, sn);
    static const char lvch[] = " .:-=+*#";
    terminal_move(row, c0);
    for (int x = 0; x < rw; x++) {
        int i0 = x * sn / rw;
        int i1 = (x + 1) * sn / rw;
        if (i1 <= i0) i1 = i0 + 1;
        if (i1 > sn) i1 = sn;
        int hi = 0;
        for (int i = i0; i < i1; i++) {
            int l = scope_level(sbuf[i]);
            if (l > hi) hi = l;
        }
        if (hi > 7) hi = 7;
        char c[2] = { lvch[hi], 0 };
        terminal_write(c);
    }
    if (rw < w) blank_inner(row, c0 + rw, w - rw);
}

/* One terminal cell contains a 2x4 pixel braille tile. */
static void inline_dot(unsigned char grid[][240], int x, int y, int width, int height) {
    static const unsigned char bits[2][4] = {{1,2,4,64},{8,16,32,128}};
    if (x < 0 || x >= width*2 || y < 0 || y >= height || width > 240 || height > 32) return;
    grid[y/4][x/2] |= bits[x%2][y%4];
}

static int queue_inline_graphic(int line, int kind, int sub, int row, int col,
                                int width, unsigned feed, unsigned generation) {
    if (graphics_disabled || !graphics_supported || !feed || width < 1) return 0;
    if (graphics_pending_count > 0) {
        InlineGraphic *last = &graphics_pending[graphics_pending_count-1];
        if (last->line == line && last->kind == kind && last->row+last->count == row) {
            last->count++;
            return 1;
        }
    }
    if (graphics_pending_count == GRAPHICS_IMAGES) return 0;
    InlineGraphic next = {line,kind,row,col,width,sub-1,1,graphics_cell_width,graphics_cell_height,feed,generation};
    graphics_pending[graphics_pending_count++] = next;
    return 1;
}

static void inline_graphics_clear(void) {
    for (int i = 0; i < graphics_previous_count; i++)
        terminal_graphics_delete(terminal_output,GRAPHICS_IMAGE_ID+(unsigned)i);
    graphics_previous_count = 0;
    graphics_redraw = 1;
}

/* Fixed foreground caches, one per compiled attachment; never touched by audio.
   Bounded by the number of inline images that can be displayed at once, with
   LRU replacement, so the 393 KiB previous-frame buffer is not multiplied by
   panes that cannot coexist. */
static TerminalFeedbackState feedback_states[GRAPHICS_IMAGES];
static struct { unsigned feed, generation, palette, revision, used; int kind; } feedback_keys[GRAPHICS_IMAGES];
static unsigned feedback_clock;
static void render_inline_feedback(TerminalImage *image, int kind, unsigned feed, unsigned generation) {
    VisualOptions options; VisualNote notes[GRAPHICS_NOTES]; float samples[4096];
    visual_options(&options,kind,NULL);
    double origin=0,seconds=2; int count=0,n=0;
    JsProgram *program=NULL;
    pthread_mutex_lock(&tracks_lock);
    if (track_count>0 && tracks[0].compile_generation==generation) {
        program=tracks[0].javascript;
        if (program) javascript_retain(program);
    }
    pthread_mutex_unlock(&tracks_lock);
    unsigned revision=javascript_visual_revision(program);
    if (program) {
        javascript_live_visual_options(program,feed,kind,&options);
        count=javascript_live_visual_copy(program,feed,kind,notes,GRAPHICS_NOTES,&origin,&seconds);
        javascript_free(program);
    }
    if (visual_audio_kind(kind)) n=audio_pattern_scope_copy(feed,generation,samples,4096);
    int slot=0, found=0;
    for (int i=0;i<GRAPHICS_IMAGES;i++) {
        if (feedback_keys[i].used && feedback_keys[i].feed==feed && feedback_keys[i].kind==kind) { slot=i; found=1; break; }
        if (feedback_keys[i].used<feedback_keys[slot].used) slot=i;
    }
    TerminalFeedbackState *state=&feedback_states[slot];
    unsigned palette=terminal_graphics_palette_generation();
    if (!found || feedback_keys[slot].generation!=generation || feedback_keys[slot].palette!=palette || feedback_keys[slot].revision!=revision)
        memset(state,0,sizeof(*state));
    feedback_keys[slot].feed=feed; feedback_keys[slot].kind=kind; feedback_keys[slot].generation=generation;
    feedback_keys[slot].palette=palette; feedback_keys[slot].revision=revision; feedback_keys[slot].used=++feedback_clock;
    terminal_graphics_feedback(image,state,kind,&options,notes,count,samples,n,audio_time_now(),origin,seconds);
}

static void inline_graphics_flush(void) {
    if (!graphics_pending_count && !graphics_previous_count) return;
    int changed = graphics_redraw || graphics_pending_count != graphics_previous_count ||
        memcmp(graphics_pending,graphics_previous,(size_t)graphics_pending_count*sizeof(InlineGraphic));
    double now = now_seconds();
    if (!changed && now-graphics_last_frame < GRAPHICS_FRAME_SECONDS) return;
    graphics_redraw = 0; graphics_last_frame = now;
    for (int i = graphics_pending_count; i < graphics_previous_count; i++)
        terminal_graphics_delete(terminal_output,GRAPHICS_IMAGE_ID+(unsigned)i);
    static TerminalImage image;
    for (int i = 0; i < graphics_pending_count; i++) {
        const InlineGraphic *g = &graphics_pending[i];
        int full_rows = g->kind == VISUAL_SCOPE ? 3 : inline_piano_rows();
        double actual_width = (double)g->width*g->cell_width;
        double actual_height = (double)full_rows*g->cell_height;
        double factor = fmin(1,fmin(GRAPHICS_WIDTH/actual_width,GRAPHICS_HEIGHT/actual_height));
        image.width = (int)(actual_width*factor);
        int cell_height = (int)(g->cell_height*factor);
        if (image.width < 1) image.width = 1;
        if (cell_height < 1) cell_height = 1;
        image.height = full_rows*cell_height;
        image.compact=0;
        render_inline_feedback(&image,g->kind,g->feed,g->generation);
        if (!terminal_graphics_send(terminal_output,GRAPHICS_IMAGE_ID+(unsigned)i,&image,
                                   g->row,g->col,g->width,g->count,g->first*cell_height,g->count*cell_height)) {
            graphics_supported = 0;
            if (graphics_previous_count < i+1) graphics_previous_count = i+1;
            inline_graphics_clear();
            return;
        }
    }
    memcpy(graphics_previous,graphics_pending,(size_t)graphics_pending_count*sizeof(InlineGraphic));
    graphics_previous_count = graphics_pending_count;
}

static void draw_inline_row(int line, int sub, int row, int col, int width) {
    if (width < 1) return;
    if (inline_mask(line)) {
        int kind=1;
        for (;kind<=VISUAL_KINDS;kind++) if (inline_mask(line)&(1<<(kind-1))) {
            int rows=kind==VISUAL_SCOPE?4:inline_piano_rows()+1;
            if (sub<rows) break;
            sub-=rows;
        }
        if (kind>VISUAL_KINDS) return;
        unsigned feed=inline_feeds?inline_feeds[line].mask[kind-1]:0;
        unsigned generation=inline_feeds?inline_feeds[line].generation:0;
        blank_inner(row,col,width);
        if (!sub) {
            char title[128];
            const char *name=kind==VISUAL_PIANOROLL?"piano roll":visual_name(kind);
            snprintf(title,sizeof(title),"%s | %s",name,!feed?"waiting for playing pattern":
                kind==VISUAL_SCOPE?"pattern output":kind==VISUAL_SPECTRUM?"pattern spectrum, 20Hz-20kHz": "pattern notes");
            draw_text(row,col,title,width,8); return;
        }
        if (queue_inline_graphic(line,kind,sub,row,col,width,feed,generation)) return;
        static TerminalImage image;
        static unsigned saved_feed,saved_generation;
        static int saved_kind,saved_width,saved_height;
        static double saved_time;
        image.compact=1; image.width=2*(width<240?width:240);
        image.height=(kind==VISUAL_SCOPE?3:inline_piano_rows())*4;
        double stamp=now_seconds();
        if (saved_feed!=feed || saved_generation!=generation || saved_kind!=kind || saved_width!=image.width ||
            saved_height!=image.height || stamp-saved_time>.01) {
            render_inline_feedback(&image,kind,feed,generation);
            saved_feed=feed; saved_generation=generation; saved_kind=kind;
            saved_width=image.width; saved_height=image.height; saved_time=stamp;
        }
        unsigned char grid[8][240]={{0}};
        terminal_move(row,col); terminal_use_role(8);
        for (int x=0;x<image.width/2;x++) {
            for (int a=0;a<2;a++) for (int b=0;b<4;b++) {
                size_t p=((size_t)((sub-1)*4+b)*image.width+x*2+a)*4;
                unsigned brightness=image.rgba[p]+image.rgba[p+1]+image.rgba[p+2];
                if (brightness && image.rgba[p+3]>=145)
                    inline_dot(grid,x*2+a,(sub-1)*4+b,image.width/2,image.height);
            }
            unsigned bits=grid[sub-1][x];
            terminal_putc(viz_utf8?(uint32_t)(0x2800+bits):(uint32_t)(bits?'*':' '));
        }
        terminal_style_reset(); return;
    }
}

static void right_geom(int *mid_out, int *h_out, int *w_out, int *state_h_out, int *track_h_out);

static int line_chip(int w, int ln) {
    if (w < 40 || ln < 0 || ln >= code_line_count) return 0;
    if (slider_on_line(ln, cur_col) < 0) return 0;
    /* borders, line number, one pad column, and the chip itself, with 8 columns of code left */
    if (w - 2 - 5 - 1 - 16 < 8) return 0;
    return 16;
}

static int line_textw(int w, int ln) {
    /* interior is columns 1..w-2; numbers take 5; one column stays blank before the border or chip */
    int textw = w - 2 - 5 - 1 - line_chip(w, ln);
    if (textw < 1) textw = 1;
    return textw;
}

static int line_text_rows(int ln, int textw) {
    if (!editor_wrap) return 1;
    const char *s = (ln >= 0 && ln < code_line_count && code_lines[ln]) ? code_lines[ln] : "";
    int content = editor_content_rows(s, textw);
    if (ln == cur_line) {
        int r = 0, c = 0;
        editor_place(s, cur_col, textw, &r, &c);
        if (r + 1 > content) content = r + 1;
    }
    return content < 1 ? 1 : content;
}

static int line_screen_rows(int ln, int textw) {
    return line_text_rows(ln,textw) + inline_rows(ln);
}

static void editor_reveal(int visible, int pane_w) {
    if (!lines_ready()) return;
    if (cur_line < 0) cur_line = 0;
    if (cur_line >= code_line_count) cur_line = code_line_count - 1;
    int L = (int)strlen(code_lines[cur_line]);
    if (cur_col < 0) cur_col = 0;
    if (cur_col > L) cur_col = L;
    if (visible < 1) visible = 1;
    if (!editor_wrap) {
        int vrow = 0;
        for (int i = 0; i < cur_line; i++) {
            int add = line_screen_rows(i,line_textw(pane_w,i));
            if (vrow > INT_MAX-add) { vrow = INT_MAX; break; }
            vrow += add;
        }
        if (vrow < view_top) view_top = vrow;
        if (vrow >= view_top + visible) view_top = vrow - visible + 1;
        if (view_top < 0) view_top = 0;
        int textw = line_textw(pane_w, cur_line);
        int cell = editor_visual_col(code_lines[cur_line], cur_col);
        if (cell < view_col) view_col = cell;
        if (cell >= view_col + textw) view_col = cell - textw + 1;
        if (view_col < 0) view_col = 0;
        return;
    }
    view_col = 0;
    int vrow = 0;
    for (int i = 0; i < cur_line; i++) {
        int add = line_screen_rows(i, line_textw(pane_w, i));
        if (vrow > INT_MAX - add) { vrow = INT_MAX; break; }
        vrow += add;
    }
    int r = 0, c = 0;
    editor_place(code_lines[cur_line], cur_col, line_textw(pane_w, cur_line), &r, &c);
    if (vrow <= INT_MAX - r) vrow += r;
    if (vrow < view_top) view_top = vrow;
    if (vrow >= view_top + visible) view_top = vrow - visible + 1;
    if (view_top < 0) view_top = 0;
}

/* Move by visual rows when wrap is on, and by logical lines when it is off.
   The column goal is the cursor's current visual cell. */
static void editor_move_rows(int delta) {
    int mid, h, rw, sh, th, pane_w, vrow, cell, target, acc;
    if (!lines_ready() || delta == 0) return;
    if (cur_line < 0) cur_line = 0;
    if (cur_line >= code_line_count) cur_line = code_line_count - 1;
    if (cur_col < 0) cur_col = 0;
    if (!editor_wrap) {
        int line = cur_line + delta;
        int L;
        if (line < 0) line = 0;
        if (line >= code_line_count) line = code_line_count - 1;
        cur_line = line;
        L = (int)strlen(code_lines[cur_line]);
        if (cur_col > L) cur_col = L;
        return;
    }
    right_geom(&mid, &h, &rw, &sh, &th);
    (void)h; (void)rw; (void)sh; (void)th;
    pane_w = mid > 0 ? mid : 1;
    vrow = 0;
    for (int i = 0; i < cur_line; i++) {
        int add = line_screen_rows(i, line_textw(pane_w, i));
        if (vrow > INT_MAX - add) { vrow = INT_MAX; break; }
        vrow += add;
    }
    {
        int r = 0;
        int tw = line_textw(pane_w, cur_line);
        cell = 0;
        editor_place(code_lines[cur_line], cur_col, tw, &r, &cell);
        if (vrow > INT_MAX - r) vrow = INT_MAX;
        else vrow += r;
    }
    if (delta > 0 && vrow > INT_MAX - delta) target = INT_MAX;
    else target = vrow + delta;
    if (target < 0) target = 0;
    acc = 0;
    for (int ln = 0; ln < code_line_count; ln++) {
        int ltw = line_textw(pane_w, ln);
        int nrows = line_screen_rows(ln, ltw);
        if (nrows < 1) nrows = 1;
        if (target < acc + nrows) {
            int textrows = line_text_rows(ln,ltw);
            if (target-acc >= textrows) {
                if (delta > 0 && ln+1 < code_line_count) {
                    cur_line = ln+1;
                    cur_col = editor_byte_at(code_lines[cur_line],0,cell,line_textw(pane_w,cur_line));
                } else {
                    cur_line = ln;
                    cur_col = editor_byte_at(code_lines[ln],textrows-1,cell,ltw);
                }
                return;
            }
            cur_line = ln;
            cur_col = editor_byte_at(code_lines[ln], target - acc, cell, ltw);
            return;
        }
        if (acc > INT_MAX - nrows) break;
        acc += nrows;
    }
    cur_line = code_line_count - 1;
    {
        int tw = line_textw(pane_w, cur_line);
        int last = line_text_rows(cur_line, tw) - 1;
        if (last < 0) last = 0;
        cur_col = editor_byte_at(code_lines[cur_line], last, cell, tw);
    }
}

#include "tui_vi.h"

static void draw_code(void) {
    int mid, h, rw, sh, th;
    editor_cur_on = 0;
    right_geom(&mid, &h, &rw, &sh, &th);
    (void)rw; (void)sh; (void)th;
    int w = mid;
    if (w < 2 || h < 2) return;
    terminal_box(1, 0, h, w);
    terminal_move(1, 2);
    terminal_write(pane_focus
        ? (editor_wrap ? " code wrap " : " code scroll ")
        : (editor_wrap ? " code wrap [active] " : " code scroll [active] "));
    int foot = editor_footer(h);
    int visible = h - 2 - foot;
    if (visible < 1) visible = 1;
    editor_reveal(visible, w);
    fill_play_mask(audio_time_now());
    hl_quote = 0;
    hl_block_comment = 0;
    int acc = 0, first = 0;
    while (first < code_line_count) {
        int nrows = line_screen_rows(first, line_textw(w, first));
        if (acc > view_top - nrows) break;
        highlight_context(code_lines[first]);
        if (acc > INT_MAX - nrows) { acc = INT_MAX; first++; break; }
        acc += nrows;
        first++;
    }
    int screen = 0;
    for (int ln = first; ln < code_line_count && screen < visible; ln++) {
        int textw = line_textw(w, ln);
        int chip = line_chip(w, ln);
        int sidx = chip ? slider_on_line(ln, cur_col) : -1;
        int nrows = line_screen_rows(ln, textw);
        int err = line_err && ln < line_err_cap && line_err[ln];
        int selected_line = sel_active && sel_linewise && selected_byte(ln, 0);
        for (int sub = 0; sub < nrows && screen < visible; sub++) {
            int vr = acc + sub;
            if (vr < view_top) continue;
            int r = screen++;
            int textrows = line_text_rows(ln,textw);
            if (sub >= textrows) {
                blank_inner(r+2,1,5);
                draw_inline_row(ln,sub-textrows,r+2,6,w-8);
                blank_inner(r+2,w-2,1);
                continue;
            }
            char num[16];
            if (sub == 0) snprintf(num, sizeof(num), "%4d ", ln + 1);
            else snprintf(num, sizeof(num), "     ");
            terminal_move(r + 2, 1);
            if (selected_line) terminal_use_role(11);
            else if (err) terminal_use_role(9);
            terminal_write(num);
            if (selected_line || err) terminal_style_reset();
            int b0 = 0, b1 = 0;
            if (editor_wrap) editor_row_span(code_lines[ln], textw, sub, &b0, &b1);
            else editor_scroll_span(code_lines[ln], view_col, textw, &b0, &b1);
            if (b1 < b0) b1 = b0;
            size_t n = (size_t)(b1 - b0);
            char *tmp = malloc(n + 1);
            int used = 5;
            if (tmp) {
                char qsave = hl_quote;
                int bsave = hl_block_comment;
                if (n) memcpy(tmp, code_lines[ln] + b0, n);
                tmp[n] = 0;
                hl_line = ln;
                hl_play = (ln < play_n) ? play_rows[ln] : NULL;
                /* Stopped playback clears existing masks without resizing
                   them. Edits/lesson switches can make the new line longer. */
                hl_play_len = (ln < play_n) ? play_cap[ln] : 0;
                hl_bias = b0;
                hl_eol_comment = 0;
                if (b0 > 0) highlight_scan(code_lines[ln], b0);
                hl_force_comment = hl_eol_comment;
                terminal_move(r + 2, 6);
                used = 5 + draw_hl_line(tmp, textw);
                hl_force_comment = 0;
                hl_eol_comment = 0;
                hl_quote = qsave;
                hl_block_comment = bsave;
                free(tmp);
            }
            int gap = (w - 2) - chip - used;
            if (gap > 0) blank_inner(r + 2, 1 + used, gap);
            if (chip && sub == 0)
                draw_slider_chip(r + 2, 1 + (w - 2) - chip, chip, sidx);
        }
        highlight_context(code_lines[ln]);
        if (acc > INT_MAX - nrows) acc = INT_MAX;
        else acc += nrows;
    }
    for (; screen < visible; screen++) blank_inner(screen + 2, 1, w - 2);
    if (foot == 2 && w > 4) {
        draw_line_roll(h - 2, 1, w - 2);
        draw_mini_scope(h - 1, 1, w - 2);
    }
    if (!pane_focus && input_mode == 0 && cur_line >= 0 && cur_line < code_line_count) {
        int vrow = 0;
        for (int i = 0; i < cur_line; i++) {
            int add = line_screen_rows(i, line_textw(w, i));
            if (vrow > INT_MAX - add) { vrow = INT_MAX; break; }
            vrow += add;
        }
        int textw = line_textw(w, cur_line);
        int r = 0, cell = 0;
        if (editor_wrap)
            editor_place(code_lines[cur_line], cur_col, textw, &r, &cell);
        else
            cell = editor_visual_col(code_lines[cur_line], cur_col) - view_col;
        if (editor_wrap && vrow <= INT_MAX - r) vrow += r;
        int cy = vrow - view_top + 2;
        int cx = cell + 6;
        if (cy >= 2 && cy <= h - 1 - foot && cx >= 6 && cx < w - 2) {
            const char *s = code_lines[cur_line];
            char glyph[8];
            int glen = 1;
            glyph[0] = ' ';
            glyph[1] = 0;
            if (cur_col >= 0 && cur_col < (int)strlen(s)) {
                int len = 0;
                unsigned cp = utf8_decode(s + cur_col, &len);
                if (len > 0 && len < 8 && cp >= 0x20 && cp != 0x7f) {
                    memcpy(glyph, s + cur_col, (size_t)len);
                    glyph[len] = 0;
                    glen = len;
                }
            }
            (void)glen;
            terminal_move(cy, cx);
            terminal_style_reverse();
            terminal_write(glyph);
            terminal_style_reset();
            terminal_move(cy, cx);
            editor_cur_r = cy;
            editor_cur_c = cx;
            editor_cur_on = 1;
        }
    }
}

/* ---- Tab completion ---- */
static void editor_indent(int backwards) {
    if (!lines_ready() || cur_line < 0 || cur_line >= code_line_count) return;
    if (!sel_active && !backwards) {
        int count = 2-editor_visual_col(code_lines[cur_line],cur_col)%2;
        int at = editor_offset(cur_line,cur_col);
        (void)editor_replace(at,at,count == 2 ? "  " : " ");
        return;
    }
    int first = cur_line, last = cur_line;
    if (sel_active) {
        first = sel_line < cur_line ? sel_line : cur_line;
        last = sel_line > cur_line ? sel_line : cur_line;
        int endcol = sel_line > cur_line ? sel_col : cur_col;
        if (!sel_linewise && !sel_inclusive && last > first && endcol == 0) last--;
    }
    size_t size = 1;
    for (int i = first; i <= last; i++) {
        size_t n = strlen(code_lines[i]);
        if (n > SOURCE_MAX-3 || size > SOURCE_MAX-n-3) return;
        size += n+3;
    }
    char *text = malloc(size);
    if (!text) { snprintf(status_msg,sizeof(status_msg),"out of memory"); return; }
    size_t used = 0;
    int removed_current = 0, selected = sel_active, original_line = cur_line, original_col = cur_col;
    for (int i = first; i <= last; i++) {
        const char *line = code_lines[i];
        int skip = 0;
        if (backwards) {
            if (*line == '\t') skip = 1;
            else while (skip < 2 && line[skip] == ' ') skip++;
        } else { text[used++] = ' '; text[used++] = ' '; }
        if (i == original_line) removed_current = skip;
        size_t n = strlen(line+skip);
        memcpy(text+used,line+skip,n); used += n;
        if (i < last) text[used++] = '\n';
    }
    text[used] = 0;
    int a = editor_offset(first,0), b = editor_offset(last,(int)strlen(code_lines[last]));
    if (editor_replace(a,b,text)) {
        if (selected) {
            sel_active = sel_linewise = 1; sel_inclusive = 0; sel_line = first; sel_col = 0;
            cur_line = last; cur_col = (int)strlen(code_lines[last]);
        } else {
            cur_line = original_line; cur_col = original_col-removed_current;
            if (cur_col < 0) cur_col = 0;
        }
    }
    free(text);
}

/* ---- bank search ---- */

#include "tui_completion.h"

static void search_use_bank(const char *dirfrag);
static void search_refresh(void) {
    int cnt = banks_count();
    unsigned revision = sounds_revision()+(unsigned)banks_ready();
    if (!search_dirty && cnt == search_lastcount && revision == search_revision) return;
    search_dirty = 0;
    search_lastcount = cnt;
    search_revision = revision;
    if (search_category == SOUND_IMPORT) {
        search_ndirs = search_ndirs_total = 0;
    } else {
        search_ndirs_total = sounds_browse(search_category,search_path,search_buf,search_rows,256);
        if (search_ndirs_total < 0) search_ndirs_total = 0;
        search_ndirs = search_ndirs_total < 256 ? search_ndirs_total : 256;
        for (int i = 0; i < search_ndirs; i++) {
            snprintf(search_dirs[i],sizeof(search_dirs[i]),"%s",search_rows[i].name);
            search_dircounts[i] = search_rows[i].sound.slots;
        }
    }
    int count=search_ndirs+(search_path[0]?1:0);
    if (search_sel >= count) search_sel = count > 0 ? count-1 : 0;
}

static void search_ascend(void) {
    if (!search_path[0]) return;
    char *s = strrchr(search_path, '/');
    if (s) *s = 0;
    else search_path[0] = 0;
    search_sel = 0;
    search_dirty = 1;
}

/* selected row: -1 none, -2 is '..', else index into search_dirs */
static int search_row_target(void) {
    int nrows = search_ndirs + (search_path[0] ? 1 : 0);
    if (nrows <= 0 || search_sel < 0 || search_sel >= nrows) return -1;
    if (search_path[0] && search_sel == 0) return -2;
    return search_sel - (search_path[0] ? 1 : 0);
}

static void search_descend_or_use(void) {
    if (search_category == SOUND_IMPORT) {
        if (companion_import_folder(search_buf)) {
            search_category = SOUND_USER; search_buf[0] = 0; search_len = 0; search_sel = 0; search_dirty = 1;
        } else snprintf(status_msg,sizeof(status_msg),"type a folder path, or wait for current import");
        return;
    }
    int t = search_row_target();
    if (t == -2) { search_ascend(); return; }
    if (t < 0 || t >= search_ndirs) return;
    if (search_rows[t].kind!=SOUND_ROW_BANK) {
        char snippet[1200];
        if (sounds_browser_snippet(&search_rows[t],snippet,sizeof(snippet))) {
            int a = editor_offset(cur_line,cur_col), b = a;
            if (sel_active) selection_bounds(&a,&b);
            if (!editor_replace(a,b,snippet)) return;
            search_mode = 0;
            snprintf(status_msg,sizeof(status_msg),"sound inserted (F5 evaluates)");
        }
        return;
    }
    snprintf(search_path, sizeof(search_path), "%s", search_dirs[t]);
    search_sel = 0;
    search_buf[0] = 0;
    search_len = 0;
    search_dirty = 1;
    snprintf(status_msg, sizeof(status_msg), "browsing %.200s", search_dirs[t]);
}

/* ---- song file picker (Ctrl+S save, Ctrl+O load) ---- */

static int project_save(const char *path);
static int project_load(const char *path, int quiet);

static int file_has_suffix(const char *name, const char *suf) {
    size_t nl = strlen(name), sl = strlen(suf);
    return nl >= sl && strcmp(name + nl - sl, suf) == 0;
}

static int file_sub_ci(const char *hay, const char *needle) {
    if (!needle[0]) return 1;
    char h[256], n[256];
    size_t hl = strlen(hay), nl = strlen(needle);
    if (hl >= sizeof(h)) hl = sizeof(h) - 1;
    if (nl >= sizeof(n)) nl = sizeof(n) - 1;
    for (size_t i = 0; i < hl; i++) h[i] = (char)tolower((unsigned char)hay[i]);
    h[hl] = 0;
    for (size_t i = 0; i < nl; i++) n[i] = (char)tolower((unsigned char)needle[i]);
    n[nl] = 0;
    return strstr(h, n) != NULL;
}

static int cmp_name(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

/* rescan CWD for *.json songs, filtered by file_buf; keeps selection */
static void file_refresh(void) {
    if (!file_dirty) return;
    file_dirty = 0;
    static char all[512][256];
    int nall = 0;
    DIR *dp = opendir(".");
    if (dp) {
        struct dirent *de;
        while ((de = readdir(dp)) != NULL && nall < 512) {
            const char *nm = de->d_name;
            if (nm[0] == '.') continue;
            if (!file_has_suffix(nm, ".json")) continue;
            if (strlen(nm) >= 256) continue;
            char fp[300];
            snprintf(fp, sizeof(fp), "./%.250s", nm);
            struct stat st;
            if (stat(fp, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            snprintf(all[nall], sizeof(all[nall]), "%s", nm);
            nall++;
        }
        closedir(dp);
    }
    qsort(all, (size_t)nall, 256, cmp_name);
    file_n = 0;
    for (int i = 0; i < nall && file_n < 256; i++) {
        if (!file_sub_ci(all[i], file_buf)) continue;
        snprintf(file_names[file_n], sizeof(file_names[file_n]), "%s", all[i]);
        file_n++;
    }
    if (file_sel >= file_n) file_sel = file_n - 1;
    if (file_sel < 0) file_sel = 0;
}

static void file_open(int mode) {
    search_mode = 0;
    file_mode = mode;
    file_sel = 0;
    if (mode == 1) {
        /* prefill with current song name for quick re-save */
        const char *b = strrchr(project_path, '/');
        const char *nm = b ? b + 1 : project_path;
        snprintf(file_buf, sizeof(file_buf), "%.255s", nm);
        file_len = (int)strlen(file_buf);
    } else {
        file_buf[0] = 0;
        file_len = 0;
    }
    file_dirty = 1;
    snprintf(status_msg, sizeof(status_msg),
        mode == 1 ? "save song: edit name, Enter to write" : "load song: pick file, Enter to load");
}

/* New names stay in CWD; the current filename retains its project directory. */
static int file_pick_path(char *out, size_t outlen) {
    const char *nm = file_buf[0] ? file_buf :
        (file_sel >= 0 && file_sel < file_n ? file_names[file_sel] : "");
    if (!nm[0]) return 0;
    if (strchr(nm, '/') || strstr(nm, "..")) return 0;
    if (strlen(nm) >= 240) return 0;
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", nm);
    if (!file_has_suffix(tmp, ".json")) {
        size_t L = strlen(tmp);
        if (L + 5 >= sizeof(tmp)) return 0;
        snprintf(tmp + L, sizeof(tmp) - L, ".json");
    }
    /* charset guard: plain portable names only */
    for (const char *p = tmp; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
            !(c >= '0' && c <= '9') && !strchr("_.-", c))
            return 0;
    }
    const char *base=strrchr(project_path,'/');
    const char *target=file_mode==1 && !strcmp(tmp,base?base+1:project_path)?project_path:tmp;
    int n=snprintf(out,outlen,"%s",target);
    return n>=0 && (size_t)n<outlen;
}

static void file_confirm(void) {
    char path[1024];
    if (!file_pick_path(path, sizeof(path))) {
        snprintf(status_msg, sizeof(status_msg), "bad name (a-z 0-9 _ . -)");
        return;
    }
    if (file_mode==1) {
        if (project_save(path)) file_mode=0;
    } else if (project_load(path,0)) file_mode=0;
}

static void draw_search_at(int r0, int c0, int h, int w) {
    search_list_visible = 0;
    if (h < 7 || w < 12) return;
    terminal_box(r0, c0, h, w);
    terminal_move(r0, c0+2);
    if (search_path[0]) {
        char title[120]; snprintf(title,sizeof(title)," bank: %.95s ",search_path);
        draw_text(r0,c0+2,title,w-4,0);
    } else if (search_category != SOUND_CATEGORIES) {
        char title[80]; snprintf(title,sizeof(title)," sounds: %s ",sounds_category(search_category));
        draw_text(r0,c0+2,title,w-4,0);
    } else if (search_path[0]) {
        char t[128];
        snprintf(t, sizeof(t), " banks: %.100s/ ", search_path);
        terminal_write(t);
    } else {
        terminal_write(" banks (F6:close) ");
    }
    char q[256];
    snprintf(q, sizeof(q), search_category == SOUND_IMPORT ? "folder> %.200s" : "find> %.200s", search_buf);
    int qu = draw_text(r0+1, c0+1, q, w-2, 0);
    if (qu < w - 2) blank_inner(r0+1, c0+1+qu, w-2-qu);
    /* Keep category controls visible while browsing and wrap in narrow panes. */
    int category_row = r0+2, category_col = 0;
    blank_inner(category_row,c0+1,w-2);
    for (int i = -1; i <= SOUND_CATEGORIES; i++) {
        const char *name = i < 0 ? "all" : i == SOUND_CATEGORIES ? "local-banks"
            : i == SOUND_USER ? "user sounds" : sounds_category(i);
        char label[64];
        snprintf(label,sizeof(label),"%s%s%s",i == search_category ? "[" : " ",name,
                 i == search_category ? "]" : " ");
        int len = (int)strlen(label);
        if (category_col && category_col+len > w-2) {
            if (category_row+1 >= r0+h-3) break;
            category_row++; category_col = 0;
            blank_inner(category_row,c0+1,w-2);
        }
        if (i == search_category) terminal_style_reverse();
        draw_text(category_row,c0+1+category_col,label,w-2-category_col,0);
        if (i == search_category) terminal_style_reset();
        category_col += len;
    }
    int info_row = category_row+1;
    int list_row = info_row+1;
    int visible = r0+h-1-list_row;
    char info[256];
    if (search_category == SOUND_IMPORT) snprintf(info,sizeof(info),"folder path or github:user/repo bank; WAV/MP3/FLAC, 8 MiB/file; strudel.json banks ok");
    else if (search_category != SOUND_CATEGORIES) snprintf(info,sizeof(info),"%d sounds; Left/Right: category",search_ndirs_total);
    else if (banks_failed()) snprintf(info, sizeof(info), "no banks dir (TOMPOUCE_BANKS?)");
    else if (!banks_ready()) snprintf(info, sizeof(info), "indexing... %d files", banks_count());
    else if (search_path[0]) snprintf(info, sizeof(info), "%d entr%s",
        search_ndirs_total, search_ndirs_total == 1 ? "y" : "ies");
    else if (search_ndirs_total <= 0) snprintf(info, sizeof(info), "%d files indexed, no match", banks_count());
    else snprintf(info, sizeof(info), "%d bank%s",
        search_ndirs_total, search_ndirs_total == 1 ? "" : "s");
    int iu = draw_text(info_row, c0+1, info, w-2, 0);
    if (iu < w - 2) blank_inner(info_row, c0+1+iu, w-2-iu);
    int top = search_sel - visible + 1;
    if (top < 0) top = 0;
    int nrows = search_ndirs + (search_path[0] ? 1 : 0);
    search_list_row = list_row; search_list_top = top;
    search_list_visible = visible; search_list_count = nrows;
    for (int r = 0; r < visible; r++) {
        int idx = top + r;
        if (idx >= nrows) { blank_inner(list_row+r, c0+1, w-2); continue; }
        char b[256];
        if (search_path[0] && idx == 0) {
            snprintf(b, sizeof(b), "%s", "..");
        } else {
            int di = idx - (search_path[0] ? 1 : 0);
            snprintf(b,sizeof(b),"> %.190s%s (%d)%s",search_dirs[di],
                search_rows[di].kind==SOUND_ROW_BANK?"/":"",search_dircounts[di],
                search_rows[di].kind==SOUND_ROW_FILE?" [file]":"");
        }
        terminal_move(list_row+r, c0+1);
        if (idx == search_sel) terminal_style_reverse();
        int used = draw_text(list_row+r, c0+1, b, w-2, 0);
        if (idx == search_sel) terminal_style_reset();
        if (used < w - 2) blank_inner(list_row+r, c0+1+used, w-2-used);
    }
    const char *foot = search_category == SOUND_IMPORT ? "Enter:import  Left/Right:category" : search_ndirs_total > search_ndirs
        ? "...more (refine query)"
        : "Enter:open/insert Tab:insert bank Bs:up";
    int fu = draw_text(r0+h-1, c0+1, foot, w-2, 0);
    if (fu < w - 2) blank_inner(r0+h-1, c0+1+fu, w-2-fu);
}

static void draw_file_at(int r0, int c0, int h, int w) {
    if (h < 4 || w < 12) return;
    terminal_box(r0, c0, h, w);
    terminal_move(r0, c0+2);
    terminal_write(file_mode == 1 ? " save song (Esc:cancel) " : " load song (Esc:cancel) ");
    char q[256];
    snprintf(q, sizeof(q), "file> %.200s", file_buf);
    int qu = draw_text(r0+1, c0+1, q, w-2, 0);
    if (qu < w - 2) blank_inner(r0+1, c0+1+qu, w-2-qu);
    char info[256];
    snprintf(info, sizeof(info), "%d song%s in this dir", file_n, file_n == 1 ? "" : "s");
    int iu = draw_text(r0+2, c0+1, info, w-2, 0);
    if (iu < w - 2) blank_inner(r0+2, c0+1+iu, w-2-iu);
    int visible = h - 4;
    int top = file_sel - visible + 1;
    if (top < 0) top = 0;
    for (int r = 0; r < visible; r++) {
        int idx = top + r;
        if (idx >= file_n) { blank_inner(r0+3+r, c0+1, w-2); continue; }
        char b[256];
        const char *mark = (file_buf[0] && strcmp(file_names[idx], file_buf) == 0) ? "*" : ">";
        snprintf(b, sizeof(b), "%s %.200s", mark, file_names[idx]);
        if (idx == file_sel) terminal_style_reverse();
        int used = draw_text(r0+3+r, c0+1, b, w-2, 0);
        if (idx == file_sel) terminal_style_reset();
        if (used < w - 2) blank_inner(r0+3+r, c0+1+used, w-2-used);
    }
    const char *ffoot = file_mode == 1
        ? "Enter:save Up/Down:pick existing"
        : "Enter:load Up/Down:sel";
    int fu2 = draw_text(r0+h-1, c0+1, ffoot, w-2, 0);
    if (fu2 < w - 2) blank_inner(r0+h-1, c0+1+fu2, w-2-fu2);
}

/* Insert a manifest bank at the source selection without evaluating it. */
static void search_use_bank(const char *dirfrag) {
    SoundBrowserRow rows[256]; char pattern[512]={0}; size_t used=0;
    int count=sounds_browse(SOUND_MACHINES,dirfrag,"",rows,256);
    for (int i=0;i<count && i<256;i++) {
        const char *part=strrchr(rows[i].sound.name,'_');
        if (!part || !part[1]) continue;
        size_t len=strlen(part+1);
        if (used+len+2>=sizeof(pattern)) break;
        if (used) pattern[used++]=' ';
        memcpy(pattern+used,part+1,len+1); used+=len;
    }
    if (!used) return;
    cJSON *name=cJSON_CreateString(dirfrag);
    char *quoted=name?cJSON_PrintUnformatted(name):NULL; cJSON_Delete(name);
    if (!quoted) return;
    cJSON *value=cJSON_CreateString(pattern);
    char *quoted_pattern=value?cJSON_PrintUnformatted(value):NULL; cJSON_Delete(value);
    if (!quoted_pattern) { free(quoted); return; }
    char snippet[1200]; snprintf(snippet,sizeof(snippet),"s(%s).bank(%s)",quoted_pattern,quoted);
    free(quoted_pattern); free(quoted);
    int a=editor_offset(cur_line,cur_col), b=a;
    if (sel_active) selection_bounds(&a,&b);
    if (editor_replace(a,b,snippet)) snprintf(status_msg,sizeof(status_msg),"bank inserted (F5 evaluates)");
}

/* ---- right-pane geometry: main area rows 1..rows-3, right side at col mid ---- */
static void right_geom(int *mid_out, int *h_out, int *w_out, int *state_h_out, int *track_h_out) {
    rows = terminal_rows; cols = terminal_cols;
    int mid = (int)(cols * split_ratio);
    if (mid < 24) mid = 24;
    if (mid > cols - 20) mid = cols - 20;
    if (mid < 0) mid = 0;
    if (!show_console && !show_docs && !show_state && !show_tracks && !search_mode && !file_mode)
        mid = cols;
    int h = rows - 3 - viz_h;
    if (h < 0) h = 0;
    int w = cols - mid;
    if (w < 0) w = 0;
    int sh = 0;
    if (show_state && h >= 14 && w >= 30) sh = h>=30?18:(h>=22?14:10);
    int th = 0;
    if (show_tracks && h - sh >= 16 && w >= 24) {
        int need = scheduler_song_count() + 2;
        if (need < 4) need = 4;
        if (need > 12) need = 12;
        if (h - sh - need >= 5) th = need;
    }
    if (!show_console && !show_docs && !search_mode && !file_mode) {
        if (sh) sh=h-th;
        else if (th) th=h;
    }
    *mid_out = mid; *h_out = h; *w_out = w; *state_h_out = sh; *track_h_out = th;
}

static void toggle_console(void) {
    show_console=!show_console;
    if (!show_console) pane_focus=0;
    graphics_redraw=1;
    snprintf(status_msg,sizeof(status_msg),show_console?"console shown":"console hidden (Ctrl+L restores)");
}

static void draw_console_at(int r0, int c0, int h, int w) {
    if (h < 3 || w < 4) return;
    terminal_box(r0, c0, h, w);
    (void)draw_text(r0,c0+2," console (Ctrl+L:hide) ",w-4,0);
    int visible = h - 2;
    int cnt = console_count_get();
    int start = (cnt <= visible) ? 0 : (cnt - visible);
    if (console_scroll > start) console_scroll = start;
    start -= console_scroll;
    for (int r = 0; r < visible; r++) {
        int idx = start + r;
        if (idx >= cnt) { blank_inner(r0+1+r, c0+1, w-2); continue; }
        const char *ln = console_line(idx);
        int used = draw_text(r0+1+r, c0+1, ln ? ln : "", w-2, 0);
        if (used < w - 2) blank_inner(r0+1+r, c0+1+used, w-2-used);
    }
}

/* ---- cheatsheet: logical lines with style tags ----
   style: 0 normal, 1 header, 2 code, 3 bullet */
typedef struct { int style; const char *text; } DocLine;
static const DocLine cheat[] = {
    {1, "Tompouce keybindings"},
    {0, "F1 closes this reference. PgUp/PgDn scroll it."},
    {0, ""},
    {1, "Playback and panels"},
    {3, "F5                Evaluate the editor buffer"},
    {3, "Ctrl+P            Play / stop"},
    {3, "Ctrl+L            Hide / restore console"},
    {3, "F1                Toggle this keybinding reference"},
    {3, "F2                Open AI / application command prompt"},
    {3, "F3                Toggle vi editing mode"},
    {3, "F4                Toggle engine / editor diagnostics"},
    {3, "F6                Open / close the sound browser"},
    {3, "F9                Toggle the song track list"},
    {3, "F10 / F11         Narrow / widen the editor pane"},
    {3, "F12               Toggle piano roll + scope pane"},
    {3, "Ctrl+Left/Right   Adjust live slider at cursor"},
    {0, ""},
    {1, "Songs and files"},
    {3, "F7                Toggle sequential song playback"},
    {3, "F8                Hold / release the current track"},
    {3, "Ctrl+T            Copy buffer to a new song track"},
    {3, "Ctrl+N / Ctrl+B   Load next / previous track"},
    {3, "Ctrl+S / Ctrl+O   Save / open whole-song JSON"},
    {3, "Ctrl+Q            Quit and save recovery session"},
    {0, ""},
    {1, "Editor and selection"},
    {3, "Arrows            Move cursor (Up/Down follow wraps)"},
    {3, "Home / End        Start / end of logical line"},
    {3, "PgUp / PgDn       Page editor; scroll help if open"},
    {3, "Enter             Split line / replace selection"},
    {3, "Backspace / Del   Delete before / at cursor"},
    {3, "Shift+arrows      Extend selection"},
    {3, "Shift+Home/End    Select to line start / end"},
    {3, "Ctrl+A            Select all"},
    {3, "Ctrl+C / Ctrl+X   Copy / cut selection"},
    {3, "Ctrl+V            Paste internal clipboard"},
    {3, "Ctrl+D            Duplicate current line"},
    {3, "Ctrl+Z / Ctrl+Y   Undo / redo"},
    {3, "Ctrl+R            Evaluate (vi normal: redo)"},
    {3, "Esc               Clear selection / return to editor"},
    {0, ""},
    {1, "Completion and indentation"},
    {3, "Tab               Complete word in insert mode"},
    {3, "Ctrl+Space        Request Strudel / sound completion"},
    {3, "Tab / Shift+Tab   Cycle completion forward / backward"},
    {3, "Tab               Indent leading space or selection"},
    {3, "Shift+Tab         Unindent when not cycling matches"},
    {0, ""},
    {1, "Vi normal mode (F3 enables)"},
    {3, "i / a             Insert before / after cursor"},
    {3, "I / A             Insert at first nonblank / line end"},
    {3, "o / O             Open line below / above"},
    {3, "Esc               Leave insert / visual mode"},
    {3, "h j k l           Left / down / up / right"},
    {3, "j/k, gj/gk        Move through wrapped editor rows"},
    {3, "w b e / W B E     Word / WORD motions"},
    {3, "0 ^ $             Line start / first nonblank / end"},
    {3, "gg / G            First / last line; 12G goes to 12"},
    {3, "{ / }             Previous / next paragraph"},
    {3, "%                 Match literal bracket"},
    {3, "f/F/t/T + char    Find / move just before character"},
    {3, "; / ,             Repeat / reverse character find"},
    {3, "/text / ?text     Search forward / backward; Enter"},
    {3, "n / N             Repeat / reverse literal search"},
    {3, "* / #             Search cursor word forward / back"},
    {3, "v / V             Select text / whole lines"},
    {3, "d / c / y         Delete / change / copy with motion"},
    {3, "dd / cc / yy      Delete / change / copy lines"},
    {3, "D / C             Delete / change to line end"},
    {3, "Y                 Copy whole line"},
    {3, "x / X             Delete at / before cursor"},
    {3, "s / r + char      Substitute / replace character"},
    {3, "p / P             Paste after / before cursor"},
    {3, "u / Ctrl+R        Undo / redo"},
    {3, "Counts            5w, 3dd, 2d3w; also counted paste"},
    {3, "Text objects      diw daw ci\" da( di[ ci{"},
    {0, ""},
    {1, "Vi command line (: in normal mode)"},
    {3, "Tab / Shift+Tab   Complete / cycle commands and paths"},
    {3, "Enter / Esc       Run / cancel command"},
    {3, "Ctrl+U            Clear prompt"},
    {3, ":w [file.json]    Save whole song"},
    {3, ":e [file.json]    Open song"},
    {3, ":q / :wq / :x     Quit / save+quit / save if changed"},
    {3, "! suffix          Override unsaved / overwrite guard"},
    {3, ":undo / :redo     Undo / redo"},
    {3, ":42 / :$          Go to logical line / last line"},
    {3, ":set [no]wrap     Enable / disable wrapping"},
    {3, ":set [no]mouse    Enable / disable mouse input"},
    {3, ":help             Open this keybinding reference"},
    {3, ":tutor            Open Strudeltutor lessons"},
    {3, ":evaluate         Evaluate editor buffer"},
    {0, ""},
    {1, "Sound browser (F6)"},
    {3, "Type / Backspace  Filter / erase query"},
    {3, "Left / Right      Switch sound categories"},
    {3, "Up / Down         Select sound or bank"},
    {3, "PgUp / PgDn       Move selection by ten rows"},
    {3, "Enter             Open bank / insert selected sound"},
    {3, "Tab               Insert bank pattern / use selection"},
    {3, "Empty Backspace   Leave bank / return to All category"},
    {3, "Esc / F6          Close browser without inserting"},
    {0, ""},
    {1, "File picker (Ctrl+S / Ctrl+O)"},
    {3, "Type / Backspace  Enter / erase filename"},
    {3, "Up / Down         Select existing song"},
    {3, "PgUp / PgDn       Move selection by ten rows"},
    {3, "Enter / Esc       Save or open / cancel picker"},
    {0, ""},
    {1, "AI / application prompt (F2)"},
    {3, "Enter / Esc       Submit / cancel prompt"},
    {3, "Ctrl+U            Clear prompt"},
    {3, "Tab / Shift+Tab   Complete / cycle /commands"},
    {3, "/tutor            Open Strudeltutor lessons"},
    {3, "/companion        Open browser companion"},
    {3, "/companion stop   Close companion"},
    {0, ""},
    {1, "Mouse and pane focus"},
    {3, "Left-click        Place cursor / focus pane"},
    {3, "Left-drag         Select text / adjust slider bar"},
    {3, "Track row click   Load that song track"},
    {3, "Wheel             Scroll editor / console / help"},
    {3, "PgUp / PgDn       Scroll focused console or open help"},
    {3, "Esc               Return focus to editor"},
    {3, "Shift+mouse       Terminal's native text selection"},
};
static const int cheat_n = (int)(sizeof(cheat) / sizeof(cheat[0]));

/* write one wrapped visual line with style; handles `code` spans via reverse */
/* docs content is static ASCII: byte count == cell count. Returns cells. */
static int write_styled(const char *text, int style) {
    int cells = 0;
    if (style == 1) terminal_style_reverse();
    const char *p = text;
    int in_code = 0;
    char seg[256];
    int si = 0;
    while (*p) {
        if (*p == '`') {
            seg[si] = 0;
            if (si) {
                if (style == 2 || in_code) { terminal_style_reverse(); terminal_write(seg); terminal_style_reset(); if (style == 1) terminal_style_reverse(); }
                else terminal_write(seg);
                cells += si;
            }
            si = 0;
            in_code = !in_code;
            p++;
            continue;
        }
        if (si < 250) seg[si++] = *p;
        p++;
    }
    seg[si] = 0;
    if (si) {
        if (style == 2 || in_code) { terminal_style_reverse(); terminal_write(seg); terminal_style_reset(); if (style == 1) terminal_style_reverse(); }
        else terminal_write(seg);
        cells += si;
    }
    if (style == 1 || style == 2) terminal_style_reset();
    return cells;
}

/* word-wrap logical doc lines into visual rows for width w */
static void draw_docs_at(int r0, int c0, int h, int w) {
    if (h < 3 || w < 8) return;
    terminal_box(r0, c0, h, w);
    (void)draw_text(r0,c0+2," keybindings (F1:back PgUp/PgDn:scroll) ",w-4,0);
    int visible = h - 2;
    /* Blank logical lines and the area beyond the text wrap limit must also
       overwrite the previous pane, including full-width editor sliders. */
    for (int row=0;row<visible;row++) blank_inner(r0+1+row,c0+1,w-2);
    int vw = w - 2;
    if (vw > 250) vw = 250;
    /* count total visual rows first to clamp scroll */
    int total = 0;
    for (int i = 0; i < cheat_n; i++) {
        const char *t = cheat[i].text;
        int len = (int)strlen(t);
        total += (len == 0) ? 1 : (len + vw - 1) / vw;
    }
    if (docs_scroll > total - visible) docs_scroll = total - visible;
    if (docs_scroll < 0) docs_scroll = 0;
    int skip = docs_scroll;
    int row = 0;
    char vb[256];
    for (int i = 0; i < cheat_n && row < visible; i++) {
        const char *t = cheat[i].text;
        int style = cheat[i].style;
        int len = (int)strlen(t);
        if (len == 0) {
            if (skip > 0) skip--;
            else { terminal_move(r0+1+row, c0+1); row++; }
            continue;
        }
        int off = 0;
        while (off < len && row < visible) {
            int chunk = len - off;
            if (chunk > vw) {
                /* break at space if possible */
                int br = vw;
                while (br > vw - 20 && br > 0 && t[off+br] != ' ') br--;
                chunk = (br > 0 && br < vw) ? br + 1 : vw;
            }
            if (skip > 0) { skip--; }
            else {
                int cl = chunk;
                if (cl > 255) cl = 255;
                memcpy(vb, t + off, cl);
                vb[cl] = 0;
                terminal_move(r0+1+row, c0+1);
                int used = write_styled(vb, style);
                int edge = c0 + 1 + vw;
                if (c0 + 1 + used < edge) blank_inner(r0+1+row, c0+1+used, edge - (c0+1+used));
                row++;
            }
            off += chunk;
        }
    }
    for (; row < visible; row++)
        blank_inner(r0+1+row, c0+1, w-2);
    /* scroll hint */
    if (total > visible) {
        char hint[64];
        snprintf(hint, sizeof(hint), "%d/%d", docs_scroll + 1, total - visible + 1);
        int hl = (int)strlen(hint);
        if (hl < w - 2) { terminal_move(r0, c0 + w - 1 - hl); terminal_write(hint); }
    }
}

static void state_line(int *row, int maxrow, int col, int width, const char *format, ...) {
    if (*row>=maxrow) return;
    char text[256]; va_list args;
    va_start(args,format); vsnprintf(text,sizeof(text),format,args); va_end(args);
    int used=draw_text(*row,col,text,width,0);
    if (used<width) blank_inner(*row,col+used,width-used);
    (*row)++;
}

static void draw_state_at(int r0, int c0, int h, int w) {
    if (h < 5 || w < 20) return;
    terminal_box(r0, c0, h, w);
    terminal_move(r0, c0+2);
    terminal_write(" state (F4:hide) ");
    char b[256];
    int row = r0 + 1;
    int maxrow = r0 + h - 1;
    double now = audio_time_now();
    int qd = event_queue_depth();
    int av = 0; int by[10] = {0};
    audio_voice_stats(&av, by);
    const char *mode = input_mode==2 ? "COMMAND" : input_mode==1 ? "PROMPT" : (vi_mode ? (vi_normal ? "VI:N" : "VI:I") : "INS");
    state_line(&row,maxrow,c0+1,w-2,"mode=%s wrap=%s mouse=%s selection=%s",mode,
        editor_wrap?"on":"off",mouse_enabled?"on":"off",sel_active?(sel_linewise?"lines":"text"):"none");
    snprintf(b, sizeof(b), "bpm %.0f %s t=%.1fs beat=%.3fs cyc=%.3fs",
        audio_get_bpm(), audio_is_playing() ? "PLAY" : "STOP",
        now, audio_sec_per_beat(), audio_sec_per_cycle());
    if (row < maxrow) {
        int used = draw_text(row, c0+1, b, w-2, 0);
        if (used < w - 2) blank_inner(row, c0+1+used, w-2-used);
        row++;
    }
    snprintf(b, sizeof(b), "q=%d voices=%d/128 dr:%d %d %d %d %d",
        qd, av, by[5], by[6], by[7], by[8], by[9]);
    if (row < maxrow) {
        int used = draw_text(row, c0+1, b, w-2, 0);
        if (used < w - 2) blank_inner(row, c0+1+used, w-2-used);
        row++;
    }
    char mcpst[32];
    ConvolutionSimdStats simd=convolution_simd_stats();
    state_line(&row,maxrow,c0+1,w-2,"SIMD conv=%s mode=%s%s cpu=%s",simd.kernel,
        simd.policy,simd.fallback?" fallback":"",simd.available);
    mcp_status(mcpst, sizeof(mcpst));
    if (slider_count() > 0 && row < maxrow) {
        char sb[256];
        int sp = snprintf(sb, sizeof(sb), "sliders");
        for (int i = 0; i < slider_count() && sp > 0 && sp < (int)sizeof(sb) - 12; i++) {
            double v = 0;
            slider_info(i, &v, NULL, NULL, NULL);
            int wri = snprintf(sb + sp, sizeof(sb) - (size_t)sp, " %d:%.2f", i + 1, v);
            if (wri < 0) break;
            sp += wri;
        }
        int used = draw_text(row, c0+1, sb, w-2, 0);
        if (used < w - 2) blank_inner(row, c0+1+used, w-2-used);
        row++;
    }
    snprintf(b, sizeof(b), "trk=%d ai=%s mcp=%s %dx%d cur=%d/%d",
        track_count, ai_is_busy() ? "busy" : "idle", mcpst,
        cols, rows, cur_line + 1, cur_col);
    if (row < maxrow) {
        int used = draw_text(row, c0+1, b, w-2, 0);
        if (used < w - 2) blank_inner(row, c0+1+used, w-2-used);
        row++;
    }
    if (h>=14) {
        int js=0, compiled=0; unsigned generation=0;
        char *text=editor_flatten();
        pthread_mutex_lock(&tracks_lock);
        compiled=track_count;
        for (int i=0;i<track_count;i++) if (tracks[i].javascript) js++;
        if (track_count) generation=tracks[0].compile_generation;
        const char *evaluated_source=track_count==1 && tracks[0].javascript?
            javascript_source(tracks[0].javascript):current_code;
        int evaluated=text && !strcmp(text,cstr(evaluated_source));
        pthread_mutex_unlock(&tracks_lock);
        state_line(&row,maxrow,c0+1,w-2,"engine=%s js=%d native=%d gen=%u queue-gen=%u",
            "QuickJS/Strudel",js,compiled-js,generation,event_queue_generation());
        state_line(&row,maxrow,c0+1,w-2,"source=%s file=%s bytes=%zu lines=%d",
            !text?"unknown":(evaluated?"evaluated":"pending F5"),
            !text?"unknown":(editor_written && !strcmp(text,editor_written)?"saved":"unsaved"),
            text?strlen(text):0,code_line_count);
        free(text);
        state_line(&row,maxrow,c0+1,w-2,"history undo=%d redo=%d cursor=L%d byte=%d",undo_n,redo_n,cur_line+1,cur_col);
        const char *graphics=graphics_disabled?"braille (forced)":graphics_supported?"Kitty":
            "braille";
        state_line(&row,maxrow,c0+1,w-2,"graphics=%s cell=%dx%d target=%s",graphics,graphics_cell_width,
            graphics_cell_height,graphics_supported && !graphics_disabled?"60fps":"20fps UI");
        state_line(&row,maxrow,c0+1,w-2,"inline=%s lines=%d images=%d/%d footer=%s F12=%s",
            inline_visuals?"on":"off",inline_count,graphics_pending_count,GRAPHICS_IMAGES,
            editor_scope?"enabled":"off",show_viz?"on":"off");
        state_line(&row,maxrow,c0+1,w-2,"audio=%s %s %.0fHz stereo MIDI=%s banks=%s:%d",
            audio_is_stream_active()?"up":"down",audio_backend_name(),SR,midi_ready()?"up":"down",banks_ready()?"ready":"indexing",banks_count());
        state_line(&row,maxrow,c0+1,w-2,"song=%s hold=%s position=%d/%d",scheduler_song_on()?"on":"off",
            scheduler_song_hold()?"on":"off",scheduler_song_pos()+1,scheduler_song_count());
    }
    /* per-track lines */
    pthread_mutex_lock(&tracks_lock);
    int n = track_count;
    if (n > MAX_TRACKS) n = MAX_TRACKS;
    for (int i = 0; i < n && row < maxrow; i++) {
        Track *t = &tracks[i];
        double wait = t->next_cycle - now;
        snprintf(b,sizeof(b),"%d JavaScript %s%s cyc%d %+.1fs",i,
            t->mute?"M":" ",t->solo?"S":" ",t->cycle_index,wait);
        int used = draw_text(row, c0+1, b, w-2, 0);
        if (used < w - 2) blank_inner(row, c0+1+used, w-2-used);
        row++;
    }
    pthread_mutex_unlock(&tracks_lock);
    while (row < maxrow) { blank_inner(row, c0+1, w-2); row++; }
}

static void draw_tracks_at(int r0, int c0, int h, int w) {
    if (h < 4 || w < 12) return;
    terminal_box(r0, c0, h, w);
    terminal_move(r0, c0+2);
    char title[64];
    snprintf(title, sizeof(title), " tracks%s%s (F9:hide) ",
        scheduler_song_on() ? " SONG" : "",
        scheduler_song_hold() ? " HOLD" : "");
    terminal_write(title);
    int n = scheduler_song_count();
    int pos = scheduler_song_pos();
    int visible = h - 2;
    int start = 0;
    if (pos >= 0 && pos >= start + visible) start = pos - visible + 1;
    if (start < 0) start = 0;
    for (int r = 0; r < visible; r++) {
        int idx = start + r;
        if (idx >= n) { blank_inner(r0+1+r, c0+1, w-2); continue; }
        char prev[256];
        scheduler_track_preview(idx, prev, sizeof(prev));
        char b[256];
        snprintf(b, sizeof(b), "%c%-3d %.200s", idx == pos ? '>' : ' ', idx, prev);
        terminal_move(r0+1+r, c0+1);
        if (idx == pos) terminal_style_reverse();
        int used = draw_text(r0+1+r, c0+1, b, w-2, 0);
        if (idx == pos) terminal_style_reset();
        if (used < w - 2) blank_inner(r0+1+r, c0+1+used, w-2-used);
    }
}

#define VIZ_H 12
#define VIZ_SCOPE_ROWS 2

static const char *midi_name(int m, char *out, size_t n) {
    static const char *nm[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    if (m < 0) m = 0;
    if (m > 127) m = 127;
    snprintf(out, n, "%s%d", nm[m % 12], m / 12 - 1);
    return out;
}

/* braille dot bits for (col 0/1, level 0..7 bottom-up) */
static unsigned braille_bit(int col, int lvl) {
    static const unsigned left[4] = {0x01, 0x02, 0x04, 0x40};
    static const unsigned right[4] = {0x08, 0x10, 0x20, 0x80};
    int row = 3 - lvl / 2;
    if (row < 0) row = 0;
    if (row > 3) row = 3;
    return (col == 0 ? left : right)[row];
}

static int scope_level(double v) {
    if (v < -1.0) v = -1.0;
    if (v > 1.0) v = 1.0;
    int l = (int)((v * 0.5 + 0.5) * 7.999);
    if (l < 0) l = 0;
    if (l > 7) l = 7;
    return l;
}

static void draw_viz_at(int r0, int w) {
    int h = VIZ_H;
    if (w < 20) return;
    terminal_box(r0, 0, h, w);
    terminal_move(r0, 2);
    terminal_write(" piano roll + scope (F12:hide) ");
    NoteHist hist[NOTE_HIST_N];
    int nh = note_hist_copy(hist, NOTE_HIST_N);
    double now = audio_time_now();
    const double W = 4.0;
    int roll_rows = h - 2 - VIZ_SCOPE_ROWS;
    int gutter = 4;
    int rw = w - 2 - gutter;
    if (rw < 8) rw = 8;
    if (rw > 500) rw = 500;
    if (roll_rows < 2) roll_rows = 2;
    if (roll_rows > 16) roll_rows = 16;

    /* pitch range over visible window */
    int lo = 127, hi = 0, any = 0;
    for (int i = 0; i < nh; i++) {
        if (hist[i].t0 + hist[i].dur < now - W || hist[i].t0 > now) continue;
        if (hist[i].midi < lo) lo = hist[i].midi;
        if (hist[i].midi > hi) hi = hist[i].midi;
        any = 1;
    }
    static char grid[16][512];
    for (int r = 0; r < roll_rows; r++)
        for (int x = 0; x < rw; x++) grid[r][x] = ' ';
    if (!any) {
        terminal_move(r0 + 2, gutter + 1);
        terminal_write("(no notes yet)");
        for (int r = 0; r < roll_rows; r++)
            if (r != 1) blank_inner(r0 + 1 + r, gutter + 1, rw);
    } else {
        if (hi - lo + 1 < 6) {
            int c = (hi + lo) / 2;
            lo = c - 3; hi = c + 2;
        }
        if (lo < 0) { hi -= lo; lo = 0; }
        if (hi > 127) { lo -= hi - 127; hi = 127; if (lo < 0) lo = 0; }
        int span = hi - lo + 1;
        int step = (span + roll_rows - 1) / roll_rows;
        if (step < 1) step = 1;
        for (int i = 0; i < nh; i++) {
            double s0 = hist[i].t0, s1 = hist[i].t0 + hist[i].dur;
            if (s1 < now - W || s0 > now) continue;
            int x0 = (int)((s0 - (now - W)) / W * rw);
            int x1 = (int)((s1 - (now - W)) / W * rw);
            if (x0 < 0) x0 = 0;
            if (x1 >= rw) x1 = rw - 1;
            int rr = (hi - hist[i].midi) / step;
            if (rr < 0) rr = 0;
            if (rr >= roll_rows) rr = roll_rows - 1;
            for (int x = x0; x <= x1; x++)
                grid[rr][x] = hist[i].is_drum ? 'o' : '#';
        }
        for (int r = 0; r < roll_rows; r++) {
            int ptop = hi - r * step;
            char lab[8] = {' ', ' ', ' ', ' ', 0};
            for (int k = 0; k < step; k++) {
                int pp = ptop - k;
                if (pp < lo) break;
                if (pp % 12 == 0) {
                    char nm[8];
                    midi_name(pp, nm, sizeof(nm));
                    snprintf(lab, sizeof(lab), "%-3s", nm);
                    break;
                }
            }
            terminal_move(r0 + 1 + r, 1);
            terminal_write(lab);
            terminal_write_n(grid[r], (unsigned)rw);
        }
    }

    /* braille scope: min/max per column over recent output */
    {
        int sc_base = r0 + 1 + roll_rows;
        float sbuf[1024];
        int sn = rw * 2;
        if (sn > 1024) sn = 1024;
        if (sn < 2) sn = 2;
        audio_scope_copy(sbuf, sn);
        for (int x = 0; x < rw; x++) {
            int i0 = x * sn / rw, i1 = (x + 1) * sn / rw;
            if (i1 <= i0) i1 = i0 + 1;
            if (i1 > sn) i1 = sn;
            int lo2 = 7, hi2 = 0;
            for (int i = i0; i < i1; i++) {
                int l = scope_level(sbuf[i]);
                if (l < lo2) lo2 = l;
                if (l > hi2) hi2 = l;
            }
            terminal_move(sc_base, gutter + 1 + x);
            if (viz_utf8) {
                unsigned bits = 0;
                for (int lv = lo2; lv <= hi2; lv++) {
                    bits |= braille_bit(0, lv) | braille_bit(1, lv);
                }
                terminal_putc((uint32_t)(0x2800 + bits));
            } else {
                static const char *lvch = " .:-=+*#@";
                char c[2] = { lvch[hi2], 0 };
                terminal_write(c);
            }
        }
        if (!viz_utf8) {
            /* second scope row: center line */
            terminal_move(sc_base + 1, gutter + 1);
            for (int x = 0; x < rw; x++) terminal_write("-");
        } else {
            blank_inner(sc_base + 1, gutter + 1, rw);
        }
    }
}

static void draw_right_pane(void) {
    int mid, h, w, sh, th;
    right_geom(&mid, &h, &w, &sh, &th);
    if (h <= 0 || w <= 0) return;
    int top_h = h - sh - th;
    if (search_mode) draw_search_at(1, mid, top_h, w);
    else if (file_mode) draw_file_at(1, mid, top_h, w);
    else if (show_docs) draw_docs_at(1, mid, top_h, w);
    else if (show_console) draw_console_at(1, mid, top_h, w);
    else for (int row=1;row<1+top_h;row++) blank_inner(row,mid,w);
    if (th > 0) draw_tracks_at(1 + top_h, mid, th, w);
    if (sh > 0) draw_state_at(1 + top_h + th, mid, sh, w);
}

/* load song track i into the editor (manual navigation) */
static void sync_editor_from_track(int i) {
    char *tmp = scheduler_track_dup(i);
    if (!tmp) return;
    replace_code_with(tmp);
    free(tmp);
    note_synced(1);
}

static void draw_console(void) {
    draw_right_pane();
}

static int prompt_view_start(void) {
    int start=0, width=cols-(input_mode==2?1:4)-1;
    if (width<0) width=0;
    while (start<input_len && editor_visual_col(input_buf+start,input_len-start)>width)
        start=vi_next(input_buf,start,input_len);
    return start;
}

static void draw_input(void) {
    char buf[sizeof(input_buf)+8];
    int used;
    if (input_mode) snprintf(buf,sizeof(buf),"%s%s",input_mode==2?":":"AI> ",input_buf+prompt_view_start());
    else snprintf(buf,sizeof(buf),vi_mode?"Type code. :commands F2=AI prompt F1=help F6=sounds.":"Type code. F2=prompt (/companion) F1=help F4=state F6=sounds.");
    used=draw_text(rows-2,0,buf,cols,0);
    if (used<cols) blank_inner(rows-2,used,cols-used);
    snprintf(buf,sizeof(buf),input_mode==2?"Enter runs command. Tab/Shift+Tab complete. Esc cancels.":"Enter submits prompt. Esc cancels.");
    used=draw_text(rows-1,0,buf,cols,0);
    if (used<cols) blank_inner(rows-1,used,cols-used);
}

static int do_evaluate(void) {
    char *buf = editor_flatten();
    if (!buf) {
        snprintf(status_msg, sizeof(status_msg), "out of memory");
        return 0;
    }
    if (!track_load_from_code(buf)) {
        snprintf(status_msg,sizeof(status_msg),"Evaluation failed; last playable pattern retained");
        note_synced(0);
        free(buf);
        return 0;
    }
    int spos = scheduler_song_pos();
    if (spos >= 0) scheduler_track_update(spos, buf); /* commit to active track */
    status_msg[0] = 0;
    snprintf(status_msg, 256, "Evaluated %d tracks", track_count);
    note_synced(1);
    free(buf);
    return 1;
}

/* Explicit Evaluate and starting Play use the editor's current source, as
   StrudelMirror.evaluate/toggle do. Loading a document remains independent. */
static void do_perform(void) {
    if (!do_evaluate()) return;
    scheduler_transport_set(1);
    scheduler_start();
}

static void duplicate_line(void) {
    if (cur_line < 0 || cur_line >= code_line_count) return;
    undo_begin(0);
    if (!line_insert(cur_line + 1, code_lines[cur_line])) {
        undo_abort();
        snprintf(status_msg, sizeof(status_msg), "out of memory");
        return;
    }
    undo_commit();
    cur_line++;
    snprintf(status_msg, sizeof(status_msg), "line duplicated");
}

static int project_save(const char *path) {
    const char *target=path && *path?path:project_path;
    if (!*target) {
        snprintf(status_msg,sizeof(status_msg),"No filename: use /save <path> or :w <path>"); return 0;
    }
    if (strlen(target)>=sizeof(project_path)) { snprintf(status_msg,sizeof(status_msg),"save failed: path too long"); return 0; }
    char *code = editor_flatten();
    if (!code) {
        snprintf(status_msg, sizeof(status_msg), "out of memory");
        return 0;
    }
    int n = songfile_save(target, code, audio_get_bpm(), 0);
    if (n > 0) {
        if (target!=project_path) snprintf(project_path,sizeof(project_path),"%s",target);
        free(editor_written); editor_written=code; code=NULL;
        log_console("saved %s (%d track%s)", project_path, n, n == 1 ? "" : "s");
        snprintf(status_msg, sizeof(status_msg), "saved %.200s (%d track%s)",
            project_path, n, n == 1 ? "" : "s");
    } else {
        snprintf(status_msg, sizeof(status_msg), "save failed");
    }
    free(code);
    return n>0;
}

static int code_has_bpm(const char *code) {
    if (!code) return 0;
    if (strncmp(code, "bpm ", 4) == 0 || strncmp(code, "bpm\t", 4) == 0) return 1;
    return strstr(code, "\nbpm ") != NULL || strstr(code, "\nbpm\t") != NULL;
}

static int project_load(const char *path, int quiet) {
    const char *target=path && *path?path:project_path;
    if (strlen(target)>=sizeof(project_path)) { snprintf(status_msg,sizeof(status_msg),"load failed: path too long"); return 0; }
    char *code = NULL;
    double b = 0;
    int n = songfile_load(target, &code, &b, quiet);
    if (n < 0) { snprintf(status_msg,sizeof(status_msg),"load failed"); return 0; }
    int apply_bpm = (b > 0 && !code_has_bpm(code));
    if (!replace_code_with(code ? code : "")) { free(code); return 0; }
    free(code);
    if (target!=project_path) snprintf(project_path,sizeof(project_path),"%s",target);
    free(editor_written); editor_written=editor_flatten();
    do_evaluate();
    if (apply_bpm) audio_set_bpm(b);
    note_synced(1);
    if (!quiet) {
        snprintf(status_msg, sizeof(status_msg), "loaded %.200s (%d track%s)",
            project_path, n, n == 1 ? "" : "s");
    }
    log_console("loaded %s (%d track%s)", project_path, n, n == 1 ? "" : "s");
    return 1;
}

/* Publish a complete private copy without replacing existing lesson progress. */
static int tutor_copy(const char *master, const char *working) {
    struct stat st;
    if (!lstat(working,&st)) {
        if (S_ISREG(st.st_mode) && st.st_nlink==1) return 1;
        errno=EINVAL; return 0;
    }
    if (errno!=ENOENT) return 0;
    char temporary[sizeof(project_path)+16];
    int n=snprintf(temporary,sizeof(temporary),"%s.XXXXXX",working);
    if (n<0 || (size_t)n>=sizeof(temporary)) { errno=ENAMETOOLONG; return 0; }
    FILE *input=fopen(master,"rb");
    if (!input) return 0;
    int fd=mkstemp(temporary);
    if (fd<0) { int error=errno; fclose(input); errno=error; return 0; }
    FILE *output=fdopen(fd,"wb");
    if (!output) {
        int error=errno; close(fd); unlink(temporary); fclose(input); errno=error; return 0;
    }
    char block[4096]; size_t count; int error=0;
    while ((count=fread(block,1,sizeof(block),input))) {
        if (fwrite(block,1,count,output)!=count) { error=errno?errno:EIO; break; }
    }
    if (ferror(input) && !error) error=errno?errno:EIO;
    if (fclose(input) && !error) error=errno?errno:EIO;
    if (fclose(output) && !error) error=errno?errno:EIO;
    if (!error && link(temporary,working)) error=errno;
    unlink(temporary);
    if (error && error!=EEXIST) { errno=error; return 0; }
    /* Another invocation may have created the working copy first. */
    if (lstat(working,&st)) return 0;
    if (!S_ISREG(st.st_mode) || st.st_nlink!=1) { errno=EINVAL; return 0; }
    return 1;
}

static void tutor_open(int force) {
    if (!force && command_modified()) {
        snprintf(status_msg,sizeof(status_msg),"E37: Unsaved changes; save first or use /tutor! or :tutor!"); return;
    }
    char path[sizeof(project_path)];
    ssize_t length=readlink("/proc/self/exe",path,sizeof(path)-1);
    if (length<0 || (size_t)length>=sizeof(path)-1) {
        snprintf(status_msg,sizeof(status_msg),"Cannot locate Strudeltutor beside the executable"); return;
    }
    path[length]=0;
    char *slash=strrchr(path,'/');
    if (!slash) return;
    size_t base=(size_t)(slash-path);
    int n=snprintf(path+base,sizeof(path)-base,"/examples/strudeltutor.json");
    if (n<0 || (size_t)n>=sizeof(path)-base) {
        snprintf(status_msg,sizeof(status_msg),"Strudeltutor missing; reinstall Tompouce to restore examples"); return;
    }
    char directory[sizeof(project_path)], working[sizeof(project_path)];
    user_dir(directory,sizeof(directory),"XDG_CONFIG_HOME","tompouce",".config/tompouce");
    if (!*directory) { snprintf(status_msg,sizeof(status_msg),"Cannot locate Tompouce config directory"); return; }
    mkdir_p(directory);
    n=snprintf(working,sizeof(working),"%s/strudeltutor.json",directory);
    if (n<0 || (size_t)n>=sizeof(working)) {
        snprintf(status_msg,sizeof(status_msg),"Strudeltutor config path is too long"); return;
    }
    if (!tutor_copy(path,working)) {
        snprintf(status_msg,sizeof(status_msg),"Cannot prepare Strudeltutor working copy: %.160s",strerror(errno)); return;
    }
    if (!project_load(working,0)) return;
    scheduler_song_set(0);
    cur_line=cur_col=view_top=view_col=sel_active=pane_focus=0;
    snprintf(status_msg,sizeof(status_msg),"Strudeltutor working copy: F5 applies; Ctrl+N/B lessons; Ctrl+S saves");
}

void tui_load_initial(const char *code) {
    if (!code || !*code) return;
    undo_suspend++;
    replace_code_with(code);
    undo_suspend--;
    do_evaluate();
    note_synced(1);
}

/* session state path: $XDG_CONFIG_HOME/tompouce/state.json */
static void state_path(char *out, size_t outlen) {
    char base[1024];
    if (!out || outlen == 0) return;
    out[0] = 0;
    user_dir(base, sizeof(base), "XDG_CONFIG_HOME", "tompouce", ".config/tompouce");
    if (!base[0]) return;
    mkdir_p(base);
    int n = snprintf(out, outlen, "%s/state.json", base);
    if (n < 0 || (size_t)n >= outlen) out[0] = 0;
}

void tui_load_state(void) {
    char p[1024], keep[1024];
    state_path(p, sizeof(p));
    if (!p[0]) return;
    snprintf(keep, sizeof(keep), "%s", project_path);
    undo_suspend++;
    project_load(p, 1);
    undo_suspend--;
    snprintf(project_path, sizeof(project_path), "%s", keep);
}

static void state_save(void) {
    char p[1024], keep[1024];
    state_path(p, sizeof(p));
    if (!p[0]) return;
    snprintf(keep, sizeof(keep), "%s", project_path);
    project_save(p);
    snprintf(project_path, sizeof(project_path), "%s", keep);
}

static void do_command_or_ai(const char *text) {
    if (text[0] == '/') {
        char tmp[1024];
        snprintf(tmp, sizeof(tmp), "%s", text+1);
        char *save = NULL;
        char *cmd = strtok_r(tmp, " \t", &save);
        if (cmd && strcmp(cmd,"bpm")==0) {
            char *a = strtok_r(NULL, " \t", &save);
            if (a) {
                char *end; double bpm=strtod(a,&end);
                if (end!=a && !*end && isfinite(bpm) && bpm>=0 && (bpm==0 || isfinite(240/bpm))) {
                    audio_set_bpm(bpm); snprintf(status_msg,256,"BPM %.0f",audio_get_bpm());
                } else snprintf(status_msg,256,"BPM must be a nonnegative finite number");
            }
        } else if (cmd && (!strcmp(cmd,"tutor") || !strcmp(cmd,"tutor!"))) {
            if (strtok_r(NULL," \t",&save)) snprintf(status_msg,sizeof(status_msg),"usage: /tutor[!]");
            else tutor_open(cmd[5]=='!');
        } else if (cmd && strcmp(cmd,"play")==0) {
            if (!audio_is_playing()) do_perform();
        } else if (cmd && strcmp(cmd,"stop")==0) {
            scheduler_transport_set(0); snprintf(status_msg,256,"stopped");
        } else if (cmd && strcmp(cmd,"companion")==0) {
            char *arg = strtok_r(NULL," \t",&save);
            char *extra = strtok_r(NULL," \t",&save);
            if (extra || (arg && strcmp(arg,"stop") && strcmp(arg,"start"))) {
                log_console("usage: /companion [start|stop]");
            } else if (arg && !strcmp(arg,"stop")) {
                companion_stop(); snprintf(status_msg,256,"companion stopping");
            } else {
                int port = mcp_companion_port();
                snprintf(status_msg,256,port && companion_start(port) ? "companion starting (URL in console)" : "companion unavailable (see console)");
            }
        } else if (cmd && strcmp(cmd,"clear")==0) {
            replace_code_with("");
            log_console("Code cleared");
        } else if (cmd && strcmp(cmd,"rescan")==0) {
            smp_invalidate();
            if (banks_rescan()) snprintf(status_msg, 256, "rescanning banks...");
            else snprintf(status_msg, 256, "indexing already running");
        } else if (cmd && strcmp(cmd,"save")==0) {
            char path[sizeof(project_path)];
            while (save && isspace((unsigned char)*save)) save++;
            if (command_path(save?save:"",path,sizeof(path))) project_save(*path?path:NULL);
        } else if (cmd && strcmp(cmd,"load")==0) {
            char path[sizeof(project_path)];
            while (save && isspace((unsigned char)*save)) save++;
            if (command_path(save?save:"",path,sizeof(path))) project_load(*path?path:NULL,0);
        } else if (cmd && strcmp(cmd,"song")==0) {
            char *a = strtok_r(NULL, " \t", &save);
            if (!a) {
                scheduler_song_toggle();
                if (scheduler_song_on()) sync_editor_from_track(scheduler_song_pos());
            } else if (strcmp(a, "on") == 0) {
                scheduler_song_set(1);
                if (scheduler_song_on()) sync_editor_from_track(scheduler_song_pos());
            } else if (strcmp(a, "off") == 0) {
                scheduler_song_set(0);
            } else if (strcmp(a, "hold") == 0) {
                scheduler_song_hold_toggle();
            } else {
                log_console("Unknown song arg: %s (on/off/hold)", a);
            }
            snprintf(status_msg, 256, scheduler_song_on() ? "song on" : "song off");
        } else if (cmd && strcmp(cmd, "inline") == 0) {
            char *a = strtok_r(NULL, " \t", &save);
            if (!a) inline_visuals = !inline_visuals;
            else if (!strcmp(a,"on")) inline_visuals = 1;
            else if (!strcmp(a,"off")) inline_visuals = 0;
            else { log_console("inline on|off"); return; }
            view_top = 0;
            save_editor_config();
            snprintf(status_msg,sizeof(status_msg),"inline visuals %s",inline_visuals ? "on" : "off");
        } else if (cmd && strcmp(cmd, "graphics") == 0) {
            char *a = strtok_r(NULL, " \t", &save);
            if (!a || !strcmp(a,"auto")) graphics_disabled = 0;
            else if (!strcmp(a,"braille")) graphics_disabled = 1;
            else { log_console("graphics auto|braille"); return; }
            save_editor_config();
            graphics_redraw = 1;
            if (!graphics_disabled && !graphics_supported) graphics_probe();
            snprintf(status_msg,sizeof(status_msg),"inline graphics %s",graphics_disabled ? "braille" : "auto");
        } else if (cmd && strcmp(cmd, "theme") == 0) {
            char *a = strtok_r(NULL, " \t", &save);
            int mode=-1;
            if (a) for (int i=0;i<4;i++) if (!strcmp(a,theme_modes[i])) mode=i;
            if (mode<0) { log_console("theme off|omarchy|pywal|auto"); return; }
            theme_mode=mode; theme_poll(1); save_editor_config();
            snprintf(status_msg,sizeof(status_msg),"theme %s",theme_modes[mode]);
        } else if (cmd && strcmp(cmd, "scope") == 0) {
            char *a = strtok_r(NULL, " \t", &save);
            if (!a) editor_scope = !editor_scope;
            else if (!strcmp(a,"on")) editor_scope = 1;
            else if (!strcmp(a,"off")) editor_scope = 0;
            else { log_console("scope on|off"); return; }
            save_editor_config();
            snprintf(status_msg,sizeof(status_msg),"editor scope %s",editor_scope ? "on" : "off");
        } else if (cmd && strcmp(cmd, "wrap") == 0) {
            char *a = strtok_r(NULL, " \t", &save);
            if (!a) editor_wrap = !editor_wrap;
            else if (strcmp(a, "on") == 0) editor_wrap = 1;
            else if (strcmp(a, "off") == 0) editor_wrap = 0;
            else { log_console("wrap on|off"); return; }
            view_top = 0;
            view_col = 0;
            save_editor_config();
            snprintf(status_msg, 256, editor_wrap ? "wrap on" : "wrap off (scroll)");
        } else {
            log_console("Unknown command: %s", text);
        }
        return;
    }
    char *ctx = editor_flatten();
    status_msg[0]=0; snprintf(status_msg,256,"AI thinking...");
    terminal_present();
    ai_request_async(text, ctx);
    free(ctx);
}

#include "tui_commands.h"

void tui_frame(void) {
    theme_poll(0);
    companion_poll();
    /* live pattern check (only when the buffer changed) */
    {
        static unsigned last_hash = 0;
        static int have_hash = 0;
        unsigned h = hash_code();
        if (!have_hash || h != last_hash) {
            have_hash = 1;
            last_hash = h;
            recheck_patterns();
            inline_refresh();
        }
    }
    inline_bind_feeds();
    struct winsize ws;
    int wsh = 0;
    if (ioctl(fileno(stdin), TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        wsh = 1;
        if (terminal_rows != ws.ws_row || terminal_cols != ws.ws_col) {
            resize_flag = 1;
        }
    } else {
        terminal_resync_size();
    }
    if (resize_flag) {
        resize_flag = 0;
        graphics_redraw = 1;
        notcurses_refresh(terminal_nc,NULL,NULL);
        terminal_resync_size();
    }
    if (wsh) {
        terminal_rows = ws.ws_row;
        terminal_cols = ws.ws_col;
    }
    rows = terminal_rows;
    cols = terminal_cols;
    graphics_cell_width = wsh && ws.ws_xpixel && ws.ws_col ? ws.ws_xpixel/ws.ws_col : 8;
    graphics_cell_height = wsh && ws.ws_ypixel && ws.ws_row ? ws.ws_ypixel/ws.ws_row : 16;
    if (graphics_cell_width < 1) graphics_cell_width = 8;
    if (graphics_cell_height < 1) graphics_cell_height = 16;
    graphics_pending_count = 0;
    memset(graphics_pending,0,sizeof(graphics_pending));
    viz_h = (show_viz && rows >= 28) ? VIZ_H : 0;
    /* full clear only on first frame or resize; otherwise every pane
       rewrites all its rows, so Notcurses diffs stay minimal (no flicker) */
    {
        static int first = 1, lr = 0, lc = 0;
        if (first || rows != lr || cols != lc) {
            terminal_clear();
            first = 0;
            lr = rows;
            lc = cols;
        }
    }
    draw_status();
    draw_code();
    draw_console();
    if (viz_h > 0) draw_viz_at(rows - 3 - viz_h, cols);
    draw_input();
    int graphic_layout_changed=graphics_pending_count!=graphics_previous_count ||
        memcmp(graphics_pending,graphics_previous,(size_t)graphics_pending_count*sizeof(InlineGraphic));
    if (graphic_layout_changed) {
        /* External Kitty placements are outside the Notcurses damage model.
           Retire them before repainting text exposed by a layout change. */
        inline_graphics_clear();
        fflush(terminal_output);
    }
    int cursor_r = 0, cursor_c = 0;
    if (search_mode) {
        int mid = (cols * split_ratio < 24) ? 24 : (int)(cols * split_ratio);
        int prefix = search_category == SOUND_IMPORT ? 9 : 7;
        int cell = editor_visual_col(search_buf,search_len);
        if (cell > cols-mid-prefix-2) cell = cols-mid-prefix-2;
        if (cell < 0) cell = 0;
        terminal_move(2, mid + prefix + cell);
        cursor_r = 2; cursor_c = mid + prefix + cell;
    } else if (file_mode) {
        int mid = (cols * split_ratio < 24) ? 24 : (int)(cols * split_ratio);
        terminal_move(2, mid + 7 + file_len);
        cursor_r = 2; cursor_c = mid + 7 + file_len;
    } else if (input_mode == 0 && !pane_focus && editor_cur_on) {
        terminal_move(editor_cur_r, editor_cur_c);
        cursor_r = editor_cur_r;
        cursor_c = editor_cur_c;
    } else if (input_mode) {
        int start=prompt_view_start();
        cursor_r=rows-2; cursor_c=(input_mode==2?1:4)+editor_visual_col(input_buf+start,input_len-start);
        if (cursor_c>=cols) cursor_c=cols-1;
        terminal_move(cursor_r,cursor_c);
    }
    if (cursor_r || cursor_c) terminal_cursor(cursor_r,cursor_c);
    terminal_present();
    if (graphic_layout_changed) notcurses_refresh(terminal_nc,NULL,NULL);
    inline_graphics_flush();
    /* DECSC/DECRC also restore rendition and collide with the library's
       saved state. Return explicitly to its requested visible cursor. */
    if (graphics_pending_count && (cursor_r || cursor_c))
        fprintf(terminal_output,"\033[%d;%dH",cursor_r+1,cursor_c+1);
    fflush(terminal_output);
}

static int report_byte(void) {
    if (quit_requested || !terminal_poll(1)) return -1;
    unsigned int c = terminal_get();
    if (c == NCKEY_ENTER) return '\n';
    if (c == NCKEY_BACKSPACE) return 127;
    if (c > 255 && c != 0xffff) return -1;
    return c == 0xffff ? -1 : (int)c;
}

static void read_mouse(void) {
    char report[64];
    int n = 0, c;
    while (n < (int)sizeof(report) - 1) {
        c = report_byte();
        if (c < 0) return;
        report[n++] = (char)c;
        if (c == 'm' || c == 'M') break;
    }
    report[n] = 0;
    unsigned value[3] = {0};
    const char *p = report;
    for (int i = 0; i < 3; i++) {
        if (!isdigit((unsigned char)*p)) return;
        do {
            if (value[i] > 10000) return;
            value[i] = value[i] * 10 + (unsigned)(*p++ - '0');
        } while (isdigit((unsigned char)*p));
        if (i < 2 && *p++ != ';') return;
    }
    unsigned b = value[0], x = value[1], y = value[2];
    char end = *p;
    if ((end != 'm' && end != 'M') || p[1] || b > 127 || x < 1 || y < 1 ||
        x > (unsigned)cols || y > (unsigned)rows) return;
    if (!mouse_enabled) return;
    mouse_x = (int)x - 1; mouse_y = (int)y - 1;
    int mid, h, w, sh, th;
    right_geom(&mid, &h, &w, &sh, &th);
    (void)w;
    if (end == 'm') { mouse_drag = 0; mouse_slider = -1; return; }
    if (b & 64) {
        int dir = (b & 1) ? 1 : -1;
        if (mouse_x >= mid && show_docs) {
            docs_scroll += dir * 3;
            if (docs_scroll < 0) docs_scroll = 0;
        } else if (mouse_x >= mid && !search_mode && !file_mode) {
            console_scroll -= dir * 3;
            if (console_scroll < 0) console_scroll = 0;
        } else if (mouse_x < mid) {
            editor_move_rows(dir * 3);
        }
        return;
    }
    if ((b & 3) != 0) return;
    if (!(b & 32)) {
        pane_focus = mouse_x >= mid;
        snprintf(status_msg, sizeof(status_msg), "%s", pane_focus ? "right pane focused (Esc: editor)" : "editor focused");
        if (mouse_y == rows - 2) { input_mode = 1; pane_focus = 0; return; }
        input_mode = 0;
        if (pane_focus) {
            int top = 1 + h - sh - th;
            if (th && mouse_y > top && mouse_y < top + th - 1) {
                int start = scheduler_song_pos() - (th - 2) + 1;
                if (start < 0) start = 0;
                int idx = start + mouse_y - top - 1;
                if (idx < scheduler_song_count()) {
                    scheduler_track_goto(idx); sync_editor_from_track(idx);
                }
            } else if (search_mode && mouse_y >= search_list_row &&
                       mouse_y < search_list_row + search_list_visible) {
                int idx = search_list_top + mouse_y - search_list_row;
                if (idx < search_list_count) search_sel = idx;
            } else if (file_mode && mouse_y >= 4) {
                file_sel = mouse_y - 4;
                if (file_sel >= file_n) file_sel = file_n - 1;
            }
            return;
        }
    }
    int visible = h - 2 - editor_footer(h);
    if (pane_focus || mouse_y < 2 || mouse_y >= 2 + visible) return;
    int line = -1, sub = 0;
    int vtarget = view_top + mouse_y - 2;
    if (vtarget < 0) return;
    {
        int acc = 0;
        for (int ln = 0; ln < code_line_count; ln++) {
            int nrows = line_screen_rows(ln, line_textw(mid, ln));
            if (vtarget < acc + nrows) {
                line = ln;
                sub = vtarget - acc;
                break;
            }
            if (acc > INT_MAX - nrows) return;
            acc += nrows;
        }
        if (line < 0) return;
    }
    if (sub >= line_text_rows(line,line_textw(mid,line))) return;
    if (!(b & 32) && sub == 0) {
        int idx = slider_on_line(line, cur_col);
        if (idx >= 0 && mid >= 40 && mouse_x >= mid - 17) mouse_slider = idx;
    }
    if (mouse_slider >= 0) {
        double lo, hi;
        if (slider_info(mouse_slider, NULL, &lo, &hi, NULL)) {
            double t = (mouse_x - (mid - 17)) / 9.0;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            if (slider_set(mouse_slider, lo + (hi - lo) * t)) {
                (void)slider_write_value(mouse_slider);
                double value;
                if (slider_info(mouse_slider, &value, NULL, NULL, NULL))
                    snprintf(status_msg, sizeof(status_msg), "slider %d = %.3g", mouse_slider + 1, value);
            }
        }
        return;
    }
    int cells = mouse_x - 6;
    if (cells < 0) cells = 0;
    int col = editor_wrap
        ? editor_byte_at(code_lines[line], sub, cells, line_textw(mid, line))
        : editor_byte_at_col(code_lines[line], view_col + cells);
    if (!(b & 32)) {
        if (!(b & 4) || !sel_active) {
            cur_line = line; cur_col = col;
            selection_start(0);
        }
        mouse_drag = 1;
    }
    if (mouse_drag) { cur_line = line; cur_col = col; }
}

static int paste_push(char **text, size_t *n, size_t *cap, char ch) {
    if (*n >= SOURCE_MAX) return 0;
    if (*n + 1 >= *cap) {
        if (*cap > SOURCE_MAX / 2) return 0;
        size_t ncap = *cap * 2;
        if (ncap <= *cap) return 0;
        char *p = realloc(*text, ncap);
        if (!p) return 0;
        *text = p;
        *cap = ncap;
    }
    (*text)[(*n)++] = ch;
    return 1;
}

static void read_paste(void) {
    size_t cap = 4096, n = 0;
    char *text = malloc(cap);
    /* A readiness timeout is a gap in a streamed paste, not its end. Keep
       draining even after allocation failure so payload cannot become editor
       commands. EOF and shutdown still cancel the uncommitted edit. A lost
       closing marker must not wedge the editor either: two seconds without
       progress abandons the paste, and Ctrl-Q quits instead of being
       swallowed mid-paste. */
    int overflow = !text, c, matched = 0;
    const char *end = "\033[201~";
    double idle_since = now_seconds();
    int quit_key = 0;
    while (!quit_requested && !terminal_input_closed) {
        c=report_byte();
        if (c<0) {
            if (now_seconds()-idle_since > 2.0) break;
            continue;
        }
        idle_since = now_seconds();
        if (c == 17) { quit_key = 1; break; }
        if (c == (unsigned char)end[matched]) {
            if (++matched == 6) break;
            continue;
        }
        if (matched) {
            for (int i = 0; i < matched; i++)
                if ((unsigned char)end[i]>=32 && !overflow &&
                    !paste_push(&text, &n, &cap, end[i])) overflow = 1;
            matched = 0;
        }
        if (c==(unsigned char)end[0]) { matched=1; continue; }
        if (c == '\r') continue;
        if (c == '\t') c = ' ';
        if (c != '\n' && (c < 32 || c == 127)) continue;
        if (!overflow && !paste_push(&text, &n, &cap, (char)c)) overflow = 1;
    }
    if (quit_key) {
        state_save();
        quit_requested = 1;
        free(text);
        return;
    }
    if (!overflow) text[n] = 0;
    if (!overflow && matched == 6 && search_mode && !strchr(text,'\n') && n <= 240) {
        snprintf(search_buf,sizeof(search_buf),"%s",text); search_len = (int)n;
        search_sel = 0; search_dirty = 1;
    } else if (!overflow && matched == 6 && !input_mode && !search_mode && !file_mode && !pane_focus) {
        int a = editor_offset(cur_line, cur_col), b = a;
        if (sel_active) selection_bounds(&a, &b);
        if (a < 0 || !editor_replace(a, b, text))
            snprintf(status_msg, sizeof(status_msg), "paste failed");
    } else if (overflow)
        snprintf(status_msg, sizeof(status_msg), "paste exceeds available memory");
    else
        snprintf(status_msg, sizeof(status_msg), "paste cancelled");
    free(text);
}

static int read_key(void) {
    unsigned int c = terminal_get();
    if (c == 0xFFFF) return -1;
    if (c == NCKEY_ENTER) return '\r';
    if (c > 255) return (int)c;
    if (c == 27) {
        if (terminal_poll(1) == 0) {
            return 27;
        }
        /* Escape followed quickly by a normal-mode command is two keys,
           not an unknown terminal sequence that should swallow the command. */
        unsigned int next=terminal_get();
        if (next!=0xffff) terminal_unget(next);
        if (next!='[' && next!='O' && next!='_' && next!=']') return 27;
        terminal_unget(27);
        return terminal_get_key();
    }
    terminal_unget((unsigned char)c);
    return terminal_get_key();
}

void tui_run(void) {
    int ch;
    if (!editor_written) editor_written=editor_flatten();
    while (!quit_requested) {
        char *res = ai_poll_result();
        if (res) {
            log_console("AI responded (%d bytes)", (int)strlen(res));
            replace_code_with(res);
            do_evaluate();
            snprintf(status_msg,256,"AI code loaded");
            free(res);
        }
        /* Song auto-step runs in the generator. Follow when the editor
           still matches the last load. Typed edits stay put until F5. */
        {
            static int seen_pos = -2;
            static int held = 0;
            int spos = scheduler_song_pos();
            int follow = scheduler_song_follow_consume();
            if (spos != seen_pos) {
                char *ed = editor_flatten();
                int clean = ed && strcmp(ed, cstr(editor_clean)) == 0;
                seen_pos = spos;
                if (ed && spos >= 0 && (follow || (scheduler_song_on() && clean))) {
                    char *tmp = scheduler_track_dup(spos);
                    if (tmp) {
                        /* Track follow is not an edit. The song list already
                           keeps each snapshot. */
                        undo_suspend++;
                        if (strcmp(ed, tmp) != 0) replace_code_with(tmp);
                        undo_suspend--;
                        note_synced(1);
                        snprintf(status_msg, sizeof(status_msg), "song %d/%d",
                            spos + 1, scheduler_song_count());
                        held = 0;
                        free(tmp);
                    }
                } else {
                    note_synced(0);
                    if (ed && scheduler_song_on() && spos >= 0 && !clean && !held) {
                        snprintf(status_msg, sizeof(status_msg),
                            "edit holds the buffer; F5 commits");
                        held = 1;
                    }
                }
                free(ed);
            }
        }
        if (strcmp(cstr(last_synced_code), cstr(current_code)) != 0) {
            replace_code_with(current_code);
            note_synced(1);
        }
        mcp_poll();
        if (strcmp(cstr(last_synced_code), cstr(current_code)) != 0) {
            replace_code_with(current_code);
            note_synced(1);
        }
        if (search_mode) search_refresh();
        if (file_mode) file_refresh();
        tui_frame();

        if (terminal_poll(0)) ch = read_key();
        else {
            if (terminal_input_closed) { quit_requested=SIGHUP; continue; }
            int idle_ms = 50;
            if (graphics_supported && !graphics_disabled && graphics_previous_count > 0) {
                /* Rendering/transfer time counts toward the frame interval.
                   Waiting a fixed tick afterward halves the effective rate. */
                double remaining = graphics_last_frame+GRAPHICS_FRAME_SECONDS-now_seconds();
                idle_ms = remaining > 0 ? (int)ceil(remaining*1000) : 0;
                if (idle_ms > 17) idle_ms = 17;
            }
            mcp_wait_input(terminal_input_fd(),idle_ms);
            continue;
        }
        if (ch == -1) continue;
        if (ch!=9 && ch!=2102 && ch!=0) completion_reset();
        undo_tick++;
        if (ch == 2100) { vi_reset(); read_mouse(); continue; }
        if (ch == 2101) { vi_reset(); read_paste(); continue; }

        if (search_mode) {
            int nrows = search_ndirs + (search_path[0] ? 1 : 0);
            if (ch == 27 || ch == ((int)NCKEY_F00+(6))) { search_mode = 0; continue; }
            else if (ch == (int)NCKEY_LEFT || ch == (int)NCKEY_RIGHT) {
                int direction = ch == (int)NCKEY_RIGHT ? 1 : -1;
                int previous = search_category;
                search_category = (search_category+1+direction+SOUND_CATEGORIES+2)%(SOUND_CATEGORIES+2)-1;
                if (previous == SOUND_IMPORT || search_category == SOUND_IMPORT) {
                    search_buf[0] = 0; search_len = 0;
                }
                search_path[0] = 0; search_sel = 0; search_dirty = 1; continue;
            }
            else if (ch == (int)NCKEY_ENTER || ch == '\n' || ch == '\r') {
                search_descend_or_use();
                continue;
            } else if (ch == 9) {
                int t = search_row_target();
                if (t >= 0 && t < search_ndirs && search_rows[t].kind==SOUND_ROW_BANK) {
                    search_use_bank(search_dirs[t]);
                    search_mode = 0;
                } else search_descend_or_use();
                continue;
            } else if (ch == (int)NCKEY_UP) { if (search_sel > 0) search_sel--; continue; }
            else if (ch == (int)NCKEY_DOWN) {
                if (search_sel < nrows - 1) search_sel++;
                continue;
            } else if (ch == (int)NCKEY_PGDOWN) { search_sel += 10; if (search_sel > nrows - 1) search_sel = nrows - 1; continue; }
            else if (ch == (int)NCKEY_PGUP) { search_sel -= 10; if (search_sel < 0) search_sel = 0; continue; }
            else if ((ch == (int)NCKEY_BACKSPACE || ch == 127 || ch == 8) && search_len > 0) {
                search_len--;
                while (search_len > 0 && ((unsigned char)search_buf[search_len]&0xc0) == 0x80) search_len--;
                search_buf[search_len] = 0; search_sel = 0; search_dirty = 1; continue;
            } else if ((ch == (int)NCKEY_BACKSPACE || ch == 127 || ch == 8) && search_path[0]) {
                search_ascend();
                continue;
            } else if ((ch == (int)NCKEY_BACKSPACE || ch == 127 || ch == 8) && search_category >= 0) {
                search_category = -1; search_sel = 0; search_dirty = 1; continue;
            } else if (ch >= 32 && ch < 127 && search_len < 240) {
                search_buf[search_len++] = (char)ch; search_buf[search_len] = 0;
                search_sel = 0; search_dirty = 1; continue;
            }
            continue;
        }
        if (file_mode) {
            if (ch == 27) { file_mode = 0; continue; }
            else if (ch == (int)NCKEY_ENTER || ch == '\n' || ch == '\r') {
                file_confirm();
                continue;
            } else if (ch == (int)NCKEY_UP) { if (file_sel > 0) file_sel--; continue; }
            else if (ch == (int)NCKEY_DOWN) {
                if (file_sel < file_n - 1) file_sel++;
                continue;
            } else if (ch == (int)NCKEY_PGDOWN) { if (file_n > 0) { file_sel += 10; if (file_sel > file_n - 1) file_sel = file_n - 1; } continue; }
            else if (ch == (int)NCKEY_PGUP) { if (file_n > 0) { file_sel -= 10; if (file_sel < 0) file_sel = 0; } continue; }
            else if ((ch == (int)NCKEY_BACKSPACE || ch == 127 || ch == 8) && file_len > 0) {
                file_len--; file_buf[file_len] = 0; file_sel = 0; file_dirty = 1; continue;
            } else if (ch >= 32 && ch < 127 && file_len < 240) {
                char c = (char)ch;
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || strchr("_.-", c)) {
                    file_buf[file_len++] = c; file_buf[file_len] = 0;
                    file_sel = 0; file_dirty = 1; continue;
                }
            }
            continue;
        }
        if (input_mode) {
            if (ch == (int)NCKEY_ENTER || ch == '\n' || ch == '\r') {
                input_buf[input_len] = 0;
                if (input_len > 0) {
                    if (input_mode==2) command_execute(input_buf);
                    else if (input_buf[0]==':') command_execute(input_buf+1);
                    else do_command_or_ai(input_buf);
                }
                input_len = 0;
                input_buf[0] = 0;
                input_mode = 0;
            } else if (ch == 27) {
                input_mode = 0;
                input_len = 0;
                input_buf[0] = 0;
            } else if ((ch == (int)NCKEY_BACKSPACE || ch == 127 || ch == 8) && input_len > 0) {
                input_len=vi_prev(input_buf,input_len);
                input_buf[input_len] = 0;
            } else if (ch==9 || ch==2102 || ch==0) {
                (void)command_complete(ch==2102);
            } else if (ch==21) {
                input_len=0; input_buf[0]=0;
            } else if (ch >= 32 && ch <= 255 && ch!=127 &&
                       input_len < (input_mode==2?(int)sizeof(input_buf)-1:AI_PROMPT_MAX)) {
                input_buf[input_len++] = (char)ch;
                input_buf[input_len] = 0;
            }
            continue;
        }
        if (ch == 26) { editor_undo(); continue; }
        if (ch == 25) { editor_redo(); continue; }
        if (ch == 27 && pane_focus) { pane_focus = 0; continue; }
        if (ch == 12) { toggle_console(); continue; } /* Ctrl+L, including vi and console focus. */
        if (pane_focus && vi_mode && vi_normal && ch==':' && vi_handle_normal(ch)) continue;
        if (pane_focus && ch != 17 && ch != 16 && ch != (int)NCKEY_PGDOWN && ch != (int)NCKEY_PGUP &&
            !(ch >= ((int)NCKEY_F00+(1)) && ch <= ((int)NCKEY_F00+(12)))) continue;
        if (ch >= 2110 && ch <= 2115) {
            vi_reset();
            if (!sel_active) selection_start(0);
            if (ch == 2110 && cur_line > 0) cur_line--;
            if (ch == 2111 && cur_line + 1 < code_line_count) cur_line++;
            if (ch == 2112) {
                if (cur_col) cur_col=vi_prev(code_lines[cur_line],cur_col);
                else if (cur_line) { cur_line--; cur_col = (int)strlen(code_lines[cur_line]); }
            }
            if (ch == 2113) {
                if (cur_col < (int)strlen(code_lines[cur_line])) cur_col=vi_next(code_lines[cur_line],cur_col,(int)strlen(code_lines[cur_line]));
                else if (cur_line + 1 < code_line_count) { cur_line++; cur_col = 0; }
            }
            if (ch == 2114) cur_col = 0;
            if (ch == 2115 || cur_col > (int)strlen(code_lines[cur_line]))
                cur_col = (int)strlen(code_lines[cur_line]);
            continue;
        }
        if (ch == 1) { /* Ctrl+A */
            vi_reset();
            cur_line = cur_col = 0; selection_start(0);
            cur_line = code_line_count - 1; cur_col = (int)strlen(code_lines[cur_line]);
            continue;
        }
        if (ch == 3 || ch == 24) { vi_reset(); selection_copy(ch == 24); continue; }
        if (ch == 22) { vi_reset(); editor_paste(1); continue; }
        if (vi_mode && vi_normal && vi_handle_normal(ch)) continue;
        if (vi_mode && vi_normal && (ch == 'v' || ch == 'V')) {
            if (sel_active && sel_linewise == (ch == 'V')) sel_active = 0;
            else { selection_start(ch == 'V'); sel_inclusive = ch == 'v'; }
            continue;
        }
        if (vi_mode && vi_normal && sel_active && (ch == 'y' || ch == 'd' || ch == 'x')) {
            selection_copy(ch != 'y'); vi_pending = 0; continue;
        }
        if (vi_mode && vi_normal && (ch == 'p' || ch == 'P')) {
            editor_paste(ch == 'P'); vi_pending = 0; continue;
        }
        if (sel_active && (!vi_mode || !vi_normal) &&
            (ch == (int)NCKEY_BACKSPACE || ch == 127 || ch == 8 || ch == (int)NCKEY_DEL ||
             ch == '\r' || ch == '\n' || (ch >= 32 && ch <= 255 && ch != 127))) {
            int a, b;
            selection_bounds(&a, &b);
            char text[2] = {0, 0};
            if (ch == '\r' || ch == '\n') text[0] = '\n';
            else if (ch >= 32 && ch <= 255 && ch != 127) text[0] = (char)ch;
            (void)editor_replace(a, b, text);
            continue;
        }
        if (ch == 27 || (!(vi_mode && vi_normal) &&
            (ch == (int)NCKEY_UP || ch == (int)NCKEY_DOWN || ch == (int)NCKEY_LEFT ||
             ch == (int)NCKEY_RIGHT || ch == (int)NCKEY_HOME || ch == (int)NCKEY_END))) sel_active = 0;
        if (ch == 17) {
            state_save(); /* persist session for next launch */
            break;
        } else if (ch == 16) {
            if (audio_is_playing()) { scheduler_transport_set(0); } else { do_perform(); }
            continue;
        } else if (ch == 2001 || ch == 2002) {
            int i = slider_at_cursor();
            if (i<0 && slider_count()==1 && (!slider_document_generation || slider_sites_count==1) &&
                !strstr(code_lines[cur_line],"slider(")) i=0;
            if (slider_count() <= 0) {
                snprintf(status_msg, sizeof(status_msg), "no slider in this buffer");
            } else if (i<0) {
                snprintf(status_msg,sizeof(status_msg),"no evaluated slider at cursor (F5 activates new controls)");
            } else if (!slider_nudge(i, ch == 2002 ? 1 : -1)) {
                double v = 0;
                slider_info(i, &v, NULL, NULL, NULL);
                snprintf(status_msg, sizeof(status_msg), "slider %d stays %.3g", i + 1, v);
            } else {
                double v = 0;
                (void)slider_write_value(i);
                slider_info(i, &v, NULL, NULL, NULL);
                snprintf(status_msg, sizeof(status_msg), "slider %d = %.3g", i + 1, v);
            }
            continue;
        } else if (ch == ((int)NCKEY_F00+(5)) || ch == 18) {
            do_perform();
            continue;
        } else if (ch == ((int)NCKEY_F00+(2))) {
            input_mode = 1;
            input_len = 0;
            input_buf[0] = 0;
            continue;
        } else if (ch == ((int)NCKEY_F00+(1))) {
            show_docs = !show_docs;
            docs_scroll = 0;
            snprintf(status_msg, 256, show_docs ? "cheatsheet on" : "cheatsheet off");
            continue;
        } else if (ch == ((int)NCKEY_F00+(4))) {
            show_state = !show_state;
            snprintf(status_msg, 256, show_state ? "state on" : "state off");
            continue;
        } else if (ch == ((int)NCKEY_F00+(6))) {
            if (!search_mode) {
                banks_ensure_default();
                search_mode = 1;
                file_mode = 0;
                search_category = -1; search_path[0] = 0; search_buf[0] = 0; search_len = 0;
                search_sel = 0;
                search_dirty = 1;
                snprintf(status_msg, 256, "bank search");
            } else search_mode = 0;
            continue;
        } else if (ch == ((int)NCKEY_F00+(7))) {
            if (scheduler_song_on()) {
                scheduler_song_set(0);
                snprintf(status_msg, 256, "song off");
            } else {
                scheduler_song_set(1);
                if (scheduler_song_on()) {
                    sync_editor_from_track(scheduler_song_pos());
                    snprintf(status_msg, 256, "song on: track 0");
                } else {
                    snprintf(status_msg, 256, "no tracks (Ctrl+T copies current)");
                }
            }
            continue;
        } else if (ch == ((int)NCKEY_F00+(8))) {
            scheduler_song_hold_toggle();
            snprintf(status_msg, 256, scheduler_song_hold() ? "hold on" : "hold off");
            continue;
        } else if (ch == ((int)NCKEY_F00+(9))) {
            show_tracks = !show_tracks;
            snprintf(status_msg, 256, show_tracks ? "tracks on" : "tracks off");
            continue;
        } else if (ch == ((int)NCKEY_F00+(10))) {
            split_ratio -= 0.05;
            if (split_ratio < 0.3) split_ratio = 0.3;
            snprintf(status_msg, 256, "split %.0f%%", split_ratio * 100);
            continue;
        } else if (ch == ((int)NCKEY_F00+(11))) {
            split_ratio += 0.05;
            if (split_ratio > 0.85) split_ratio = 0.85;
            snprintf(status_msg, 256, "split %.0f%%", split_ratio * 100);
            continue;
        } else if (ch == ((int)NCKEY_F00+(12))) {
            show_viz = !show_viz;
            snprintf(status_msg, 256, show_viz ? "viz on" : "viz off");
            continue;
        } else if (ch == 20) {
            char *buf = editor_flatten();
            if (!buf) {
                snprintf(status_msg, 256, "out of memory");
            } else {
                int idx = scheduler_track_copy(buf);
                free(buf);
                if (idx >= 0) {
                    note_synced(1);
                    snprintf(status_msg, 256, "track %d/%d", idx + 1, scheduler_song_count());
                }
            }
            continue;
        } else if (ch == 14) {
            if (scheduler_song_count() <= 0) {
                snprintf(status_msg, 256, "no tracks (Ctrl+T copies current)");
            } else {
                scheduler_track_next();
                sync_editor_from_track(scheduler_song_pos());
            }
            continue;
        } else if (ch == 2) {
            if (scheduler_song_count() <= 0) {
                snprintf(status_msg, 256, "no tracks (Ctrl+T copies current)");
            } else {
                scheduler_track_prev();
                sync_editor_from_track(scheduler_song_pos());
            }
            continue;
        } else if (ch == 4) {
            duplicate_line();
            continue;
        } else if (ch == 19) {
            file_open(1); /* Ctrl+S: save picker (whole song) */
            continue;
        } else if (ch == 15) {
            file_open(2); /* Ctrl+O: load picker */
            continue;
        } else if (ch == 9) {
            if (sel_active || (vi_mode && vi_normal) || !editor_complete(0,0)) editor_indent(0);
            continue;
        } else if (ch == 2102) {
            if (!completion_current(1)) editor_indent(1); else (void)editor_complete(1,0);
            continue;
        } else if (ch == 0) {
            (void)editor_complete(0,1);
            continue;
        } else if (ch == ((int)NCKEY_F00+(3))) {
            editor_toggle_mode();
            save_editor_config();
            snprintf(status_msg, 256, vi_mode ? (vi_normal ? "vi normal" : "vi insert") : "insert mode");
            continue;
        } else if (ch == 27) {
            if (vi_mode && !vi_normal && cur_col>0) cur_col=vi_prev(code_lines[cur_line],cur_col);
            vi_normal = vi_mode;
            vi_pending = 0;
            vi_reset();
            snprintf(status_msg, 256, "%s", vi_mode ? "vi normal" : "insert mode");
            continue;
        } else if (ch == (int)NCKEY_UP) {
            editor_move_rows(-1);
            vi_pending = 0;
            continue;
        } else if (ch == (int)NCKEY_DOWN) {
            editor_move_rows(1);
            vi_pending = 0;
            continue;
        } else if (ch == (int)NCKEY_LEFT) {
            if (cur_col > 0) cur_col=vi_prev(code_lines[cur_line],cur_col);
            else if (cur_line > 0) { cur_line--; cur_col = strlen(code_lines[cur_line]); }
            vi_pending = 0;
            continue;
        } else if (ch == (int)NCKEY_RIGHT) {
            if (cur_col < (int)strlen(code_lines[cur_line])) cur_col=vi_next(code_lines[cur_line],cur_col,(int)strlen(code_lines[cur_line]));
            else if (cur_line < code_line_count-1) { cur_line++; cur_col = 0; }
            vi_pending = 0;
            continue;
        } else if (ch == (int)NCKEY_HOME) {
            cur_col = 0;
            vi_pending = 0;
            continue;
        } else if (ch == (int)NCKEY_END) {
            cur_col = strlen(code_lines[cur_line]);
            vi_pending = 0;
            continue;
        } else if (ch == (int)NCKEY_PGDOWN || ch == (int)NCKEY_PGUP) {
            if (show_docs) {
                int mid, h, w, sh, th;
                right_geom(&mid, &h, &w, &sh, &th);
                int page = h - sh - th - 3;
                if (page < 1) page = 1;
                if (ch == (int)NCKEY_PGDOWN) docs_scroll += page;
                else docs_scroll -= page;
                if (docs_scroll < 0) docs_scroll = 0;
                continue;
            }
            if (pane_focus) {
                console_scroll += ch == (int)NCKEY_PGUP ? 10 : -10;
                if (console_scroll < 0) console_scroll = 0;
                continue;
            }
            {
                int mid, h, w, sh, th, foot, page;
                right_geom(&mid, &h, &w, &sh, &th);
                (void)mid; (void)w; (void)sh; (void)th;
                foot = editor_footer(h);
                page = h - 2 - foot;
                if (page < 1) page = 1;
                editor_move_rows(ch == (int)NCKEY_PGDOWN ? page : -page);
            }
            vi_pending = 0;
            continue;
        } else if (vi_mode && vi_normal) {
            if (ch == 17) break;
            /* Motions/operators are handled by vi_handle_normal above. */
            if (ch == 'i') { vi_normal = 0; vi_pending=0; snprintf(status_msg, 256, "vi insert"); continue; }
            if (ch == 'a') { cur_col=vi_next(code_lines[cur_line],cur_col,(int)strlen(code_lines[cur_line])); vi_normal=0; vi_pending=0; snprintf(status_msg, 256, "vi insert"); continue; }
            if (ch == 'I') { cur_col = vi_first_nonblank(code_lines[cur_line]); vi_normal=0; vi_pending=0; snprintf(status_msg, 256, "vi insert"); continue; }
            if (ch == 'A') { cur_col = strlen(code_lines[cur_line]); vi_normal=0; vi_pending=0; snprintf(status_msg, 256, "vi insert"); continue; }
            if (ch == 'o') {
                undo_begin(UNDO_TYPE);
                if (line_insert(cur_line + 1, "")) {
                    cur_line++;
                    cur_col = 0;
                    vi_normal = 0;
                    undo_commit();
                    snprintf(status_msg, 256, "vi insert");
                } else {
                    undo_abort();
                    snprintf(status_msg, 256, "out of memory");
                }
                vi_pending=0; continue;
            }
            if (ch == 'O') {
                undo_begin(UNDO_TYPE);
                if (line_insert(cur_line, "")) {
                    cur_col = 0;
                    vi_normal = 0;
                    undo_commit();
                    snprintf(status_msg, 256, "vi insert");
                } else {
                    undo_abort();
                    snprintf(status_msg, 256, "out of memory");
                }
                vi_pending=0; continue;
            }
            vi_pending = 0;
            continue;
        }
        if (ch == (int)NCKEY_BACKSPACE || ch == 127 || ch == 8) {
            if (cur_col > 0) {
                char *l = code_lines[cur_line];
                int L = strlen(l);
                undo_begin(UNDO_TYPE);
                int previous=vi_prev(l,cur_col);
                memmove(l+previous, l+cur_col, (size_t)(L-cur_col+1));
                cur_col=previous;
                undo_commit();
            } else if (cur_line > 0) {
                int prevlen = (int)strlen(code_lines[cur_line - 1]);
                undo_begin(UNDO_TYPE);
                if (line_join_next(cur_line - 1)) {
                    cur_line--;
                    cur_col = prevlen;
                    undo_commit();
                } else {
                    undo_abort();
                    snprintf(status_msg, 256, "out of memory");
                }
            }
        } else if (ch == (int)NCKEY_DEL) {
            char *l = code_lines[cur_line];
            int L = (int)strlen(l);
            if (cur_col < L) {
                undo_begin(UNDO_TYPE);
                memmove(l + cur_col, l + cur_col + 1, (size_t)(L - cur_col));
                undo_commit();
            } else if (cur_line < code_line_count - 1) {
                undo_begin(UNDO_TYPE);
                if (line_join_next(cur_line)) undo_commit();
                else {
                    undo_abort();
                    snprintf(status_msg, 256, "out of memory");
                }
            }
        } else if (ch == (int)NCKEY_ENTER || ch == '\r' || ch == '\n') {
            undo_begin(UNDO_TYPE);
            if (line_split(cur_line, cur_col)) {
                cur_line++;
                cur_col = 0;
                undo_commit();
            } else {
                undo_abort();
                snprintf(status_msg, 256, "out of memory");
            }
        } else if (ch >= 32 && ch <= 255 && ch != 127) {
            char one[2] = { (char)ch, 0 };
            undo_begin(UNDO_TYPE);
            if (line_splice(cur_line, cur_col, 0, one, 1)) {
                cur_col++;
                undo_commit();
            } else {
                undo_abort();
                snprintf(status_msg, 256, "out of memory");
            }
        }
    }
    tui_shutdown();
}
