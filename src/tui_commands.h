/* Native Ex subset. UI thread only; no shell commands or AI fallback. */
static const char *const command_words[]={
    "w","write","wq","x","xit","q","quit","e","edit","u","undo","redo","help","set",
    "evaluate",NULL
};
static const char *const native_command_words[]={"bpm","play","stop","companion","clear","rescan",
    "save","load","song","inline","graphics","theme","scope","wrap","tutor",NULL};
static void command_prompt_open(void) {
    completion_reset();
    input_mode=2; input_len=0; input_buf[0]=0; pane_focus=0;
    snprintf(status_msg,sizeof(status_msg),"command: Tab completes; Esc cancels");
}
static int command_modified(void) {
    char *text=editor_flatten();
    if (!text) return 1;
    int modified=!editor_written || strcmp(text,editor_written);
    free(text); return modified;
}
static void command_quit(void) { state_save(); quit_requested=1; }
static int command_abbrev(const char *word, const char *full, size_t minimum) {
    size_t n=strlen(word);
    return n>=minimum && n<=strlen(full) && !strncmp(word,full,n);
}
static int command_expand_home(char *path, size_t capacity) {
    if (path[0]!='~' || (path[1] && path[1]!='/')) return 1;
    const char *home=getenv("HOME");
    if (!home || !*home) return 0;
    size_t h=strlen(home), n=strlen(path+1);
    if (h>=capacity || n>=capacity-h) return 0;
    memmove(path+h,path+1,n+1); memcpy(path,home,h); return 1;
}
static int command_path(const char *arg, char *path, size_t capacity) {
    size_t n=0; char quote=0;
    if (*arg=='\'' || *arg=='"') quote=*arg++;
    while (*arg) {
        if (quote && *arg==quote && !arg[1]) { arg++; quote=0; break; }
        if (*arg=='\\' && arg[1] && (isspace((unsigned char)arg[1]) || arg[1]=='\\' || arg[1]=='\'' || arg[1]=='"')) arg++;
        if (n+1>=capacity) return 0;
        path[n++]=*arg++;
    }
    path[n]=0;
    return !quote && command_expand_home(path,capacity);
}
static void command_set(char *args) {
    int wrap=editor_wrap, mouse=mouse_enabled, changed=0;
    char *save=NULL;
    for (char *option=strtok_r(args," \t",&save);option;option=strtok_r(NULL," \t",&save)) {
        if (!strcmp(option,"wrap")) { wrap=1; changed=1; }
        else if (!strcmp(option,"nowrap")) { wrap=0; changed=1; }
        else if (!strcmp(option,"mouse")) { mouse=1; changed=1; }
        else if (!strcmp(option,"nomouse")) { mouse=0; changed=1; }
        else if (strcmp(option,"wrap?") && strcmp(option,"mouse?")) {
            snprintf(status_msg,sizeof(status_msg),"E518: Unknown option: %.160s",option); return;
        }
    }
    if (changed) {
        if (terminal_nc && mouse!=mouse_enabled &&
            notcurses_mice_enable(terminal_nc,mouse?(NCMICE_BUTTON_EVENT|NCMICE_DRAG_EVENT):NCMICE_NO_EVENTS)<0) {
            snprintf(status_msg,sizeof(status_msg),"cannot change mouse tracking"); return;
        }
        editor_wrap=wrap; mouse_enabled=mouse; view_top=view_col=0; save_editor_config();
    }
    snprintf(status_msg,sizeof(status_msg),"%s %s",editor_wrap?"wrap":"nowrap",mouse_enabled?"mouse":"nomouse");
}
static void command_execute(const char *text) {
    char copy[sizeof(input_buf)];
    snprintf(copy,sizeof(copy),"%s",text);
    char *cmd=copy;
    while (isspace((unsigned char)*cmd)) cmd++;
    char *args=cmd;
    while (*args && !isspace((unsigned char)*args)) args++;
    if (*args) *args++=0;
    while (isspace((unsigned char)*args)) args++;
    size_t length=strlen(args);
    while (length && isspace((unsigned char)args[length-1])) args[--length]=0;
    if (!*cmd) return;
    length=strlen(cmd);
    int force=length && cmd[length-1]=='!';
    if (force) cmd[--length]=0;
    int numeric=1;
    for (size_t i=0;i<length;i++) if (!isdigit((unsigned char)cmd[i])) numeric=0;
    if ((!strcmp(cmd,"$") || numeric) && !force && !*args) {
        char *end=NULL; errno=0;
        long line=!strcmp(cmd,"$")?code_line_count:strtol(cmd,&end,10);
        if (!length || errno || line<1 || line>code_line_count || (numeric && (!end || *end))) {
            snprintf(status_msg,sizeof(status_msg),"E16: Invalid line number"); return;
        }
        cur_line=(int)line-1; cur_col=vi_first_nonblank(code_lines[cur_line]); sel_active=0;
        snprintf(status_msg,sizeof(status_msg),"line %d",cur_line+1); return;
    }
    int write=command_abbrev(cmd,"write",1) || !strcmp(cmd,"wq");
    int xit=command_abbrev(cmd,"xit",1);
    int edit=command_abbrev(cmd,"edit",1);
    int quit=command_abbrev(cmd,"quit",1);
    if (write || xit || edit || quit) {
        if (quit && *args) { snprintf(status_msg,sizeof(status_msg),"E488: Trailing characters"); return; }
        if ((edit || quit) && !force && command_modified()) {
            snprintf(status_msg,sizeof(status_msg),"E37: Unsaved changes (use ! to discard)"); return;
        }
        if (quit) { command_quit(); return; }
        char path[sizeof(project_path)];
        if (!command_path(args,path,sizeof(path))) { snprintf(status_msg,sizeof(status_msg),"invalid or unfinished filename"); return; }
        if (edit) { (void)project_load(*path?path:NULL,0); return; }
        if (xit && !*path && !command_modified()) { command_quit(); return; }
        struct stat st;
        if (!force && *path && strcmp(path,project_path) && !stat(path,&st)) {
            snprintf(status_msg,sizeof(status_msg),"E13: File exists (use ! to overwrite)"); return;
        }
        if (project_save(*path?path:NULL) && (xit || !strcmp(cmd,"wq"))) command_quit();
        return;
    }
    if (!strcmp(cmd,"tutor")) {
        if (*args) snprintf(status_msg,sizeof(status_msg),"E488: Trailing characters");
        else tutor_open(force);
        return;
    }
    if (force) { snprintf(status_msg,sizeof(status_msg),"E477: No ! allowed"); return; }
    if (command_abbrev(cmd,"set",2)) { command_set(args); return; }
    if (command_abbrev(cmd,"undo",1) || command_abbrev(cmd,"redo",3)) {
        if (*args) { snprintf(status_msg,sizeof(status_msg),"E488: Trailing characters"); return; }
        if (cmd[0]=='u') editor_undo(); else editor_redo(); return;
    }
    if (command_abbrev(cmd,"help",1)) {
        show_docs=1; docs_scroll=0; snprintf(status_msg,sizeof(status_msg),"cheatsheet on"); return;
    }
    if (!strcmp(cmd,"evaluate") || !strcmp(cmd,"eval")) { do_perform(); return; }
    for (int i=0;native_command_words[i];i++) if (!strcmp(cmd,native_command_words[i])) {
        char native[sizeof(input_buf)];
        int n=snprintf(native,sizeof(native),"/%s%s%s",cmd,*args?" ":"",args);
        if (n>0 && (size_t)n<sizeof(native)) do_command_or_ai(native);
        else snprintf(status_msg,sizeof(status_msg),"command is too long");
        return;
    }
    snprintf(status_msg,sizeof(status_msg),"E492: Not an editor command: %.180s",cmd);
}
static void command_file_candidates(const char *prefix) {
    char decoded[COMPLETION_WORD], directory[COMPLETION_WORD], candidate[COMPLETION_WORD];
    char quote=*prefix=='\'' || *prefix=='"'?*prefix:0;
    if (quote) prefix++;
    size_t n=0;
    while (*prefix) {
        if (*prefix=='\\' && prefix[1] && (isspace((unsigned char)prefix[1]) || prefix[1]=='\\' || prefix[1]=='\'' || prefix[1]=='"')) prefix++;
        if (n+1>=sizeof(decoded)) return;
        decoded[n++]=*prefix++;
    }
    decoded[n]=0;
    if (quote && n && decoded[n-1]==quote) decoded[--n]=0;
    if (!command_expand_home(decoded,sizeof(decoded))) return;
    n=strlen(decoded);
    char *slash=strrchr(decoded,'/');
    size_t base=slash?(size_t)(slash-decoded)+1:0;
    if (base) { memcpy(directory,decoded,base); directory[base]=0; }
    else snprintf(directory,sizeof(directory),".");
    DIR *dir=opendir(directory);
    if (!dir) return;
    struct dirent *entry; int visited=0;
    while ((entry=readdir(dir)) && visited++<8192) {
        const char *name=entry->d_name;
        if (!strcmp(name,".") || !strcmp(name,"..") || (name[0]=='.' && decoded[base]!='.') ||
            strncmp(name,decoded+base,n-base)) continue;
        int valid=1;
        for (const unsigned char *p=(const unsigned char *)name;*p;p++) if (*p<32 || *p==127 || *p=='\'' || *p=='"') valid=0;
        if (!valid) continue;
        int count=snprintf(candidate,sizeof(candidate),"%.*s%s",(int)base,decoded,name);
        if (count<0 || (size_t)count>=sizeof(candidate)) continue;
        struct stat st;
        if (stat(candidate,&st)) continue;
        int isdir=S_ISDIR(st.st_mode);
        size_t len=strlen(name);
        if (!isdir && (!S_ISREG(st.st_mode) || len<5 || strcmp(name+len-5,".json"))) continue;
        char formatted[COMPLETION_WORD];
        count=snprintf(formatted,sizeof(formatted),"%s%s%s%s",quote?(quote=='\''?"'":"\""):"",candidate,
                       isdir?"/":"",quote && !isdir?(quote=='\''?"'":"\""):"");
        if (count>0 && (size_t)count<sizeof(formatted)) completion_add(formatted,"");
    }
    closedir(dir);
    for (int i=1;i<completion.count;i++) for (int j=i;j>0 && strcmp(completion.words[j],completion.words[j-1])<0;j--) {
        char tmp[COMPLETION_WORD]; memcpy(tmp,completion.words[j],sizeof(tmp));
        memcpy(completion.words[j],completion.words[j-1],sizeof(tmp)); memcpy(completion.words[j-1],tmp,sizeof(tmp));
    }
}
static int command_complete(int backwards) {
    int base=input_mode==2?0:1;
    if (input_mode!=2 && input_buf[0]!='/' && input_buf[0]!=':') return 0;
    if (completion_current(2)) { completion_choose(backwards,0); return 1; }
    completion_reset(); completion.target=2; completion.index=-1;
    int end=base;
    while (end<input_len && !isspace((unsigned char)input_buf[end])) end++;
    if (end==input_len) {
        completion.start=base; completion.end=input_len; completion.prefix_len=input_len-base;
        completion_list(command_words,input_buf+base);
        completion_list(native_command_words,input_buf+base);
    } else {
        char cmd[64]; int n=end-base;
        if (n<=0 || n>=(int)sizeof(cmd)) return 0;
        memcpy(cmd,input_buf+base,(size_t)n); cmd[n]=0;
        if (n && cmd[n-1]=='!') cmd[n-1]=0;
        int start=end;
        while (start<input_len && isspace((unsigned char)input_buf[start])) start++;
        const char *const *values=NULL;
        static const char *const onoff[]={"on","off",NULL};
        static const char *const themes[]={"off","omarchy","pywal","auto",NULL};
        static const char *const graphics[]={"auto","braille",NULL};
        static const char *const settings[]={"wrap","nowrap","mouse","nomouse","wrap?","mouse?",NULL};
        static const char *const companion[]={"start","stop",NULL};
        static const char *const song[]={"on","off","hold",NULL};
        int file=command_abbrev(cmd,"write",1) || !strcmp(cmd,"wq") || command_abbrev(cmd,"xit",1) ||
            command_abbrev(cmd,"edit",1) || !strcmp(cmd,"save") || !strcmp(cmd,"load");
        if (!strcmp(cmd,"set") || !strcmp(cmd,"se")) {
            for (int i=start;i<input_len;i++) if (isspace((unsigned char)input_buf[i])) start=i+1;
            values=settings;
        } else if (!strcmp(cmd,"theme")) values=themes;
        else if (!strcmp(cmd,"graphics")) values=graphics;
        else if (!strcmp(cmd,"wrap") || !strcmp(cmd,"scope") || !strcmp(cmd,"inline")) values=onoff;
        else if (!strcmp(cmd,"companion")) values=companion;
        else if (!strcmp(cmd,"song")) values=song;
        completion.start=start; completion.end=input_len; completion.prefix_len=input_len-start;
        if (file) command_file_candidates(input_buf+start);
        else if (values) completion_list(values,input_buf+start);
    }
    snprintf(completion.prompt,sizeof(completion.prompt),"%s",input_buf);
    completion_choose(backwards,1); return 1;
}
