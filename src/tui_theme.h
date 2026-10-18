/* Foreground-only, bounded theme polling. Reopen paths to follow atomic writes
 * and Omarchy's current/theme symlink without retaining stale file handles. */
static const char *theme_modes[] = {"off", "omarchy", "pywal", "auto"};
static int theme_mode;
static unsigned theme_revision;
static double theme_next_poll;
static uint32_t theme_start_ansi[16], theme_start_fg, theme_start_bg;
static int theme_ready;

static int theme_hex(const char *s, uint32_t *rgb) {
    if (!s || strlen(s)!=7 || *s!='#') return 0;
    uint32_t n=0;
    for (int i=1;i<7;i++) {
        unsigned char c=(unsigned char)s[i];
        if (!isxdigit(c)) return 0;
        n=(n<<4)+(isdigit(c)?c-'0':tolower(c)-'a'+10);
    }
    *rgb=n; return 1;
}

static int theme_parse(char *text, int wal, uint32_t colors[18]) {
    if (wal) {
        cJSON *root=cJSON_ParseWithOpts(text,NULL,1);
        cJSON *special=cJSON_GetObjectItemCaseSensitive(root,"special");
        cJSON *palette=cJSON_GetObjectItemCaseSensitive(root,"colors");
        int ok=1;
        for (int i=0;i<18;i++) {
            char key[32]; snprintf(key,sizeof(key),"color%d",i);
            cJSON *v=cJSON_GetObjectItemCaseSensitive(i<16?palette:special,
                i<16?key:i==16?"foreground":"background");
            if (!cJSON_IsString(v) || !theme_hex(v->valuestring,&colors[i])) ok=0;
        }
        cJSON_Delete(root); return ok;
    }
    const char *names[]={"background","red","green","yellow","blue","magenta","cyan","foreground",
        "dark_foreground","bright_red","bright_green","bright_yellow","bright_blue","bright_magenta","bright_cyan","bright_foreground",
        "foreground","background"};
    unsigned seen=0;
    char *save=NULL;
    for (char *line=strtok_r(text,"\n",&save);line;line=strtok_r(NULL,"\n",&save)) {
        char key[64], value[64]; int end=0;
        if (sscanf(line," %63[a-zA-Z0-9_] = \"%63[^\"]\" %n",key,value,&end)<2 || !end) continue;
        if (line[end] && line[end]!='#' && line[end]!='\r') return 0;
        for (int i=0;i<18;i++) {
            char indexed[32]; snprintf(indexed,sizeof(indexed),"color%d",i);
            if (!strcmp(key,names[i]) || (i<16 && !strcmp(key,indexed))) {
                if (!theme_hex(value,&colors[i])) return 0;
                seen|=1u<<i;
            }
        }
    }
    return seen==((1u<<18)-1);
}

static int theme_read_location(int location, uint32_t colors[18]) {
    char path[2048];
    int wal=location==1;
    const char *base=getenv(wal?"XDG_CACHE_HOME":location==2?"XDG_STATE_HOME":"XDG_CONFIG_HOME");
    const char *home=getenv("HOME");
    int n;
    if (base && *base) n=snprintf(path,sizeof(path),"%s/%s",base,wal?"wal/colors.json":"omarchy/current/theme/colors.toml");
    else if (home && *home) n=snprintf(path,sizeof(path),"%s/%s",home,wal?".cache/wal/colors.json":location==2?".local/state/omarchy/current/theme/colors.toml":".config/omarchy/current/theme/colors.toml");
    else return 0;
    if (n<0 || (size_t)n>=sizeof(path)) return 0;
    FILE *f=fopen(path,"r"); if (!f) return 0;
    char text[16385]; size_t len=fread(text,1,sizeof(text)-1,f);
    int ok=!ferror(f) && feof(f); fclose(f);
    text[len]=0;
    return ok && theme_parse(text,wal,colors);
}

static int theme_read(int wal, uint32_t colors[18]) {
    return wal?theme_read_location(1,colors):
        (theme_read_location(2,colors) || theme_read_location(0,colors));
}

static void theme_roles(void) {
    const char *names[]={"yellow","brightcyan","brightmagenta","brightblue","gray","brown","brightgreen","red"};
    for (int i=0;i<8;i++) SLtt_set_color(i+2,NULL,names[i],"black");
    SLtt_set_color(10,NULL,"black","yellow");
    SLtt_set_color(11,NULL,"black","brightcyan");
}

static void theme_apply(const uint32_t colors[18]) {
    memcpy(terminal_ansi,colors,sizeof(terminal_ansi));
    terminal_colors[0]=terminal_inverse=0;
    ncchannels_set_fg_rgb(&terminal_colors[0],colors[16]);
    ncchannels_set_fg_rgb(&terminal_inverse,colors[17]);
    ncchannels_set_bg_rgb(&terminal_inverse,colors[16]);
    theme_roles();
    char reply[48];
    for (int i=0;i<18;i++) {
        if (i<16) snprintf(reply,sizeof(reply),"4;%d;#%06x",i,colors[i]);
        else snprintf(reply,sizeof(reply),"%d;#%06x",i==16?10:11,colors[i]);
        terminal_graphics_palette_reply(reply);
    }
    theme_revision++; graphics_redraw=1;
}

static void theme_poll(int force) {
    if (!theme_ready) return;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    double now=ts.tv_sec+ts.tv_nsec/1e9;
    if (!force && (theme_mode==0 || now<theme_next_poll)) return;
    theme_next_poll=now+1;
    uint32_t colors[18];
    if (!theme_mode) {
        memcpy(colors,theme_start_ansi,sizeof(theme_start_ansi));
        colors[16]=theme_start_fg; colors[17]=theme_start_bg;
    } else if (!(theme_mode==2?theme_read(1,colors):theme_read(0,colors)) &&
               !(theme_mode==3 && theme_read(1,colors))) return;
    uint32_t fg=ncchannels_fg_rgb(terminal_colors[0]);
    uint32_t bg=ncchannels_fg_rgb(terminal_inverse);
    if (memcmp(colors,terminal_ansi,sizeof(terminal_ansi)) || fg!=colors[16] || bg!=colors[17]) theme_apply(colors);
}

static void theme_init(void) {
    memcpy(theme_start_ansi,terminal_ansi,sizeof(theme_start_ansi));
    theme_start_fg=ncchannels_fg_rgb(terminal_colors[0]);
    theme_start_bg=ncchannels_fg_rgb(terminal_inverse);
    theme_ready=1; theme_poll(1);
}
