#include "tui.h"
#include "track.h"
#include "scheduler.h"
#include "audio.h"
#include "common.h"
#include "ai.h"
#include "mcp.h"
#include "wavetable.h"
#include "banks.h"
#include "sampler.h"
#include "songfile.h"
#include <cjson/cJSON.h>
#include <slang.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
extern int mcp_input_fd;
extern FILE *mcp_out_stream;

static char code_lines[MAX_CODE_LINES][MAX_CODE_LINE_LEN];
static char last_synced_code[65536] = {0};
static int vi_mode = 0; /* 0 = insert, 1 = vi-ish normal */
static int vi_normal = 0;
static int vi_pending = 0;  /* for dd/yy */
static char vi_yank[1024] = {0};
static int show_docs = 0;
static int show_state = 0;
static int show_tracks = 0;
static int show_viz = 0;
static int viz_h = 0;
static int viz_utf8 = 0;
static double split_ratio = 0.6; /* code pane fraction; F10/F11 adjust */
static int line_err[MAX_CODE_LINES];
static char first_err[256] = {0};
static int docs_scroll = 0;
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
static int file_mode = 0; /* 0 off, 1 save picker, 2 load picker */
static char file_buf[256] = {0};
static int file_len = 0;
static char file_names[256][256]; /* filtered *.json basenames */
static int file_n = 0;
static int file_sel = 0;
static int file_dirty = 1;
static char project_path[1024] = "strudelterm-song.json";
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

static void flatten_code(char *out, int maxlen) {
    out[0] = 0; int pos = 0;
    for (int i = 0; i < code_line_count; i++) {
        int l = strlen(code_lines[i]);
        if (pos + l + 2 < maxlen) {
            memcpy(out+pos, code_lines[i], l);
            pos += l;
            out[pos++] = '\n';
            out[pos] = 0;
        }
    }
}

static void replace_code_with(const char *txt) {
    for (int i = 0; i < MAX_CODE_LINES; i++) code_lines[i][0] = 0;
    code_line_count = 1;
    cur_line = 0; cur_col = 0; view_top = 0;
    const char *p = txt ? txt : "";
    int row = 0;
    char line[MAX_CODE_LINE_LEN];
    int li = 0;
    while (*p && row < MAX_CODE_LINES-1) {
        if (*p == '\n') {
            line[li] = 0;
            snprintf(code_lines[row], MAX_CODE_LINE_LEN, "%s", line);
            row++;
            li = 0;
            code_line_count = row+1;
        } else if (*p == '\r') {
            /* strip: CRLF files and MCP/AI text must not reach the renderer */
        } else if (*p == '\t') {
            if (li < MAX_CODE_LINE_LEN-3) { line[li++] = ' '; line[li++] = ' '; }
        } else if ((unsigned char)*p < 0x20) {
            /* drop other controls */
        } else if (li < MAX_CODE_LINE_LEN-1) {
            line[li++] = *p;
        }
        p++;
    }
    if (li > 0 && row < MAX_CODE_LINES) {
        line[li]=0;
        snprintf(code_lines[row], MAX_CODE_LINE_LEN, "%s", line);
        code_line_count = row+1;
    }
    if (code_line_count < 1) code_line_count = 1;
}

void tui_init(void) {
    signal(SIGWINCH, handle_winch);
    SLutf8_enable(-1); /* unicode when the locale supports it */
    log_console_stderr(0); /* stderr shares the terminal: keep it quiet */
    SLtt_get_terminfo();
    if (SLsmg_init_smg() == -1) {
        fprintf(stderr, "SLsmg_init_smg failed\n");
        exit(1);
    }
    if (SLang_init_tty(-1, 1, 0) == -1) {
        fprintf(stderr, "SLang_init_tty failed\n");
        exit(1);
    }
    if (SLkp_init() == -1) {
        fprintf(stderr, "SLkp_init failed\n");
        exit(1);
    }
    SLkp_define_keysym("\033OP", SL_KEY_F(1));
    SLkp_define_keysym("\033OQ", SL_KEY_F(2));
    SLkp_define_keysym("\033OR", SL_KEY_F(3));
    SLkp_define_keysym("\033OS", SL_KEY_F(4));
    SLkp_define_keysym("\033[11~", SL_KEY_F(1));
    SLkp_define_keysym("\033[12~", SL_KEY_F(2));
    SLkp_define_keysym("\033[13~", SL_KEY_F(3));
    SLkp_define_keysym("\033[14~", SL_KEY_F(4));
    SLkp_define_keysym("\033[15~", SL_KEY_F(5));
    SLkp_define_keysym("\033[17~", SL_KEY_F(6));
    SLkp_define_keysym("\033[18~", SL_KEY_F(7));
    SLkp_define_keysym("\033[19~", SL_KEY_F(8));
    SLkp_define_keysym("\033[20~", SL_KEY_F(9));
    SLkp_define_keysym("\033[21~", SL_KEY_F(10));
    SLkp_define_keysym("\033[23~", SL_KEY_F(11));
    SLkp_define_keysym("\033[24~", SL_KEY_F(12));
    SLsmg_utf8_enable(1);
    viz_utf8 = SLsmg_is_utf8_mode();
    /* editor colors (degrade to inverse video on mono terminals) */
    SLtt_set_color(2, NULL, "yellow", "black");        /* keyword */
    SLtt_set_color(3, NULL, "brightcyan", "black");   /* note */
    SLtt_set_color(4, NULL, "brightmagenta", "black");/* drum */
    SLtt_set_color(5, NULL, "brightblue", "black");   /* transform */
    SLtt_set_color(6, NULL, "gray", "black");          /* comment */
    SLtt_set_color(7, NULL, "brown", "black");        /* rest */
    SLtt_set_color(8, NULL, "brightgreen", "black");  /* number */
    SLtt_set_color(9, NULL, "red", "black");          /* error marker */
    rows = SLtt_Screen_Rows;
    cols = SLtt_Screen_Cols;
    code_lines[0][0] = 0;
}

void tui_shutdown(void) {
    SLsmg_reset_smg();
    SLang_reset_tty();
}

/* blank w cells (bounded) for flicker-free redraw without cls */
static void blank_inner(int r, int c, int w) {
    static char sp[512];
    static int init = 0;
    if (!init) { memset(sp, ' ', sizeof(sp)); init = 1; }
    if (w <= 0) return;
    SLsmg_gotorc(r, c);
    while (w > 0) {
        int n = w > 512 ? 512 : w;
        SLsmg_write_nstring(sp, (unsigned)n);
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

/* write text clipped to maxcells cells (controls -> space, tabs -> 2sp);
   color 0 = keep current. Returns cells written. */
static int draw_text(int r, int c, const char *text, int maxcells, int color) {
    if (maxcells <= 0 || !text) return 0;
    SLsmg_gotorc(r, c);
    if (color) SLsmg_set_color(color);
    int cells = 0;
    const char *p = text;
    while (*p && cells < maxcells) {
        int len = 0;
        unsigned cp = utf8_decode(p, &len);
        if (cp == '\t') {
            if (cells + 2 > maxcells) break;
            SLsmg_write_nstring((char *)"  ", 2);
            cells += 2;
            p += len;
        } else if (cp < 0x20 || cp == 0x7f) {
            SLsmg_write_nstring((char *)" ", 1);
            cells += 1;
            p += len;
        } else {
            int w = cell_width(cp);
            if (w == 0) { p += len; continue; }
            if (cells + w > maxcells) break;
            SLsmg_write_nstring((char *)p, (unsigned)len);
            p += len;
            cells += w;
        }
    }
    if (color) SLsmg_normal_video();
    return cells;
}

static void draw_status(void) {
    rows = SLtt_Screen_Rows; cols = SLtt_Screen_Cols;
    SLsmg_gotorc(0, 0);
    SLsmg_reverse_video();
    const char *mode_str = (input_mode == 1) ? "PROMPT" : (vi_mode ? (vi_normal ? "VI:NORMAL" : "VI:INSERT") : "INSERT");
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
        snprintf(buf, sizeof(buf), "StrudelTerm [%s] BPM:%.0f %s trk:%d%s | ! %.300s",
            mode_str, audio_get_bpm(), audio_is_playing()?"PLAY":"STOP", track_count, songstr,
            first_err);
    else
        snprintf(buf, sizeof(buf), "StrudelTerm [%s] BPM:%.0f %s trk:%d%s | %s | F1:help F2:AI F3:vi F4:state F5:eval F6:find F7:song F8:hold F9:tracks ^P:play ^Q:quit F12:viz",
            mode_str, audio_get_bpm(), audio_is_playing()?"PLAY":"STOP", track_count, songstr,
            status_msg[0] ? status_msg : "Ready");
    SLsmg_gotorc(0, 0);
    SLsmg_reverse_video();
    int used = draw_text(0, 0, buf, cols, 0);
    /* draw_text already positioned; reverse was set before it */
    SLsmg_normal_video();
    if (used < cols) blank_inner(0, used, cols - used);
}

/* ---- syntax highlight: token classes, SLsmg color objects ----
   0 normal 2 keyword 3 note 4 drum 5 transform 6 comment 7 rest 8 number */
static int is_drum_tok(const char *t) {
    static const char *drums[] = {"bd","sd","sn","hh","hat","cp","clap","tom",NULL};
    for (int i = 0; drums[i]; i++)
        if (strcmp(t, drums[i]) == 0) return 1;
    return 0;
}
static int is_transform_tok(const char *t) {
    static const char *ts[] = {
        "rev","fast","slow","gain","inst","s","sound","bank","mute","solo","midi",
        "att","attack","rel","release","dec","decay","sus","sustain","adsr",
        "scale","transpose","add","arp","lfo","vib","pw","lpf","cutoff",
        "shuffle","degrade", NULL
    };
    for (int i = 0; ts[i]; i++)
        if (strcmp(t, ts[i]) == 0) return 1;
    return 0;
}
static int is_track_kw(const char *t) {
    static const char *ks[] = {"drum","drums","sound","note","notes","n","bass","lead","melody","noise","chord","s",NULL};
    for (int i = 0; ks[i]; i++)
        if (strcmp(t, ks[i]) == 0) return 1;
    return 0;
}
static int is_number_tok(const char *t) {
    if (!*t) return 0;
    for (const char *p = t; *p; p++)
        if (!isdigit((unsigned char)*p) && *p != '.' && *p != '-') return 0;
    return 1;
}
static int is_rest_tok(const char *t) {
    return strcmp(t, "~") == 0 || strcmp(t, "_") == 0 || strcmp(t, "rest") == 0;
}

/* write seg (n chars) in color obj, tracking printed columns against maxw */
/* write at most maxw-*col cells; controls become spaces. Advances *col. */
static void hl_put(const char *seg, int n, int obj, int *col, int maxw) {
    if (n <= 0 || *col >= maxw) return;
    if (obj) SLsmg_set_color(obj);
    int i = 0;
    while (i < n && seg[i] && *col < maxw) {
        int len = 0;
        unsigned cp = utf8_decode(seg + i, &len);
        if (i + len > n) break; /* don't split caller's segment */
        if (cp == '\t') {
            if (*col + 2 > maxw) break;
            SLsmg_write_nstring((char *)"  ", 2);
            *col += 2;
        } else if (cp < 0x20 || cp == 0x7f) {
            SLsmg_write_nstring((char *)" ", 1);
            *col += 1;
        } else {
            int w = cell_width(cp);
            if (w == 0) { i += len; continue; }
            if (*col + w > maxw) break;
            SLsmg_write_nstring((char *)(seg + i), (unsigned)len);
            *col += w;
        }
        i += len;
    }
    if (obj) SLsmg_normal_video();
}

/* highlighted code line (without line-number prefix); maxw = pane width-2 */
static int draw_hl_line(const char *line, int maxw) {
    int col = 0;
    const char *p = line;
    while (*p == ' ' || *p == '\t') { hl_put(p, 1, 0, &col, maxw); p++; }
    if (*p == '#') { hl_put(p, (int)strlen(p), 6, &col, maxw); return col; }
    if (*p == 0) return col;
    /* first word */
    char w[64]; int wi = 0;
    while (*p && *p != ' ' && *p != '\t' && wi < 63) w[wi++] = *p++;
    w[wi] = 0;
    if (strcmp(w, "bpm") == 0) {
        hl_put(w, wi, 2, &col, maxw);
        while (*p) {
            if (*p == ' ' || *p == '\t') { hl_put(p, 1, 0, &col, maxw); p++; continue; }
            char t[64]; int ti = 0;
            while (*p && *p != ' ' && *p != '\t' && ti < 63) t[ti++] = *p++;
            t[ti] = 0;
            hl_put(t, ti, is_number_tok(t) ? 8 : 0, &col, maxw);
        }
        return col;
    }
    if (strcmp(w, "stop") == 0 || strcmp(w, "fx") == 0) { hl_put(w, wi, 2, &col, maxw); return col; }
    if (!is_track_kw(w)) {
        hl_put(w, wi, 0, &col, maxw);
        hl_put(p, (int)strlen(p), 0, &col, maxw);
        return col;
    }
    hl_put(w, wi, 2, &col, maxw);
    int drumtrk = (strncmp(w, "drum", 4) == 0 || strcmp(w, "sound") == 0);
    int xform = 0;
    while (*p) {
        if (*p == ' ' || *p == '\t') { hl_put(p, 1, 0, &col, maxw); p++; continue; }
        if (*p == '|') { hl_put(p, 1, 0, &col, maxw); p++; xform = 1; continue; }
        if (*p == '[' || *p == ']' || *p == '<' || *p == '>') { hl_put(p, 1, 0, &col, maxw); p++; continue; }
        if (*p == '(' || *p == ')' || *p == ',') { hl_put(p, 1, 0, &col, maxw); p++; continue; }
        char t[64]; int ti = 0;
        while (*p && *p != ' ' && *p != '\t' && *p != '|' &&
               *p != '[' && *p != ']' && *p != '<' && *p != '>' &&
               *p != '(' && *p != ')' && *p != ',' &&
               *p != '*' && ti < 63) t[ti++] = *p++;
        t[ti] = 0;
        int obj = 0;
        if (ti > 0) {
            if (is_rest_tok(t)) obj = 7;
            else if (xform) obj = (is_transform_tok(t) || is_number_tok(t) || strncmp(t, "wt:", 3) == 0) ? (is_number_tok(t) ? 8 : 5) : 0;
            else if (drumtrk) obj = is_drum_tok(t) ? 4 : (is_number_tok(t) ? 8 : 0);
            else obj = is_drum_tok(t) ? 0 : 3;
            hl_put(t, ti, obj, &col, maxw);
        }
        /* *N suffix in number color */
        while (*p == '*') {
            hl_put(p, 1, 8, &col, maxw); p++;
            char d[16]; int di = 0;
            while (*p && isdigit((unsigned char)*p) && di < 15) d[di++] = *p++;
            if (di) hl_put(d, di, 8, &col, maxw);
        }
    }
    return col;
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

/* mirror of track.c line splitting: pattern part of a track line, if any */
static const char *line_pattern_part(char *line, char *out, size_t outlen) {
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '#') return NULL;
    char w[32];
    int k = 0;
    while (*p && *p != ' ' && *p != '\t' && k < 31) w[k++] = *p++;
    w[k] = 0;
    if (strcmp(w, "bpm") == 0 || strcmp(w, "fx") == 0 || strcmp(w, "stop") == 0)
        return NULL;
    if (!is_track_kw(w)) return NULL;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return NULL;
    /* up to transform separator, like split_transforms */
    size_t n = 0;
    while (p[n] && p[n] != '|' && n + 1 < outlen) { out[n] = p[n]; n++; }
    out[n] = 0;
    return n ? out : NULL;
}

static void recheck_patterns(void) {
    memset(line_err, 0, sizeof(line_err));
    first_err[0] = 0;
    char part[MAX_CODE_LINE_LEN];
    for (int i = 0; i < code_line_count; i++) {
        const char *pat = line_pattern_part(code_lines[i], part, sizeof(part));
        if (!pat) continue;
        Node *n = parse_pattern_text(pat);
        node_free(n);
        const char *e = pattern_last_error();
        if (e && *e) {
            line_err[i] = 1;
            if (!first_err[0])
                snprintf(first_err, sizeof(first_err), "L%d: %.200s", i + 1, e);
        }
    }
}


static void draw_code(void) {
    rows = SLtt_Screen_Rows; cols = SLtt_Screen_Cols;
    int mid = (int)(cols * split_ratio);
    if (mid < 24) mid = 24;
    int h = rows - 3 - viz_h;
    int w = mid;
    SLsmg_draw_box(1, 0, h, w);
    SLsmg_gotorc(1, 2);
    SLsmg_write_string(" code ");
    int visible = h - 2;
    if (cur_line < view_top) view_top = cur_line;
    if (cur_line >= view_top + visible) view_top = cur_line - visible + 1;
    if (view_top < 0) view_top = 0;
    for (int r = 0; r < visible; r++) {
        int ln = view_top + r;
        if (ln >= code_line_count) { blank_inner(r+2, 1, w-2); continue; }
        char num[16];
        snprintf(num, sizeof(num), "%4d ", ln+1);
        SLsmg_gotorc(r+2, 1);
        if (line_err[ln]) SLsmg_set_color(9);
        SLsmg_write_string(num);
        if (line_err[ln]) SLsmg_normal_video();
        int used = 5 + draw_hl_line(code_lines[ln], w-2-5);
        if (used < w - 2) blank_inner(r+2, 1+used, w-2-used);
    }
    // cursor highlight in code pane
    int cy = cur_line - view_top + 2;
    int cx = cur_col + 6;
    if (input_mode == 0 && cy >= 2 && cy <= h-1 && cx < w-1) {
        char cch = ' ';
        if (cur_col < (int)strlen(code_lines[cur_line])) cch = code_lines[cur_line][cur_col];
        char cstr[2] = { cch, 0 };
        SLsmg_gotorc(cy, cx);
        SLsmg_reverse_video();
        SLsmg_write_string(cstr);
        SLsmg_normal_video();
        SLsmg_gotorc(cy, cx);
    }
}

/* ---- Tab completion ---- */
static const char *kw_cands[] = {"bpm","stop","drum","drums","sound","note","notes","n","chord","bass","lead","melody","noise","s","fx",NULL};
static const char *xform_cands[] = {
    "rev","fast","slow","gain","inst","sound","bank","mute","solo","midi",
    "att","decay","sustain","rel","adsr","scale","transpose","arp","lfo","vib",
    "pw","lpf","cutoff","shuffle","degrade",NULL};
static const char *inst_cands[] = {"sine","saw","sawtooth","sqr","square","tri","triangle","noise","white","wt",NULL};
static const char *drum_cands[] = {"bd","sd","sn","hh","hat","cp","clap","tom","~",NULL};
static char note_pool[40][8];
static int note_pool_init = 0;

static void insert_text(const char *txt) {
    char *line = code_lines[cur_line];
    int L = (int)strlen(line);
    if (cur_col < 0) cur_col = 0;
    if (cur_col > L) cur_col = L;
    int tl = (int)strlen(txt);
    if (tl <= 0) return;
    if (L + tl > MAX_CODE_LINE_LEN - 2) tl = MAX_CODE_LINE_LEN - 2 - L;
    if (tl <= 0) return;
    memmove(line + cur_col + tl, line + cur_col, (size_t)(L - cur_col + 1));
    memcpy(line + cur_col, txt, (size_t)tl);
    cur_col += tl;
}

static void complete_at_cursor(void) {
    char *line = code_lines[cur_line];
    int L = (int)strlen(line);
    if (cur_col > L) cur_col = L;
    int s = cur_col;
    while (s > 0 && line[s-1] != ' ' && line[s-1] != '\t' && line[s-1] != '[' &&
           line[s-1] != ']' && line[s-1] != '<' && line[s-1] != '>' && line[s-1] != '|') s--;
    char prefix[64];
    int plen = cur_col - s;
    if (plen >= 63) plen = 63;
    memcpy(prefix, line + s, (size_t)plen);
    prefix[plen] = 0;
    int first = 1;
    for (int i = 0; i < s; i++)
        if (line[i] != ' ' && line[i] != '\t') { first = 0; break; }
    char prev[64] = {0};
    if (!first) {
        int e = s;
        while (e > 0 && (line[e-1] == ' ' || line[e-1] == '\t')) e--;
        int b = e;
        while (b > 0 && line[b-1] != ' ' && line[b-1] != '\t' && line[b-1] != '|') b--;
        int pl = e - b;
        if (pl > 63) pl = 63;
        if (pl > 0) { memcpy(prev, line + b, (size_t)pl); prev[pl] = 0; }
    }
    int bar = 0;
    for (int i = 0; i < s; i++) if (line[i] == '|') { bar = 1; break; }
    char firstw[64] = {0};
    {
        int i = 0;
        while (line[i] == ' ' || line[i] == '\t') i++;
        int k = 0;
        while (line[i] && line[i] != ' ' && line[i] != '\t' && k < 63) firstw[k++] = line[i++];
        firstw[k] = 0;
    }
    int drumtrk = (strncmp(firstw, "drum", 4) == 0 || strcmp(firstw, "sound") == 0);

    const char *cands[80];
    int nc = 0;
    char wtbuf[16][48];
    if (first) {
        for (int i = 0; kw_cands[i] && nc < 80; i++) cands[nc++] = kw_cands[i];
    } else if (strcmp(prev, "inst") == 0 || strcmp(prev, "sound") == 0 ||
               (bar && strcmp(prev, "s") == 0)) {
        for (int i = 0; inst_cands[i] && nc < 60; i++) cands[nc++] = inst_cands[i];
        int nw = wt_count();
        if (nw > 15) nw = 15;
        for (int i = 0; i < nw && nc < 80; i++) {
            snprintf(wtbuf[i], sizeof(wtbuf[i]), "wt:%s", wt_name(i));
            cands[nc++] = wtbuf[i];
        }
    } else if (strcmp(prev, "fast") == 0 || strcmp(prev, "slow") == 0 || strcmp(prev, "gain") == 0 ||
               strcmp(prev, "transpose") == 0 || strcmp(prev, "add") == 0 ||
               strcmp(prev, "pw") == 0 || strcmp(prev, "lpf") == 0 || strcmp(prev, "cutoff") == 0 ||
               strcmp(prev, "att") == 0 || strcmp(prev, "attack") == 0 ||
               strcmp(prev, "decay") == 0 || strcmp(prev, "dec") == 0 ||
               strcmp(prev, "sustain") == 0 || strcmp(prev, "sus") == 0 ||
               strcmp(prev, "rel") == 0 || strcmp(prev, "release") == 0) {
        snprintf(status_msg, sizeof(status_msg), "number expected");
        return;
    } else if (strcmp(prev, "scale") == 0) {
        static const char *sc[] = {"C:major","C:minor","C:dorian","C:mixolydian","A:minor:pentatonic","C:blues",NULL};
        for (int i = 0; sc[i] && nc < 80; i++) cands[nc++] = sc[i];
    } else if (strcmp(prev, "lfo") == 0) {
        static const char *lc[] = {"gain","freq","pitch","lpf","pw","sine","triangle","saw","square",NULL};
        for (int i = 0; lc[i] && nc < 80; i++) cands[nc++] = lc[i];
    } else if (strcmp(prev, "arp") == 0) {
        snprintf(status_msg, sizeof(status_msg), "arp index pattern, e.g. 0 1 2 1");
        return;
    } else if (strcmp(firstw, "fx") == 0) {
        static const char *fxc[] = {"drive","cutoff","delay","verb",NULL};
        for (int i = 0; fxc[i] && nc < 80; i++) cands[nc++] = fxc[i];
    } else if (bar) {
        for (int i = 0; xform_cands[i] && nc < 80; i++) cands[nc++] = xform_cands[i];
    } else if (drumtrk) {
        for (int i = 0; drum_cands[i] && nc < 80; i++) cands[nc++] = drum_cands[i];
    } else {
        if (!note_pool_init) {
            const char *lets = "cdefgab";
            int n = 0;
            for (int o = 2; o <= 5 && n < 32; o++)
                for (int li = 0; lets[li] && n < 32; li++) {
                    snprintf(note_pool[n], sizeof(note_pool[n]), "%c%d", lets[li], o);
                    n++;
                }
            snprintf(note_pool[n], sizeof(note_pool[n]), "%s", "~");
            n++;
            snprintf(note_pool[n], sizeof(note_pool[n]), "%s", "rest");
            n++;
            note_pool_init = n;
        }
        for (int i = 0; i < note_pool_init && nc < 80; i++) cands[nc++] = note_pool[i];
    }

    const char *m[80];
    int nm = 0;
    for (int i = 0; i < nc; i++)
        if (strncmp(cands[i], prefix, (size_t)plen) == 0) m[nm++] = cands[i];
    if (nm == 0) { snprintf(status_msg, sizeof(status_msg), "no completion"); return; }
    /* longest common extension beyond prefix */
    int ext = plen;
    for (;;) {
        char c = 0;
        int ok = 1;
        for (int i = 0; i < nm; i++) {
            if (m[i][ext] == 0) { ok = 0; break; }
            if (c == 0) c = m[i][ext];
            else if (m[i][ext] != c) { ok = 0; break; }
        }
        if (!ok) break;
        ext++;
    }
    if (ext > plen) {
        char ins[64];
        int il = ext - plen;
        if (il > 63) il = 63;
        memcpy(ins, m[0] + plen, (size_t)il);
        ins[il] = 0;
        insert_text(ins);
    }
    if (nm == 1) {
        snprintf(status_msg, sizeof(status_msg), "completed: %s", m[0]);
    } else {
        char lst[256]; int lp = 0;
        lst[0] = 0;
        for (int i = 0; i < nm && lp < 220; i++) {
            int wlen = snprintf(lst + lp, sizeof(lst) - (size_t)lp, "%s%s", i ? " | " : "", m[i]);
            if (wlen < 0) break;
            lp += wlen;
        }
        lst[sizeof(lst)-1] = 0;
        snprintf(status_msg, sizeof(status_msg), "%.240s", lst);
    }
}

/* ---- bank search ---- */

static void search_use_bank(const char *dirfrag);
static void search_refresh(void) {
    int cnt = banks_count();
    if (!search_dirty && cnt == search_lastcount) return;
    search_dirty = 0;
    search_lastcount = cnt;
    if (search_path[0]) {
        search_ndirs_total = banks_list_dir(search_path, search_buf, search_dirs, 256);
        if (search_ndirs_total < 0) search_ndirs_total = 0;
        search_ndirs = search_ndirs_total < 256 ? search_ndirs_total : 256;
    } else {
        search_ndirs_total = banks_search_dirs(search_buf, search_dirs, search_dircounts, 256);
        search_ndirs = search_ndirs_total < 256 ? search_ndirs_total : 256;
    }
    int mx = search_ndirs + (search_path[0] ? 1 : 0) - 1; /* +1 for '..' */
    if (search_sel > mx) search_sel = mx;
    if (search_sel < 0) search_sel = 0;
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
    int t = search_row_target();
    if (t == -2) { search_ascend(); return; }
    if (t < 0 || t >= search_ndirs) return;
    char probe[1][512];
    int has_kids = banks_list_dir(search_dirs[t], "", probe, 1);
    if (has_kids > 0) {
        snprintf(search_path, sizeof(search_path), "%s", search_dirs[t]);
        search_sel = 0;
        search_buf[0] = 0;
        search_len = 0;
        search_dirty = 1;
        snprintf(status_msg, sizeof(status_msg), "browsing %.200s", search_dirs[t]);
    } else {
        search_use_bank(search_dirs[t]);
        search_mode = 0;
    }
}

/* ---- song file picker (Ctrl+S save, Ctrl+O load) ---- */

static void project_save(const char *path);
static void project_load(const char *path, int quiet);

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

/* typed/selected name -> confined CWD path. Returns 1 with out set. */
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
    snprintf(out, outlen, "%s", tmp);
    return 1;
}

static void file_confirm(void) {
    char path[1024];
    if (!file_pick_path(path, sizeof(path))) {
        snprintf(status_msg, sizeof(status_msg), "bad name (a-z 0-9 _ . -)");
        return;
    }
    if (file_mode == 1) {
        char before[1024];
        snprintf(before, sizeof(before), "%s", project_path);
        project_save(path);
        /* project_save sets project_path itself; keep picker open on failure */
        FILE *probe = fopen(project_path, "r");
        if (probe) { fclose(probe); file_mode = 0; }
        else snprintf(project_path, sizeof(project_path), "%s", before);
    } else {
        project_load(path, 0);
        /* project_load logs failure; close picker only if file read */
        FILE *probe = fopen(path, "r");
        if (probe) { fclose(probe); file_mode = 0; }
    }
}

static void draw_search_at(int r0, int c0, int h, int w) {
    if (h < 4 || w < 12) return;
    SLsmg_draw_box(r0, c0, h, w);
    SLsmg_gotorc(r0, c0+2);
    if (search_path[0]) {
        char t[128];
        snprintf(t, sizeof(t), " banks: %.100s/ ", search_path);
        SLsmg_write_string(t);
    } else {
        SLsmg_write_string(" banks (F6:close) ");
    }
    char q[256];
    snprintf(q, sizeof(q), "find> %.200s", search_buf);
    int qu = draw_text(r0+1, c0+1, q, w-2, 0);
    if (qu < w - 2) blank_inner(r0+1, c0+1+qu, w-2-qu);
    int visible = h - 4;
    char info[256];
    if (banks_failed()) snprintf(info, sizeof(info), "no banks dir (STRUDELTERM_BANKS?)");
    else if (!banks_ready()) snprintf(info, sizeof(info), "indexing... %d files", banks_count());
    else if (search_path[0]) snprintf(info, sizeof(info), "%d entr%s",
        search_ndirs_total, search_ndirs_total == 1 ? "y" : "ies");
    else if (search_ndirs_total <= 0) snprintf(info, sizeof(info), "%d files indexed, no match", banks_count());
    else snprintf(info, sizeof(info), "%d bank%s",
        search_ndirs_total, search_ndirs_total == 1 ? "" : "s");
    int iu = draw_text(r0+2, c0+1, info, w-2, 0);
    if (iu < w - 2) blank_inner(r0+2, c0+1+iu, w-2-iu);
    int top = search_sel - visible + 1;
    if (top < 0) top = 0;
    int nrows = search_ndirs + (search_path[0] ? 1 : 0);
    for (int r = 0; r < visible; r++) {
        int idx = top + r;
        if (idx >= nrows) { blank_inner(r0+3+r, c0+1, w-2); continue; }
        char b[256];
        if (search_path[0] && idx == 0) {
            snprintf(b, sizeof(b), "%s", "..");
        } else {
            int di = idx - (search_path[0] ? 1 : 0);
            if (search_path[0]) {
                const char *base = strrchr(search_dirs[di], '/');
                snprintf(b, sizeof(b), "> %.200s/", base ? base + 1 : search_dirs[di]);
            } else {
                snprintf(b, sizeof(b), "> %.200s (%d)", search_dirs[di], search_dircounts[di]);
            }
        }
        SLsmg_gotorc(r0+3+r, c0+1);
        if (idx == search_sel) SLsmg_reverse_video();
        int used = draw_text(r0+3+r, c0+1, b, w-2, 0);
        if (idx == search_sel) SLsmg_normal_video();
        if (used < w - 2) blank_inner(r0+3+r, c0+1+used, w-2-used);
    }
    const char *foot = search_ndirs_total > search_ndirs
        ? "...more (refine query)"
        : "Enter:open/use Tab:use bank Up/Down:sel Bs:up";
    int fu = draw_text(r0+h-1, c0+1, foot, w-2, 0);
    if (fu < w - 2) blank_inner(r0+h-1, c0+1+fu, w-2-fu);
}

static void draw_file_at(int r0, int c0, int h, int w) {
    if (h < 4 || w < 12) return;
    SLsmg_draw_box(r0, c0, h, w);
    SLsmg_gotorc(r0, c0+2);
    SLsmg_write_string(file_mode == 1 ? " save song (Esc:cancel) " : " load song (Esc:cancel) ");
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
        if (idx == file_sel) SLsmg_reverse_video();
        int used = draw_text(r0+3+r, c0+1, b, w-2, 0);
        if (idx == file_sel) SLsmg_normal_video();
        if (used < w - 2) blank_inner(r0+3+r, c0+1+used, w-2-used);
    }
    const char *ffoot = file_mode == 1
        ? "Enter:save Up/Down:pick existing"
        : "Enter:load Up/Down:sel";
    int fu2 = draw_text(r0+h-1, c0+1, ffoot, w-2, 0);
    if (fu2 < w - 2) blank_inner(r0+h-1, c0+1+fu2, w-2-fu2);
}

/* append "| bank <dir>" to a drum line, else insert a new drum line */
static void search_use_bank(const char *dirfrag) {
    char firstw[64] = {0};
    char *line = code_lines[cur_line];
    int i = 0;
    while (line[i] == ' ' || line[i] == '\t') i++;
    int k = 0;
    while (line[i] && line[i] != ' ' && line[i] != '\t' && k < 63) firstw[k++] = line[i++];
    firstw[k] = 0;
    int drumtrk = (strncmp(firstw, "drum", 4) == 0 || strcmp(firstw, "sound") == 0);
    if (drumtrk) {
        size_t L = strlen(line);
        int hasbar = strchr(line, '|') != NULL;
        char add[512];
        snprintf(add, sizeof(add), "%sbank %.400s", hasbar ? " " : " | ", dirfrag);
        snprintf(line + L, MAX_CODE_LINE_LEN - L, "%s", add);
        cur_col = (int)strlen(line);
        snprintf(status_msg, sizeof(status_msg), "bank set");
    } else {
        if (code_line_count >= MAX_CODE_LINES - 1) return;
        for (int j = code_line_count; j > cur_line + 1; j--)
            memcpy(code_lines[j], code_lines[j-1], MAX_CODE_LINE_LEN);
        snprintf(code_lines[cur_line+1], MAX_CODE_LINE_LEN,
            "drum bd sd hh cp | bank %.200s", dirfrag);
        code_line_count++;
        cur_line++;
        cur_col = 0;
        snprintf(status_msg, sizeof(status_msg), "drum line added");
    }
}

/* ---- right-pane geometry: main area rows 1..rows-3, right side at col mid ---- */
static void right_geom(int *mid_out, int *h_out, int *w_out, int *state_h_out, int *track_h_out) {
    rows = SLtt_Screen_Rows; cols = SLtt_Screen_Cols;
    int mid = (int)(cols * split_ratio);
    if (mid < 24) mid = 24;
    if (mid > cols - 20) mid = cols - 20;
    if (mid < 0) mid = 0;
    int h = rows - 3 - viz_h;
    if (h < 0) h = 0;
    int w = cols - mid;
    if (w < 0) w = 0;
    int sh = 0;
    if (show_state && h >= 14 && w >= 30) sh = 10;
    int th = 0;
    if (show_tracks && h - sh >= 16 && w >= 24) {
        int need = scheduler_song_count() + 2;
        if (need < 4) need = 4;
        if (need > 12) need = 12;
        if (h - sh - need >= 5) th = need;
    }
    *mid_out = mid; *h_out = h; *w_out = w; *state_h_out = sh; *track_h_out = th;
}

static void draw_console_at(int r0, int c0, int h, int w) {
    if (h < 3 || w < 4) return;
    SLsmg_draw_box(r0, c0, h, w);
    SLsmg_gotorc(r0, c0+2);
    SLsmg_write_string(show_docs ? " console (F1:back) " : " console (F1:help) ");
    int visible = h - 2;
    int cnt = console_count_get();
    int start = (cnt <= visible) ? 0 : (cnt - visible);
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
    {1, "StrudelTerm language cheatsheet"},
    {0, "One voice per line. F5 evaluates. Cycle = 4 beats."},
    {0, "A song track holds a whole buffer; F7 steps tracks."},
    {0, ""},
    {1, "Track lines"},
    {2, "bpm 128"},
    {2, "drum bd sd [bd bd] sd"},
    {2, "drum hh hh hh hh | fast 2"},
    {2, "bass c2 e2 g2 c3 | rev"},
    {2, "lead c4 e4 g4 c5 | slow 2"},
    {2, "drum bd bd bd bd | inst wt:acid"},
    {0, "keywords: drum sound note notes n chord bass lead melody noise s"},
    {0, ""},
    {1, "Pattern mini-notation"},
    {3, "space separates steps in one cycle"},
    {3, "[ a b ] groups steps into one slot"},
    {3, "< a b > picks one per cycle (rotates)"},
    {3, "tok*N repeats inside slot, incl. [..]*N and <..>*N (1-64)"},
    {3, "euclid(k,n,tok[,rot])  Bjorklund rhythm"},
    {3, "every(n, X)  play X every nth cycle"},
    {3, "~  _  -  rest are rests"},
    {3, "a,b  stacks over the same time (chords)"},
    {3, "typos show as pattern errors in the console"},
    {0, ""},
    {1, "Transforms (after |)"},
    {3, "rev  mirror timing in cycle"},
    {3, "fast N  compress + repeat (1-64)"},
    {3, "slow N  play every Nth cycle"},
    {3, "gain X  volume 0-2"},
    {3, "attack/decay/sustain/release  or adsr A:D:S:R"},
    {3, "inst/sound/s  sine saw square tri noise (pink=white)"},
    {3, "inst wt[:name]  wavetable: sine tri saw square"},
    {3, "  organ acid voice pluck pad glass (bare wt = saw)"},
    {3, "bank NAME  drum samples (e.g. bank 909)"},
    {3, "bank NAME on notes too: nearest pitch, shifted"},
    {3, "scale C:minor  degrees 0 2 4 (root octave 3)"},
    {3, "transpose N  semitones (alias add)"},
    {3, "arp 0 1 2 1  indexes a stacked chord"},
    {3, "lfo gain|freq|lpf|pw [shape] rate [depth]"},
    {3, "vib 4  or vib 4:.1  sine pitch LFO"},
    {3, "pw 0.5  pulse width; lpf/cutoff Hz per voice"},
    {0, ""},
    {1, "Sounds"},
    {3, "drums: bd kick, sd/sn snare, hh/hat hat,"},
    {3, "cp/clap clap, tom tom (unknown -> hat)"},
    {3, "notes: c d e f g a b + #/b + octave"},
    {3, "e.g. c4 eb4 f#3, or MIDI number 60"},
    {3, "chords: Cm7 C^7 C-7  (keyword chord: C = major)"},
    {3, "on note tracks c7 is the note, cm7 is a chord"},
    {3, "defaults: bass>saw lead>square notes/n/chord>tri"},
    {0, ""},
    {1, "Limits"},
    {3, "32 voices per buffer, 32 song tracks, 512-char lines"},
    {3, "64kB code, nesting 32 deep, bpm 20-400"},
    {0, ""},
    {1, "Keys"},
    {3, "F1 help  F2 AI  F3 vi  F4 state  F5 eval  F6 banks"},
    {3, "F7 song  F8 hold  F9 tracks  Tab completes"},
    {3, "Ctrl+T copy buffer to track  Ctrl+N/B next/prev"},
    {3, "Ctrl+D dup line  Ctrl+S save song  Ctrl+O load song"},
    {3, "Ctrl+P play  Ctrl+Q quit  Esc vi-normal"},
    {3, "F12 piano roll + scope  PgUp/PgDn scroll this help"},
};
static const int cheat_n = (int)(sizeof(cheat) / sizeof(cheat[0]));

/* write one wrapped visual line with style; handles `code` spans via reverse */
/* docs content is static ASCII: byte count == cell count. Returns cells. */
static int write_styled(const char *text, int style) {
    int cells = 0;
    if (style == 1) SLsmg_reverse_video();
    const char *p = text;
    int in_code = 0;
    char seg[256];
    int si = 0;
    while (*p) {
        if (*p == '`') {
            seg[si] = 0;
            if (si) {
                if (style == 2 || in_code) { SLsmg_reverse_video(); SLsmg_write_string(seg); SLsmg_normal_video(); if (style == 1) SLsmg_reverse_video(); }
                else SLsmg_write_string(seg);
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
        if (style == 2 || in_code) { SLsmg_reverse_video(); SLsmg_write_string(seg); SLsmg_normal_video(); if (style == 1) SLsmg_reverse_video(); }
        else SLsmg_write_string(seg);
        cells += si;
    }
    if (style == 1 || style == 2) SLsmg_normal_video();
    return cells;
}

/* word-wrap logical doc lines into visual rows for width w */
static void draw_docs_at(int r0, int c0, int h, int w) {
    if (h < 3 || w < 8) return;
    SLsmg_draw_box(r0, c0, h, w);
    SLsmg_gotorc(r0, c0+2);
    SLsmg_write_string(" cheatsheet (F1:back PgUp/PgDn:scroll) ");
    int visible = h - 2;
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
            else { SLsmg_gotorc(r0+1+row, c0+1); row++; }
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
                SLsmg_gotorc(r0+1+row, c0+1);
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
        if (hl < w - 2) { SLsmg_gotorc(r0, c0 + w - 1 - hl); SLsmg_write_string(hint); }
    }
}

static void draw_state_at(int r0, int c0, int h, int w) {
    if (h < 5 || w < 20) return;
    SLsmg_draw_box(r0, c0, h, w);
    SLsmg_gotorc(r0, c0+2);
    SLsmg_write_string(" state (F4:hide) ");
    char b[256];
    int row = r0 + 1;
    int maxrow = r0 + h - 1;
    double now = audio_time_now();
    int qd = event_queue_depth();
    int av = 0; int by[10] = {0};
    audio_voice_stats(&av, by);
    const char *mode = (input_mode == 1) ? "PROMPT" : (vi_mode ? (vi_normal ? "VI:N" : "VI:I") : "INS");
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
    mcp_status(mcpst, sizeof(mcpst));
    snprintf(b, sizeof(b), "trk=%d ai=%s mcp=%s %dx%d cur=%d/%d",
        track_count, ai_is_busy() ? "busy" : "idle", mcpst,
        cols, rows, cur_line + 1, cur_col);
    if (row < maxrow) {
        int used = draw_text(row, c0+1, b, w-2, 0);
        if (used < w - 2) blank_inner(row, c0+1+used, w-2-used);
        row++;
    }
    /* per-track lines */
    pthread_mutex_lock(&tracks_lock);
    int n = track_count;
    if (n > MAX_TRACKS) n = MAX_TRACKS;
    for (int i = 0; i < n && row < maxrow; i++) {
        Track *t = &tracks[i];
        double wait = t->next_cycle - now;
        const char *inm = (t->inst == INST_WT && t->wt >= 0) ? wt_name(t->wt) : audio_inst_name(t->inst);
        snprintf(b, sizeof(b), "%d %.12s f%d s%d %s g%.1f %.10s %s%s cyc%d %+.1fs",
            i, t->keyword, t->fast_repeat, t->slow_stride,
            t->rev ? "rev" : "   ", t->gain,
            inm, t->mute ? "M" : " ", t->solo ? "S" : " ", t->cycle_index, wait);
        int used = draw_text(row, c0+1, b, w-2, 0);
        if (used < w - 2) blank_inner(row, c0+1+used, w-2-used);
        row++;
    }
    pthread_mutex_unlock(&tracks_lock);
    while (row < maxrow) { blank_inner(row, c0+1, w-2); row++; }
    (void)mode;
}

static void draw_tracks_at(int r0, int c0, int h, int w) {
    if (h < 4 || w < 12) return;
    SLsmg_draw_box(r0, c0, h, w);
    SLsmg_gotorc(r0, c0+2);
    char title[64];
    snprintf(title, sizeof(title), " tracks%s%s (F9:hide) ",
        scheduler_song_on() ? " SONG" : "",
        scheduler_song_hold() ? " HOLD" : "");
    SLsmg_write_string(title);
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
        SLsmg_gotorc(r0+1+r, c0+1);
        if (idx == pos) SLsmg_reverse_video();
        int used = draw_text(r0+1+r, c0+1, b, w-2, 0);
        if (idx == pos) SLsmg_normal_video();
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
    SLsmg_draw_box(r0, 0, h, w);
    SLsmg_gotorc(r0, 2);
    SLsmg_write_string(" piano roll + scope (F12:hide) ");
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
        SLsmg_gotorc(r0 + 2, gutter + 1);
        SLsmg_write_string("(no notes yet)");
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
            SLsmg_gotorc(r0 + 1 + r, 1);
            SLsmg_write_string(lab);
            SLsmg_write_nstring(grid[r], (unsigned)rw);
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
            SLsmg_gotorc(sc_base, gutter + 1 + x);
            if (viz_utf8) {
                unsigned bits = 0;
                for (int lv = lo2; lv <= hi2; lv++) {
                    bits |= braille_bit(0, lv) | braille_bit(1, lv);
                }
                SLsmg_write_char((SLwchar_Type)(0x2800 + bits));
            } else {
                static const char *lvch = " .:-=+*#@";
                char c[2] = { lvch[hi2], 0 };
                SLsmg_write_string(c);
            }
        }
        if (!viz_utf8) {
            /* second scope row: center line */
            SLsmg_gotorc(sc_base + 1, gutter + 1);
            for (int x = 0; x < rw; x++) SLsmg_write_string("-");
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
    else draw_console_at(1, mid, top_h, w);
    if (th > 0) draw_tracks_at(1 + top_h, mid, th, w);
    if (sh > 0) draw_state_at(1 + top_h + th, mid, sh, w);
}

/* load song track i into the editor (manual navigation) */
static void sync_editor_from_track(int i) {
    char tmp[65536];
    if (!scheduler_track_code(i, tmp, sizeof(tmp))) return;
    replace_code_with(tmp);
    snprintf(last_synced_code, sizeof(last_synced_code), "%s", current_code);
}

static void draw_console(void) {
    draw_right_pane();
}

static void draw_input(void) {
    rows = SLtt_Screen_Rows; cols = SLtt_Screen_Cols;
    char buf[256];
    int used;
    if (input_mode == 1) snprintf(buf, sizeof(buf), "AI> %.240s", input_buf);
    else snprintf(buf, sizeof(buf), "Type code above. Tab=complete F1=help F2=AI F4=state F6=find banks.");
    used = draw_text(rows-2, 0, buf, cols, 0);
    if (used < cols) blank_inner(rows-2, used, cols - used);
    snprintf(buf, sizeof(buf), "Enter submits prompt. Esc cancels.");
    used = draw_text(rows-1, 0, buf, cols, 0);
    if (used < cols) blank_inner(rows-1, used, cols - used);
}

static void do_evaluate(void) {
    char buf[65536];
    flatten_code(buf, 65536);
    track_load_from_code(buf);
    int spos = scheduler_song_pos();
    if (spos >= 0) scheduler_track_update(spos, buf); /* commit to active track */
    status_msg[0] = 0;
    snprintf(status_msg, 256, "Evaluated %d tracks", track_count);
}

static void duplicate_line(void) {
    if (code_line_count >= MAX_CODE_LINES - 1) return;
    for (int i = code_line_count; i > cur_line + 1; i--)
        memcpy(code_lines[i], code_lines[i-1], MAX_CODE_LINE_LEN);
    memcpy(code_lines[cur_line+1], code_lines[cur_line], MAX_CODE_LINE_LEN);
    code_line_count++;
    cur_line++;
    snprintf(status_msg, sizeof(status_msg), "line duplicated");
}

static void project_save(const char *path) {
    if (path && *path) snprintf(project_path, sizeof(project_path), "%s", path);
    char code[65536];
    flatten_code(code, sizeof(code));
    int n = songfile_save(project_path, code, audio_get_bpm(), 0);
    if (n > 0) {
        log_console("saved %s (%d track%s)", project_path, n, n == 1 ? "" : "s");
        snprintf(status_msg, sizeof(status_msg), "saved %.200s (%d track%s)",
            project_path, n, n == 1 ? "" : "s");
    } else {
        snprintf(status_msg, sizeof(status_msg), "save failed");
    }
}

static int code_has_bpm(const char *code) {
    if (!code) return 0;
    if (strncmp(code, "bpm ", 4) == 0 || strncmp(code, "bpm\t", 4) == 0) return 1;
    return strstr(code, "\nbpm ") != NULL || strstr(code, "\nbpm\t") != NULL;
}

static void project_load(const char *path, int quiet) {
    if (path && *path) snprintf(project_path, sizeof(project_path), "%s", path);
    char code[65536];
    double b = 0;
    int n = songfile_load(project_path, code, sizeof(code), &b, quiet);
    if (n < 0) return;
    int apply_bpm = (b > 0 && !code_has_bpm(code));
    replace_code_with(code);
    do_evaluate();
    if (apply_bpm) audio_set_bpm(b);
    snprintf(last_synced_code, sizeof(last_synced_code), "%s", current_code);
    if (!quiet) {
        snprintf(status_msg, sizeof(status_msg), "loaded %.200s (%d track%s)",
            project_path, n, n == 1 ? "" : "s");
    }
    log_console("loaded %s (%d track%s)", project_path, n, n == 1 ? "" : "s");
}

void tui_load_initial(const char *code) {
    if (!code || !*code) return;
    replace_code_with(code);
    do_evaluate();
    snprintf(last_synced_code, sizeof(last_synced_code), "%s", current_code);
}

/* session state path: $XDG_CONFIG_HOME/strudelterm/state.json */
static void state_path(char *out, size_t outlen) {
    char base[1024];
    if (!out || outlen == 0) return;
    out[0] = 0;
    user_dir(base, sizeof(base), "XDG_CONFIG_HOME", "strudelterm", ".config/strudelterm");
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
    project_load(p, 1);
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
            if (a) { audio_set_bpm(atof(a)); snprintf(status_msg,256,"BPM %.0f",audio_get_bpm()); }
        } else if (cmd && strcmp(cmd,"play")==0) {
            audio_toggle_play(1); scheduler_start(); snprintf(status_msg,256,"playing");
        } else if (cmd && strcmp(cmd,"stop")==0) {
            audio_toggle_play(0); snprintf(status_msg,256,"stopped");
        } else if (cmd && strcmp(cmd,"clear")==0) {
            replace_code_with("");
            log_console("Code cleared");
        } else if (cmd && strcmp(cmd,"rescan")==0) {
            smp_invalidate();
            if (banks_rescan()) snprintf(status_msg, 256, "rescanning banks...");
            else snprintf(status_msg, 256, "indexing already running");
        } else if (cmd && strcmp(cmd,"save")==0) {
            char *a = strtok_r(NULL, " \t", &save);
            project_save(a);
        } else if (cmd && strcmp(cmd,"load")==0) {
            char *a = strtok_r(NULL, " \t", &save);
            project_load(a, 0);
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
        } else {
            log_console("Unknown command: %s", text);
        }
        return;
    }
    char ctx[65536]; flatten_code(ctx, 65536);
    status_msg[0]=0; snprintf(status_msg,256,"AI thinking...");
    SLsmg_refresh();
    ai_request_async(text, ctx);
}

void tui_frame(void) {
    /* live pattern check (only when the buffer changed) */
    {
        static unsigned last_hash = 0;
        static int have_hash = 0;
        unsigned h = hash_code();
        if (!have_hash || h != last_hash) {
            have_hash = 1;
            last_hash = h;
            recheck_patterns();
        }
    }
    struct winsize ws;
    int wsh = 0;
    if (ioctl(fileno(stdin), TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        wsh = 1;
        if (SLtt_Screen_Rows != ws.ws_row || SLtt_Screen_Cols != ws.ws_col) {
            resize_flag = 1;
        }
    } else {
        SLtt_get_screen_size();
    }
    if (resize_flag) {
        resize_flag = 0;
        SLsmg_reset_smg();
        SLtt_get_terminfo();
        SLtt_get_screen_size();
        SLsmg_init_smg();
    }
    if (wsh) {
        SLtt_Screen_Rows = ws.ws_row;
        SLtt_Screen_Cols = ws.ws_col;
    }
    rows = SLtt_Screen_Rows;
    cols = SLtt_Screen_Cols;
    viz_h = (show_viz && rows >= 28) ? VIZ_H : 0;
    /* full clear only on first frame or resize; otherwise every pane
       rewrites all its rows, so SLsmg diffs stay minimal (no flicker) */
    {
        static int first = 1, lr = 0, lc = 0;
        if (first || rows != lr || cols != lc) {
            SLsmg_cls();
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
    int cursor_r = 0, cursor_c = 0;
    if (search_mode) {
        int mid = (cols * split_ratio < 24) ? 24 : (int)(cols * split_ratio);
        SLsmg_gotorc(2, mid + 7 + search_len);
        cursor_r = 2; cursor_c = mid + 7 + search_len;
    } else if (file_mode) {
        int mid = (cols * split_ratio < 24) ? 24 : (int)(cols * split_ratio);
        SLsmg_gotorc(2, mid + 7 + file_len);
        cursor_r = 2; cursor_c = mid + 7 + file_len;
    } else if (input_mode == 0) {
        int mid = (cols * split_ratio < 24) ? 24 : (int)(cols * split_ratio);
        int cy = cur_line - view_top + 2;
        int cx = cur_col + 6;
        if (cy >= 2 && cy < rows - 2 && cx < mid - 1) {
            SLsmg_gotorc(cy, cx);
            cursor_r = cy; cursor_c = cx;
        }
    } else {
        SLsmg_gotorc(rows - 2, 4 + input_len);
        cursor_r = rows - 2; cursor_c = 4 + input_len;
    }
    SLsmg_refresh();
    if (cursor_r || cursor_c) {
        SLtt_goto_rc(cursor_r, cursor_c);
        fflush(stdout);
    }
}

static int read_key(void) {
    unsigned int c = SLang_getkey();
    if (c == 0xFFFF) return SL_KEY_ERR;
    if (c == 27) {
        if (SLang_input_pending(1) == 0) {
            return 27;
        }
        SLang_ungetkey(27);
        return SLkp_getkey();
    }
    SLang_ungetkey((unsigned char)c);
    return SLkp_getkey();
}

void tui_run(void) {
    int ch;
    while (1) {
        char *res = ai_poll_result();
        if (res) {
            log_console("AI responded (%d bytes)", (int)strlen(res));
            replace_code_with(res);
            do_evaluate();
            snprintf(status_msg,256,"AI code loaded");
            free(res);
        }
        /* song auto-step runs in the generator thread; notice position
           changes first so auto-advance never clobbers typed text */
        {
            static int seen_pos = -2;
            int spos = scheduler_song_pos();
            if (spos != seen_pos) {
                seen_pos = spos;
                if (scheduler_song_follow_consume()) {
                    char tmp[65536];
                    if (scheduler_track_code(spos, tmp, sizeof(tmp))) {
                        replace_code_with(tmp);
                        snprintf(last_synced_code, sizeof(last_synced_code), "%s", current_code);
                    }
                } else {
                    snprintf(last_synced_code, sizeof(last_synced_code), "%s", current_code);
                }
            }
        }
        if (strcmp(last_synced_code, current_code) != 0) {
            replace_code_with(current_code);
            strncpy(last_synced_code, current_code, 65535);
            last_synced_code[65535] = 0;
        }
        mcp_poll();
        if (strcmp(last_synced_code, current_code) != 0) {
            replace_code_with(current_code);
            strncpy(last_synced_code, current_code, 65535);
            last_synced_code[65535] = 0;
        }
        if (search_mode) search_refresh();
        if (file_mode) file_refresh();
        tui_frame();

        if (SLang_input_pending(0)) ch = read_key(); else { usleep(50000); continue; }
        if (ch == SL_KEY_ERR) continue;

        if (search_mode) {
            int nrows = search_ndirs + (search_path[0] ? 1 : 0);
            if (ch == 27 || ch == SL_KEY_F(6)) { search_mode = 0; continue; }
            else if (ch == SL_KEY_ENTER || ch == '\n' || ch == '\r') {
                search_descend_or_use();
                continue;
            } else if (ch == 9) {
                int t = search_row_target();
                if (t >= 0 && t < search_ndirs) {
                    search_use_bank(search_dirs[t]);
                    search_mode = 0;
                }
                continue;
            } else if (ch == SL_KEY_UP) { if (search_sel > 0) search_sel--; continue; }
            else if (ch == SL_KEY_DOWN) {
                if (search_sel < nrows - 1) search_sel++;
                continue;
            } else if (ch == SL_KEY_NPAGE) { search_sel += 10; if (search_sel > nrows - 1) search_sel = nrows - 1; continue; }
            else if (ch == SL_KEY_PPAGE) { search_sel -= 10; if (search_sel < 0) search_sel = 0; continue; }
            else if ((ch == SL_KEY_BACKSPACE || ch == 127 || ch == 8) && search_len > 0) {
                search_len--; search_buf[search_len] = 0; search_sel = 0; search_dirty = 1; continue;
            } else if ((ch == SL_KEY_BACKSPACE || ch == 127 || ch == 8) && search_path[0]) {
                search_ascend();
                continue;
            } else if (ch >= 32 && ch < 127 && search_len < 240) {
                search_buf[search_len++] = (char)ch; search_buf[search_len] = 0;
                search_sel = 0; search_dirty = 1; continue;
            }
            continue;
        }
        if (file_mode) {
            if (ch == 27) { file_mode = 0; continue; }
            else if (ch == SL_KEY_ENTER || ch == '\n' || ch == '\r') {
                file_confirm();
                continue;
            } else if (ch == SL_KEY_UP) { if (file_sel > 0) file_sel--; continue; }
            else if (ch == SL_KEY_DOWN) {
                if (file_sel < file_n - 1) file_sel++;
                continue;
            } else if (ch == SL_KEY_NPAGE) { if (file_n > 0) { file_sel += 10; if (file_sel > file_n - 1) file_sel = file_n - 1; } continue; }
            else if (ch == SL_KEY_PPAGE) { if (file_n > 0) { file_sel -= 10; if (file_sel < 0) file_sel = 0; } continue; }
            else if ((ch == SL_KEY_BACKSPACE || ch == 127 || ch == 8) && file_len > 0) {
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
        if (input_mode == 1) {
            if (ch == SL_KEY_ENTER || ch == '\n' || ch == '\r') {
                input_buf[input_len] = 0;
                if (input_len > 0) do_command_or_ai(input_buf);
                input_len = 0;
                input_buf[0] = 0;
                input_mode = 0;
            } else if (ch == 27) {
                input_mode = 0;
                input_len = 0;
                input_buf[0] = 0;
            } else if ((ch == SL_KEY_BACKSPACE || ch == 127 || ch == 8) && input_len > 0) {
                input_len--;
                input_buf[input_len] = 0;
            } else if (ch >= 32 && ch < 127 && input_len < AI_PROMPT_MAX) {
                input_buf[input_len++] = (char)ch;
                input_buf[input_len] = 0;
            }
            continue;
        }
        if (ch == 17) {
            state_save(); /* persist session for next launch */
            break;
        } else if (ch == 16) {
            if (audio_is_playing()) { audio_toggle_play(0); } else { audio_toggle_play(1); scheduler_start(); }
            continue;
        } else if (ch == SL_KEY_F(5) || ch == 18) {
            do_evaluate();
            continue;
        } else if (ch == SL_KEY_F(2)) {
            input_mode = 1;
            input_len = 0;
            input_buf[0] = 0;
            continue;
        } else if (ch == SL_KEY_F(1)) {
            show_docs = !show_docs;
            docs_scroll = 0;
            snprintf(status_msg, 256, show_docs ? "cheatsheet on" : "cheatsheet off");
            continue;
        } else if (ch == SL_KEY_F(4)) {
            show_state = !show_state;
            snprintf(status_msg, 256, show_state ? "state on" : "state off");
            continue;
        } else if (ch == SL_KEY_F(6)) {
            if (!search_mode) {
                banks_ensure_default();
                search_mode = 1;
                file_mode = 0;
                search_sel = 0;
                search_dirty = 1;
                snprintf(status_msg, 256, "bank search");
            } else search_mode = 0;
            continue;
        } else if (ch == SL_KEY_F(7)) {
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
        } else if (ch == SL_KEY_F(8)) {
            scheduler_song_hold_toggle();
            snprintf(status_msg, 256, scheduler_song_hold() ? "hold on" : "hold off");
            continue;
        } else if (ch == SL_KEY_F(9)) {
            show_tracks = !show_tracks;
            snprintf(status_msg, 256, show_tracks ? "tracks on" : "tracks off");
            continue;
        } else if (ch == SL_KEY_F(10)) {
            split_ratio -= 0.05;
            if (split_ratio < 0.3) split_ratio = 0.3;
            snprintf(status_msg, 256, "split %.0f%%", split_ratio * 100);
            continue;
        } else if (ch == SL_KEY_F(11)) {
            split_ratio += 0.05;
            if (split_ratio > 0.85) split_ratio = 0.85;
            snprintf(status_msg, 256, "split %.0f%%", split_ratio * 100);
            continue;
        } else if (ch == SL_KEY_F(12)) {
            show_viz = !show_viz;
            snprintf(status_msg, 256, show_viz ? "viz on" : "viz off");
            continue;
        } else if (ch == 20) {
            char buf[65536];
            flatten_code(buf, sizeof(buf));
            int idx = scheduler_track_copy(buf);
            if (idx >= 0) {
                snprintf(last_synced_code, sizeof(last_synced_code), "%s", current_code);
                snprintf(status_msg, 256, "track %d/%d", idx + 1, scheduler_song_count());
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
            complete_at_cursor();
            continue;
        } else if (ch == SL_KEY_F(3)) {
            vi_mode = !vi_mode;
            vi_normal = vi_mode;
            vi_pending = 0;
            snprintf(status_msg, 256, vi_mode ? (vi_normal ? "vi normal" : "vi insert") : "insert mode");
            continue;
        } else if (ch == 27) {
            vi_mode = 1;
            vi_normal = 1;
            vi_pending = 0;
            snprintf(status_msg, 256, "vi normal");
            continue;
        } else if (ch == SL_KEY_UP) {
            if (cur_line > 0) { cur_line--; if (cur_col > (int)strlen(code_lines[cur_line])) cur_col = strlen(code_lines[cur_line]); }
            vi_pending = 0;
            continue;
        } else if (ch == SL_KEY_DOWN) {
            if (cur_line < code_line_count - 1) { cur_line++; if (cur_col > (int)strlen(code_lines[cur_line])) cur_col = strlen(code_lines[cur_line]); }
            vi_pending = 0;
            continue;
        } else if (ch == SL_KEY_LEFT) {
            if (cur_col > 0) cur_col--;
            else if (cur_line > 0) { cur_line--; cur_col = strlen(code_lines[cur_line]); }
            vi_pending = 0;
            continue;
        } else if (ch == SL_KEY_RIGHT) {
            if (cur_col < (int)strlen(code_lines[cur_line])) cur_col++;
            else if (cur_line < code_line_count-1) { cur_line++; cur_col = 0; }
            vi_pending = 0;
            continue;
        } else if (ch == SL_KEY_HOME) {
            cur_col = 0;
            vi_pending = 0;
            continue;
        } else if (ch == SL_KEY_END) {
            cur_col = strlen(code_lines[cur_line]);
            vi_pending = 0;
            continue;
        } else if (ch == SL_KEY_NPAGE || ch == SL_KEY_PPAGE) {
            if (show_docs) {
                int mid, h, w, sh, th;
                right_geom(&mid, &h, &w, &sh, &th);
                int page = h - sh - th - 3;
                if (page < 1) page = 1;
                if (ch == SL_KEY_NPAGE) docs_scroll += page;
                else docs_scroll -= page;
                if (docs_scroll < 0) docs_scroll = 0;
                continue;
            }
        } else if (vi_mode && vi_normal) {
            if (ch == 17) break;
            /* vi normal mode */
            if (ch == 'j') { if (cur_line < code_line_count - 1) { cur_line++; if (cur_col > (int)strlen(code_lines[cur_line])) cur_col = strlen(code_lines[cur_line]); } ; snprintf(status_msg, 256, "vi normal"); continue; }
            if (ch == 'k') { if (cur_line > 0) { cur_line--; if (cur_col > (int)strlen(code_lines[cur_line])) cur_col = strlen(code_lines[cur_line]); } ; snprintf(status_msg, 256, "vi normal"); continue; }
            if (ch == 'h') { if (cur_col > 0) cur_col--; vi_pending=0; continue; }
            if (ch == 'l') { if (cur_col < (int)strlen(code_lines[cur_line])) cur_col++; vi_pending=0; continue; }
            if (ch == '0' || ch == '^') { cur_col = 0; vi_pending=0; continue; }
            if (ch == '$') { cur_col = strlen(code_lines[cur_line]); vi_pending=0; continue; }
            if (ch == 'i') { vi_normal = 0; vi_pending=0; snprintf(status_msg, 256, "vi insert"); continue; }
            if (ch == 'a') { if (cur_col < (int)strlen(code_lines[cur_line])) cur_col++; vi_normal=0; vi_pending=0; snprintf(status_msg, 256, "vi insert"); continue; }
            if (ch == 'I') { cur_col = 0; vi_normal=0; vi_pending=0; snprintf(status_msg, 256, "vi insert"); continue; }
            if (ch == 'A') { cur_col = strlen(code_lines[cur_line]); vi_normal=0; vi_pending=0; snprintf(status_msg, 256, "vi insert"); continue; }
            if (ch == 'o') {
                if (code_line_count < MAX_CODE_LINES - 1) {
                    for (int i = code_line_count; i > cur_line + 1; i--) memcpy(code_lines[i], code_lines[i-1], MAX_CODE_LINE_LEN);
                    code_lines[cur_line+1][0] = 0;
                    code_line_count++;
                    cur_line++;
                    cur_col = 0;
                    vi_normal = 0;
                    snprintf(status_msg, 256, "vi insert");
                }
                vi_pending=0; continue;
            }
            if (ch == 'O') {
                if (code_line_count < MAX_CODE_LINES - 1) {
                    for (int i = code_line_count; i > cur_line; i--) memcpy(code_lines[i], code_lines[i-1], MAX_CODE_LINE_LEN);
                    code_lines[cur_line][0] = 0;
                    code_line_count++;
                    cur_col = 0;
                    vi_normal = 0;
                    snprintf(status_msg, 256, "vi insert");
                }
                vi_pending=0; continue;
            }
            if (ch == 'x') {
                char *l = code_lines[cur_line];
                int L = strlen(l);
                if (cur_col < L) memmove(l+cur_col, l+cur_col+1, L-cur_col);
                vi_pending=0; continue;
            }
            if (ch == 'd') {
                if (vi_pending == 'd') {
                    strncpy(vi_yank, code_lines[cur_line], sizeof(vi_yank)-1);
                    vi_yank[sizeof(vi_yank)-1]=0;
                    if (code_line_count > 1) {
                        for (int i = cur_line; i < code_line_count - 1; i++) memcpy(code_lines[i], code_lines[i+1], MAX_CODE_LINE_LEN);
                        code_lines[code_line_count-1][0]=0;
                        code_line_count--;
                        if (cur_line >= code_line_count) cur_line = code_line_count - 1;
                    } else code_lines[cur_line][0]=0;
                    cur_col = 0;
                    if (cur_col > (int)strlen(code_lines[cur_line])) cur_col = strlen(code_lines[cur_line]);
                    vi_pending = 0;
                    snprintf(status_msg, 256, "vi normal: deleted line");
                } else vi_pending = 'd';
                continue;
            }
            if (ch == 'y') {
                if (vi_pending == 'y') {
                    strncpy(vi_yank, code_lines[cur_line], sizeof(vi_yank)-1);
                    vi_yank[sizeof(vi_yank)-1]=0;
                    vi_pending=0;
                    snprintf(status_msg, 256, "vi normal: yanked");
                } else vi_pending='y';
                continue;
            }
            if (ch == 'p') {
                if (vi_yank[0]) {
                    if (code_line_count < MAX_CODE_LINES - 1) {
                        for (int i = code_line_count; i > cur_line + 1; i--) memcpy(code_lines[i], code_lines[i-1], MAX_CODE_LINE_LEN);
                        size_t yl = strlen(vi_yank);
                        if (yl > MAX_CODE_LINE_LEN - 1) yl = MAX_CODE_LINE_LEN - 1;
                        memcpy(code_lines[cur_line+1], vi_yank, yl);
                        code_lines[cur_line+1][yl] = 0;
                        code_line_count++;
                        cur_line++;
                        cur_col = 0;
                    }
                }
                vi_pending=0; continue;
            }
            if (ch == 'P') {
                if (vi_yank[0]) {
                    if (code_line_count < MAX_CODE_LINES - 1) {
                        for (int i = code_line_count; i > cur_line; i--) memcpy(code_lines[i], code_lines[i-1], MAX_CODE_LINE_LEN);
                        size_t yl = strlen(vi_yank);
                        if (yl > MAX_CODE_LINE_LEN - 1) yl = MAX_CODE_LINE_LEN - 1;
                        memcpy(code_lines[cur_line], vi_yank, yl);
                        code_lines[cur_line][yl] = 0;
                        code_line_count++;
                        cur_col = 0;
                    }
                }
                vi_pending=0; continue;
            }
            if (ch == 'G') { cur_line = code_line_count-1; cur_col = 0; vi_pending=0; continue; }
            if (ch == 'g' && vi_pending == 'g') { vi_pending=0; cur_line = 0; cur_col=0; continue; }
            if (ch == 'g') { vi_pending = 'g'; continue; }
            if (ch == '$') { cur_col = strlen(code_lines[cur_line]); vi_pending=0; continue; }
            vi_pending = 0;
            continue;
        }
        if (ch == SL_KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (cur_col > 0) {
                char *l = code_lines[cur_line];
                int L = strlen(l);
                memmove(l+cur_col-1, l+cur_col, L-cur_col+1);
                cur_col--;
            } else if (cur_line > 0) {
                int prevlen = strlen(code_lines[cur_line-1]);
                if (prevlen + (int)strlen(code_lines[cur_line]) < MAX_CODE_LINE_LEN - 1) {
                    size_t need = strlen(code_lines[cur_line]);
                    if (prevlen + (int)need < MAX_CODE_LINE_LEN)
                        memmove(code_lines[cur_line-1]+prevlen, code_lines[cur_line], need+1);
                    for (int i = cur_line; i < code_line_count-1; i++)
                        memcpy(code_lines[i], code_lines[i+1], MAX_CODE_LINE_LEN);
                    code_lines[code_line_count-1][0] = 0;
                    code_line_count--;
                    cur_line--;
                    cur_col = prevlen;
                }
            }
        } else if (ch == SL_KEY_DELETE) {
            char *l = code_lines[cur_line];
            int L = strlen(l);
            if (cur_col < L) {
                memmove(l+cur_col, l+cur_col+1, L-cur_col);
            } else if (cur_line < code_line_count-1) {
                if (strlen(l) + strlen(code_lines[cur_line+1]) < MAX_CODE_LINE_LEN -1) {
                    size_t need = strlen(code_lines[cur_line+1]);
                    if (L + (int)need < MAX_CODE_LINE_LEN)
                        memmove(l+L, code_lines[cur_line+1], need+1);
                    for (int i = cur_line+1; i < code_line_count-1; i++)
                        memcpy(code_lines[i], code_lines[i+1], MAX_CODE_LINE_LEN);
                    code_lines[code_line_count-1][0] = 0;
                    code_line_count--;
                }
            }
        } else if (ch == SL_KEY_ENTER || ch == '\r' || ch == '\n') {
            if (code_line_count < MAX_CODE_LINES - 1) {
                for (int i = code_line_count; i > cur_line + 1; i--)
                    memcpy(code_lines[i], code_lines[i-1], MAX_CODE_LINE_LEN);
                char rest[MAX_CODE_LINE_LEN];
                snprintf(rest, sizeof(rest), "%s", code_lines[cur_line] + cur_col);
                code_lines[cur_line][cur_col] = 0;
                snprintf(code_lines[cur_line+1], MAX_CODE_LINE_LEN, "%s", rest);
                code_line_count++;
                cur_line++;
                cur_col = 0;
            }
        } else if (ch >= 32 && ch < 127) {
            char *l = code_lines[cur_line];
            int L = strlen(l);
            if (L < MAX_CODE_LINE_LEN - 2) {
                memmove(l + cur_col + 1, l + cur_col, L - cur_col + 1);
                l[cur_col] = (char)ch;
                cur_col++;
            }
        }
    }
    tui_shutdown();
}