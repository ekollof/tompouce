/* Notcurses terminal adapter. The editor keeps logical color roles and byte
 * offsets; all backing cells, Unicode layout and screen diffs live in ncplane.
 * Legacy draw names below keep the renderer independent of the backend API. */
#ifndef TOMPOUCE_TUI_TERMINAL_H
#define TOMPOUCE_TUI_TERMINAL_H
#include <notcurses/notcurses.h>
#include <locale.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

static struct notcurses *terminal_nc;
static struct ncplane *terminal_plane;
static FILE *terminal_output;
static int terminal_event_pipe[2]={-1,-1};
static pthread_t terminal_input_thread;
static int terminal_input_thread_active;
/* Notcurses 3.x returns zero for both a timed read and a literal NUL, even
 * through getvec(). A blocking reader preserves Ctrl+Space unambiguously.
 * Fixed-size atomic pipe messages provide bounded backpressure and pollable
 * readiness without blocking the UI or sharing its state with this thread. */
static void *terminal_input_pump(void *unused) {
    (void)unused;
    for (;;) {
        ncinput event={0};
        event.id=notcurses_get_blocking(terminal_nc,&event);
        ssize_t sent;
        do { sent=write(terminal_event_pipe[1],&event,sizeof(event)); } while (sent<0 && errno==EINTR);
        if (sent!=(ssize_t)sizeof(event) || event.id==UINT32_MAX || event.id==NCKEY_EOF) break;
    }
    return NULL;
}
static int terminal_input_fd(void) { return terminal_event_pipe[0]; }
static int SLtt_Screen_Rows = 24, SLtt_Screen_Cols = 80;
static int terminal_y, terminal_x;
static int terminal_last_y=-1, terminal_last_x=-1;
static uint64_t terminal_colors[256];
static uint64_t terminal_inverse;
static unsigned terminal_styles[256];
static uint32_t terminal_ansi[16]={0x000000,0x800000,0x008000,0x808000,
    0x000080,0x800080,0x008080,0xc0c0c0,0x808080,0xff0000,0x00ff00,
    0xffff00,0x0000ff,0xff00ff,0x00ffff,0xffffff};
static int terminal_color;
static int terminal_reverse;
typedef uint32_t SLwchar_Type;
typedef unsigned SLtt_Char_Type;
typedef struct { uint32_t wchars[2]; int color, nchars; } SLsmg_Char_Type;
#define SLTT_BOLD_MASK NCSTYLE_BOLD
#define SLTT_ULINE_MASK NCSTYLE_UNDERLINE
#define SLTT_ITALIC_MASK NCSTYLE_ITALIC
#define SL_KEY_ERR (-1)
#define SL_KEY_UP ((int)NCKEY_UP)
#define SL_KEY_DOWN ((int)NCKEY_DOWN)
#define SL_KEY_LEFT ((int)NCKEY_LEFT)
#define SL_KEY_RIGHT ((int)NCKEY_RIGHT)
#define SL_KEY_HOME ((int)NCKEY_HOME)
#define SL_KEY_END ((int)NCKEY_END)
#define SL_KEY_BACKSPACE ((int)NCKEY_BACKSPACE)
#define SL_KEY_DELETE ((int)NCKEY_DEL)
#define SL_KEY_ENTER ((int)NCKEY_ENTER)
#define SL_KEY_NPAGE ((int)NCKEY_PGDOWN)
#define SL_KEY_PPAGE ((int)NCKEY_PGUP)
#define SL_KEY_F(n) ((int)NCKEY_F00+(n))

static void terminal_dimensions(void) {
    if (!terminal_nc) return;
    unsigned y,x;
    ncplane_dim_yx(notcurses_stdplane(terminal_nc),&y,&x);
    SLtt_Screen_Rows=(int)y; SLtt_Screen_Cols=(int)x;
}
static int SLsmg_init_smg(void) {
    notcurses_options opts={.loglevel=NCLOGLEVEL_SILENT,
        .flags=NCOPTION_SUPPRESS_BANNERS|NCOPTION_NO_QUIT_SIGHANDLERS|
               NCOPTION_NO_WINCH_SIGHANDLER|NCOPTION_NO_CLEAR_BITMAPS};
    /* stdin/stdout commonly share one open-file description. Notcurses makes
       stdin nonblocking; a separately opened output tty keeps large RGBA and
       screen writes blocking without changing its input thread's flags. */
    char path[4096];
    if (ttyname_r(fileno(stdout),path,sizeof(path))) return -1;
    int output_fd=open(path,O_WRONLY|O_NOCTTY|O_CLOEXEC);
    if (output_fd<0) return -1;
    terminal_output=fdopen(output_fd,"w");
    if (!terminal_output) { close(output_fd); return -1; }
    terminal_nc=notcurses_core_init(&opts,terminal_output);
    if (!terminal_nc) { fclose(terminal_output); terminal_output=stdout; return -1; }
    if (pipe(terminal_event_pipe)<0 ||
        fcntl(terminal_event_pipe[0],F_SETFD,FD_CLOEXEC)<0 ||
        fcntl(terminal_event_pipe[1],F_SETFD,FD_CLOEXEC)<0 ||
        fcntl(terminal_event_pipe[0],F_SETFL,O_NONBLOCK)<0 ||
        pthread_create(&terminal_input_thread,NULL,terminal_input_pump,NULL)) {
        if (terminal_event_pipe[0]>=0) close(terminal_event_pipe[0]);
        if (terminal_event_pipe[1]>=0) close(terminal_event_pipe[1]);
        terminal_event_pipe[0]=terminal_event_pipe[1]=-1;
        notcurses_stop(terminal_nc); terminal_nc=NULL;
        fclose(terminal_output); terminal_output=stdout; return -1;
    }
    terminal_input_thread_active=1;
    terminal_plane=notcurses_stdplane(terminal_nc);
    ncpalette *palette=ncpalette_new(terminal_nc);
    if (palette) {
        for (int i=0;i<16;i++) {
            uint32_t rgb; ncpalette_get(palette,i,&rgb);
            if (rgb || !i) terminal_ansi[i]=rgb;
        }
        ncpalette_free(palette);
    }
    uint32_t foreground=terminal_ansi[7], background=terminal_ansi[0];
    notcurses_default_foreground(terminal_nc,&foreground);
    notcurses_default_background(terminal_nc,&background);
    ncchannels_set_fg_rgb(&terminal_colors[0],foreground);
    ncchannels_set_fg_rgb(&terminal_inverse,background);
    ncchannels_set_bg_rgb(&terminal_inverse,foreground);
    terminal_dimensions();
    return 0;
}
static void SLsmg_reset_smg(void) {
    if (terminal_input_thread_active) {
        pthread_cancel(terminal_input_thread); pthread_join(terminal_input_thread,NULL);
        terminal_input_thread_active=0;
    }
    if (terminal_event_pipe[0]>=0) close(terminal_event_pipe[0]);
    if (terminal_event_pipe[1]>=0) close(terminal_event_pipe[1]);
    terminal_event_pipe[0]=terminal_event_pipe[1]=-1;
    if (terminal_nc) notcurses_stop(terminal_nc);
    if (terminal_output && terminal_output!=stdout) fclose(terminal_output);
    terminal_output=stdout;
    terminal_nc=NULL; terminal_plane=NULL;
}
static void SLsmg_gotorc(int y,int x) {
    terminal_y=y; terminal_x=x; terminal_last_y=terminal_last_x=-1;
}
static void terminal_apply_style(void) {
    uint64_t ch=terminal_colors[terminal_color];
    if (terminal_reverse) ch=terminal_inverse;
    ncplane_set_channels(terminal_plane,ch);
    ncplane_set_styles(terminal_plane,terminal_styles[terminal_color]);
}
static void SLsmg_set_color(int color) {
    terminal_color=color >= 0 && color < 256 ? color : 0;
    terminal_reverse=0;
}
static void SLsmg_normal_video(void) { terminal_color=terminal_reverse=0; }
static void SLsmg_reverse_video(void) { terminal_color=0; terminal_reverse=1; }
static void terminal_channel(uint64_t *ch,const char *name,int fg) {
    static const char *names[]={"black","red","green","brown","blue","magenta","cyan","gray",
        "brightblack","brightred","brightgreen","yellow","brightblue","brightmagenta","brightcyan","white"};
    /* Syntax roles use the terminal background, including its transparency. */
    if (!fg && !strcmp(name,"black")) return;
    if (name[0]=='#') {
        unsigned rgb=(unsigned)strtoul(name+1,NULL,16);
        if (fg) ncchannels_set_fg_rgb(ch,rgb); else ncchannels_set_bg_rgb(ch,rgb);
    } else {
        for (unsigned i=0;i<16;i++) if (!strcmp(name,names[i])) {
            /* RGB channels avoid 3.x palette-index rendition cache errors
               when switching between inverse cursor cells and syntax ink. */
            if (fg) ncchannels_set_fg_rgb(ch,terminal_ansi[i]); else ncchannels_set_bg_rgb(ch,terminal_ansi[i]);
            return;
        }
    }
}
static void SLtt_set_color(int role,const char *unused,const char *fg,const char *bg) {
    (void)unused;
    if (role<0 || role>=256) return;
    uint64_t ch=0;
    terminal_channel(&ch,fg,1); terminal_channel(&ch,bg,0);
    terminal_colors[role]=ch; terminal_styles[role]=0;
}
static void SLtt_add_color_attribute(int role,unsigned attributes) {
    if (role>=0 && role<256) terminal_styles[role]|=attributes;
}
static void SLsmg_write_nchars(const char *s,unsigned n) {
    if (!terminal_plane) return;
    terminal_apply_style();
    unsigned height,width; ncplane_dim_yx(terminal_plane,&height,&width);
    const char *end=s+n;
    while (s<end && *s) {
        /* ncplane_putegc consumes exactly one complete grapheme. The caller
         * passes whole UTF-8 glyphs; bound it with a temporary terminated copy. */
        char glyph[128]; size_t len=0;
        mbstate_t state={0}; wchar_t cp='?';
        size_t take=mbrtowc(&cp,s,(size_t)(end-s),&state);
        if (take==(size_t)-1 || take==(size_t)-2 || take==0) take=1;
        if (take>=sizeof(glyph)) break;
        memcpy(glyph,s,take); len=take; glyph[len]=0;
        int span=wcwidth(cp); if (span<0) span=1;
        if (!span && terminal_last_y==terminal_y && terminal_last_x>=0) {
            char *previous=ncplane_at_yx(terminal_plane,terminal_last_y,terminal_last_x,NULL,NULL);
            if (previous) {
                char combined[256];
                size_t prior_len=strlen(previous);
                if (prior_len+len<sizeof(combined)) {
                    memcpy(combined,previous,prior_len);
                    memcpy(combined+prior_len,glyph,len+1);
                    ncplane_putegc_yx(terminal_plane,terminal_last_y,terminal_last_x,combined,NULL);
                }
                free(previous);
            }
        } else if (terminal_y>=0 && terminal_y<(int)height && terminal_x>=0 && terminal_x+span<=(int)width) {
            ncplane_putegc_yx(terminal_plane,terminal_y,terminal_x,glyph,NULL);
            terminal_last_y=terminal_y; terminal_last_x=terminal_x;
        }
        terminal_x+=span; s+=take;
    }
}
static void SLsmg_write_nstring(const char *s,unsigned n) {
    size_t len=strnlen(s,n); SLsmg_write_nchars(s,(unsigned)len);
    while (len++<n) SLsmg_write_nchars(" ",1);
}
static void SLsmg_write_string(const char *s) { SLsmg_write_nchars(s,(unsigned)strlen(s)); }
static void SLsmg_write_char(uint32_t cp) {
    char glyph[5]={0}; mbstate_t state={0};
    size_t n=wcrtomb(glyph,(wchar_t)cp,&state);
    if (n!=(size_t)-1) SLsmg_write_nchars(glyph,(unsigned)n);
}
static void SLsmg_cls(void) { if (terminal_plane) ncplane_erase(terminal_plane); }
static void SLsmg_draw_box(int y,int x,int h,int w) {
    if (h<2 || w<2) return;
    SLsmg_gotorc(y,x); SLsmg_write_string("┌");
    for (int c=1;c<w-1;c++) SLsmg_write_string("─");
    SLsmg_write_string("┐");
    for (int r=1;r<h-1;r++) {
        SLsmg_gotorc(y+r,x); SLsmg_write_string("│");
        SLsmg_gotorc(y+r,x+w-1); SLsmg_write_string("│");
    }
    SLsmg_gotorc(y+h-1,x); SLsmg_write_string("└");
    for (int c=1;c<w-1;c++) SLsmg_write_string("─");
    SLsmg_write_string("┘");
}
static void SLsmg_refresh(void) { if (terminal_nc) notcurses_render(terminal_nc); }
static void SLtt_goto_rc(int y,int x) { if (terminal_nc) notcurses_cursor_enable(terminal_nc,y,x); }
static void SLtt_get_terminfo(void) { setlocale(LC_ALL,""); }
static void SLtt_get_screen_size(void) { terminal_dimensions(); }
static void SLutf8_enable(int on) { (void)on; setlocale(LC_ALL,""); }
static void SLsmg_utf8_enable(int on) { (void)on; }
static int SLsmg_is_utf8_mode(void) { return MB_CUR_MAX>1; }
static int terminal_input[256], terminal_input_count;
static int terminal_input_closed;
static int terminal_mouse_down;
static void SLang_ungetkey(unsigned int key) {
    if (terminal_input_count<256) terminal_input[terminal_input_count++]=(int)key;
}
static void terminal_push_bytes(const char *s) {
    for (size_t i=strlen(s);i>0;i--) SLang_ungetkey((unsigned char)s[i-1]);
}
static int terminal_event(void) {
    ncinput ni={0};
    ssize_t count=read(terminal_input_fd(),&ni,sizeof(ni));
    if (count!=(ssize_t)sizeof(ni)) {
        if (!count || (count<0 && errno!=EAGAIN && errno!=EINTR)) terminal_input_closed=1;
        return -1;
    }
    uint32_t key=ni.id;
    if (key==UINT32_MAX || key==NCKEY_EOF) { terminal_input_closed=1; return -1; }
    if (key==NCKEY_RESIZE || key==NCKEY_SIGNAL) return -1;
    if (nckey_mouse_p(key)) {
        unsigned b;
        if (key==NCKEY_BUTTON4 || key==NCKEY_BUTTON5) b=64+(key==NCKEY_BUTTON5);
        else if (key==NCKEY_BUTTON1) {
            b=terminal_mouse_down && ni.evtype!=NCTYPE_RELEASE ? 32 : 0;
            terminal_mouse_down=ni.evtype!=NCTYPE_RELEASE;
        }
        else if (key==NCKEY_MOTION && terminal_mouse_down) b=32;
        else return -1;
        if (ni.evtype==NCTYPE_REPEAT && b==0) b=32;
        char report[64];
        snprintf(report,sizeof(report),"\033[<%u;%d;%d%c",b,ni.x+1,ni.y+1,
                 ni.evtype==NCTYPE_RELEASE ? 'm' : 'M');
        terminal_push_bytes(report);
        return terminal_input[--terminal_input_count];
    }
    if (ni.evtype==NCTYPE_RELEASE) return -1;
    /* 3.0.x legacy escapes set the deprecated booleans without modifiers. */
    if (ncinput_ctrl_p(&ni) || ni.ctrl) {
        if (key==NCKEY_LEFT) return 2001;
        if (key==NCKEY_RIGHT) return 2002;
        if (key>='a' && key<='z') return (int)key-'a'+1;
        if (key>='A' && key<='Z') return (int)key-'A'+1;
        if (key==' ' || key=='@') return 0;
    }
    if (ncinput_shift_p(&ni) || ni.shift) {
        if (key=='\t') return 2102;
        static const uint32_t shifted[]={NCKEY_UP,NCKEY_DOWN,NCKEY_LEFT,NCKEY_RIGHT,NCKEY_HOME,NCKEY_END};
        for (int i=0;i<6;i++) if (key==shifted[i]) return 2110+i;
    }
    if (key>=PRETERUNICODEBASE) return (int)key;
    /* Kitty reports the physical key separately from the text produced by
       Shift, the keyboard layout, or composition. utf8 encodes only id. */
    char bytes[NCINPUT_MAX_EFF_TEXT_CODEPOINTS*4+1]={0};
    unsigned text_count=0;
    while (text_count<NCINPUT_MAX_EFF_TEXT_CODEPOINTS && ni.eff_text[text_count]) text_count++;
    if (text_count) {
        if (notcurses_ucs32_to_utf8(ni.eff_text,text_count,(unsigned char *)bytes,sizeof(bytes)-1)<0) return -1;
    }
    else if (ni.utf8[0]) snprintf(bytes,sizeof(bytes),"%s",ni.utf8);
    else if (key<128) { bytes[0]=(char)key; bytes[1]=0; }
    terminal_push_bytes(bytes);
    if (ncinput_alt_p(&ni) || ni.alt) return 27;
    if (terminal_input_count) return terminal_input[--terminal_input_count];
    return (int)key;
}
static int SLang_input_pending(int tenth) {
    if (terminal_input_count) return 1;
    int key=terminal_event();
    if (key>=0) { SLang_ungetkey((unsigned)key); return 1; }
    if (tenth) {
        struct pollfd pfd={.fd=terminal_input_fd(),.events=POLLIN};
        if (poll(&pfd,1,tenth*100)>0) {
            key=terminal_event();
            if (key>=0) { SLang_ungetkey((unsigned)key); return 1; }
        }
    }
    return 0;
}
static unsigned int SLang_getkey(void) {
    if (!terminal_input_count && !SLang_input_pending(1)) return 0xffff;
    return (unsigned)terminal_input[--terminal_input_count];
}
typedef struct { const char *sequence; int key; } TerminalKey;
static TerminalKey terminal_keys[64];
static int terminal_key_count;
static int SLkp_init(void) { return 0; }
static void SLkp_define_keysym(const char *sequence,int key) {
    if (terminal_key_count<64) terminal_keys[terminal_key_count++]=(TerminalKey){sequence,key};
}
static int SLkp_getkey(void) {
    int first=(int)SLang_getkey();
    if (first!=27) return first;
    char sequence[32]={27,0}; int n=1;
    while (n<(int)sizeof(sequence)-1) {
        int prefix=0;
        for (int i=0;i<terminal_key_count;i++) {
            if (!strncmp(terminal_keys[i].sequence,sequence,(size_t)n)) {
                if (!terminal_keys[i].sequence[n]) return terminal_keys[i].key;
                prefix=1;
            }
        }
        if (!prefix || !SLang_input_pending(1)) break;
        int key=(int)SLang_getkey();
        if (key>255) { SLang_ungetkey((unsigned)key); break; }
        sequence[n++]=(char)key; sequence[n]=0;
    }
    for (int i=n-1;i>0;i--) SLang_ungetkey((unsigned char)sequence[i]);
    return 27;
}
static int __attribute__((unused)) SLsmg_char_at(SLsmg_Char_Type *cell) {
    uint16_t style; uint64_t channels;
    char *egc=ncplane_at_yx(terminal_plane,terminal_y,terminal_x,&style,&channels);
    if (!egc) return -1;
    mbstate_t state={0}; wchar_t cp=0;
    mbrtowc(&cp,egc,strlen(egc),&state); free(egc);
    cell->wchars[0]=(uint32_t)cp; cell->wchars[1]=0; cell->color=0; cell->nchars=cp ? 1 : 0;
    for (int i=1;i<256;i++) if (channels==terminal_colors[i] && style==terminal_styles[i]) { cell->color=i; break; }
    return 0;
}
#endif
