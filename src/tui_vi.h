/* Private vi command state: included by tui.c, foreground/UI thread only. */
static struct {
    int count, op_count, op, prefix, last_find, last_char;
    int search_edit, search_dir, search_whole;
    char search[128], draft[128];
} vi;

static void vi_reset(void) {
    vi.count=vi.op_count=vi.op=vi.prefix=vi.search_edit=0;
    vi_pending=0;
}
static int vi_next(const char *s, int at, int n) {
    if (at>=n) return n;
    at++;
    while (at<n && ((unsigned char)s[at]&0xc0)==0x80) at++;
    return at;
}
static int vi_prev(const char *s, int at) {
    if (at<=0) return 0;
    at--;
    while (at>0 && ((unsigned char)s[at]&0xc0)==0x80) at--;
    return at;
}
static int vi_class(int c, int big) {
    unsigned char byte=(unsigned char)c;
    if (!byte || isspace(byte)) return 0;
    return big || byte>=128 || isalnum(byte) || byte=='_'?1:2;
}
static int vi_first_nonblank(const char *s) {
    int at=0;
    while (s[at]==' ' || s[at]=='\t') at++;
    return at;
}
static int vi_line_target(int line, int delta) {
    if (delta>0 && delta>code_line_count-1-line) return code_line_count-1;
    if (delta<0 && line < -delta) return 0;
    return line+delta;
}
static void vi_position(int at) {
    if (at<0) at=0;
    cur_line=0;
    while (cur_line+1<code_line_count && at>(int)strlen(code_lines[cur_line]))
        at-=(int)strlen(code_lines[cur_line++])+1;
    cur_col=at;
    int n=(int)strlen(code_lines[cur_line]);
    if (cur_col>n) cur_col=n;
}
static int vi_word(const char *s, int n, int at, int key, int count) {
    int big=key=='W' || key=='B' || key=='E';
    for (int step=0;step<count;step++) {
        int old=at;
        if (key=='b' || key=='B') {
            at=vi_prev(s,at);
            while (at>0 && !vi_class(s[at],big)) at=vi_prev(s,at);
            int kind=vi_class(s[at],big);
            while (at>0 && vi_class(s[vi_prev(s,at)],big)==kind) at=vi_prev(s,at);
        } else if (key=='e' || key=='E') {
            at=vi_next(s,at,n);
            while (at<n && !vi_class(s[at],big)) at=vi_next(s,at,n);
            int kind=vi_class(s[at],big);
            while (at<n && vi_next(s,at,n)<n && vi_class(s[vi_next(s,at,n)],big)==kind)
                at=vi_next(s,at,n);
        } else {
            int kind=vi_class(s[at],big);
            if (kind) while (at<n && vi_class(s[at],big)==kind) at=vi_next(s,at,n);
            while (at<n && !vi_class(s[at],big)) at=vi_next(s,at,n);
        }
        if (at==old) break;
    }
    return at;
}
static int vi_find(const char *s, int n, int at, int key, int target, int count) {
    int backwards=key=='F' || key=='T', found=at;
    for (int i=0;i<count;i++) {
        int p=found;
        for (;;) {
            if ((backwards && p==0) || (!backwards && p>=n)) return -1;
            p=backwards?vi_prev(s,p):vi_next(s,p,n);
            if (p>=n || s[p]=='\n') return -1;
            if ((unsigned char)s[p]==(unsigned)target) { found=p; break; }
        }
    }
    return key=='t'?vi_prev(s,found):key=='T'?vi_next(s,found,n):found;
}
static int vi_match(const char *s, int n, int at) {
    const char *open="([{",*close=")]}",*p=NULL;
    while (at<n && s[at]!='\n') {
        if (strchr(open,s[at]) || strchr(close,s[at])) break;
        at=vi_next(s,at,n);
    }
    if (at>=n || s[at]=='\n') return -1;
    p=strchr(open,s[at]);
    int direction=p?1:-1, other=p?close[p-open]:open[strchr(close,s[at])-close];
    int original=s[at],depth=1;
    for (int i=at+direction;i>=0 && i<n;i+=direction) {
        if (s[i]==original) depth++;
        if (s[i]==other && !--depth) return i;
        /* Only matching delimiters change depth. */
        if (s[i]!=other) continue;
    }
    return -1;
}
static int vi_search_at(const char *s, int n, int at, int direction, int count) {
    size_t length=strlen(vi.search);
    if (!length) { snprintf(status_msg,sizeof(status_msg),"no previous search"); return -1; }
    for (int repeat=0;repeat<count;repeat++) {
        int found=-1;
        for (int checked=0,p=at;checked<=n;checked++) {
            p+=direction;
            if (p<0) p=n;
            if (p>n) p=0;
            if (((unsigned char)s[p]&0xc0)==0x80 || length>(size_t)(n-p) || memcmp(s+p,vi.search,length)) continue;
            if (vi.search_whole && ((p>0 && vi_class(s[p-1],0)==1) ||
                (p+(int)length<n && vi_class(s[p+length],0)==1))) continue;
            found=p; break;
        }
        if (found<0) { snprintf(status_msg,sizeof(status_msg),"not found: %.120s",vi.search); return -1; }
        at=found;
    }
    return at;
}
static int vi_object(const char *s, int n, int at, int key, int around, int count, int *a, int *b) {
    if (key=='w' || key=='W') {
        if (at>=n) return 0;
        int big=key=='W',kind=vi_class(s[at],big);
        *a=at;
        while (*a>0 && s[vi_prev(s,*a)]!='\n' && vi_class(s[vi_prev(s,*a)],big)==kind) *a=vi_prev(s,*a);
        *b=at;
        while (*b<n && s[*b]!='\n' && vi_class(s[*b],big)==kind) *b=vi_next(s,*b,n);
        if (count>1) *b=vi_next(s,vi_word(s,n,vi_prev(s,*b),big?'E':'e',count-1),n);
        if (around) {
            int old=*b;
            while (*b<n && (s[*b]==' ' || s[*b]=='\t')) (*b)++;
            if (*b==old) while (*a>0 && (s[*a-1]==' ' || s[*a-1]=='\t')) (*a)--;
        }
        return *b>*a;
    }
    if (key=='\'' || key=='"' || key=='`') {
        int start=at,left=-1;
        while (start>0 && s[start-1]!='\n') start--;
        for (int i=start;i<n && s[i]!='\n';i++) if (s[i]==key) {
            int slashes=0;
            for (int j=i-1;j>=start && s[j]=='\\';j--) slashes++;
            if (slashes%2) continue;
            if (left<0) left=i;
            else {
                if (at>=left && at<=i) { *a=left+!around; *b=i+around; return 1; }
                left=-1;
            }
        }
        return 0;
    }
    const char *opens="([{",*closes=")]}",*p=strchr(opens,key);
    int index=p?(int)(p-opens):((p=strchr(closes,key))?(int)(p-closes):-1);
    if (key=='b') index=0;
    if (key=='B') index=2;
    if (index<0) return 0;
    int left=-1,right=-1;
    for (int i=at;i>=0;i--) if (s[i]==opens[index]) {
        int end=vi_match(s,n,i);
        if (end>=at && --count==0) { left=i; right=end; break; }
    }
    if (left<0) return 0;
    *a=left+!around; *b=right+around; return 1;
}
static void vi_apply(int a, int b, int linewise, int op) {
    int end=editor_offset(code_line_count-1,(int)strlen(code_lines[code_line_count-1]));
    if (a<0 || b<a || b>end) return;
    snprintf(status_msg,sizeof(status_msg),"vi %s",op=='c'?"insert":"normal");
    size_t length=(size_t)(b-a);
    char *yank=malloc(length+2);
    if (!yank) { snprintf(status_msg,sizeof(status_msg),"out of memory"); return; }
    editor_copy_range(yank,(size_t)a,length);
    if (linewise && (!length || yank[length-1]!='\n')) yank[length++]='\n';
    yank[length]=0;
    int delete_at=a;
    if (linewise && op=='d' && b==end && a>0) delete_at--;
    const char *replacement=linewise && op=='c' && b<end?"\n":"";
    if (op!='y' && !editor_replace(delete_at,b,replacement)) { free(yank); return; }
    if (vi_yank!=yank_blank) free(vi_yank);
    vi_yank=yank; yank_lines=linewise; sel_active=0;
    if (op=='c') {
        vi_position(a); vi_normal=0;
        undo_group=UNDO_TYPE; undo_group_tick=undo_tick; undo_group_time=now_seconds();
    } else if (op=='d') {
        if (linewise) cur_col=vi_first_nonblank(code_lines[cur_line]);
        int n=(int)strlen(code_lines[cur_line]);
        if (n && cur_col>=n) cur_col=vi_prev(code_lines[cur_line],n);
    }
}
static void vi_finish(const char *s, int n, int origin, int target, int inclusive, int linewise) {
    if (target<0) { vi_reset(); return; }
    if (vi.op) {
        int a=origin<target?origin:target, b=origin>target?origin:target;
        if (linewise) {
            while (a>0 && s[a-1]!='\n') a--;
            while (b<n && s[b]!='\n') b++;
            if (b<n) b++;
        } else if (inclusive) b=vi_next(s,b,n);
        if (a!=b || vi.op=='c') vi_apply(a,b,linewise,vi.op);
    } else {
        snprintf(status_msg,sizeof(status_msg),"vi normal");
        vi_position(target);
        int length=(int)strlen(code_lines[cur_line]);
        if (!sel_active && length && cur_col>=length) cur_col=vi_prev(code_lines[cur_line],length);
    }
    vi_reset();
}

/* Returns 1 for a consumed vi command; application shortcuts keep precedence. */
static int vi_handle_normal(int ch) {
    if (!lines_ready()) return 0;
    if (ch==27) { vi_reset(); sel_active=0; snprintf(status_msg,sizeof(status_msg),"vi normal"); return 1; }
    if (vi.search_edit) {
        if (ch<32 && ch!=8 && ch!='\r' && ch!='\n') { vi_reset(); return 0; }
        size_t length=strlen(vi.draft);
        if (ch=='\r' || ch=='\n') {
            if (length) { snprintf(vi.search,sizeof(vi.search),"%s",vi.draft); vi.search_whole=0; }
            int direction=vi.search_edit; vi.search_dir=direction; vi.search_edit=0;
            char *text=editor_flatten();
            if (text) {
                int n=(int)strlen(text)-1, origin=editor_offset(cur_line,cur_col); text[n]=0;
                vi_finish(text,n,origin,vi_search_at(text,n,origin,direction,vi.count?vi.count:1),0,0); free(text);
            } else vi_reset();
            return 1;
        }
        if (ch==8 || ch==127 || ch==SL_KEY_BACKSPACE) vi.draft[vi_prev(vi.draft,(int)length)]=0;
        else if (ch>=32 && ch<=255 && length<sizeof(vi.draft)-1) { vi.draft[length]=(char)ch; vi.draft[length+1]=0; }
        else if (ch>255) { vi_reset(); return 0; }
        snprintf(status_msg,sizeof(status_msg),"%c%s",vi.search_edit>0?'/':'?',vi.draft);
        return 1;
    }
    if (ch==18 && !vi.prefix && !vi.op) { editor_redo(); vi_reset(); return 1; }
    if (ch<32 || ch>126) { vi_reset(); return 0; }
    if (!vi.prefix && ch>='0' && ch<='9' && (ch!='0' || vi.count)) {
        vi.count=vi.count>999?9999:vi.count*10+ch-'0';
        snprintf(status_msg,sizeof(status_msg),"vi count=%d%s",vi.count,vi.op?" (operator pending)":"");
        return 1;
    }
    int had_count=vi.count || vi.op_count;
    int count=(vi.count?vi.count:1)*(vi.op_count?vi.op_count:1);
    if (count>9999) count=9999;
    if (sel_active && (ch=='d' || ch=='y' || ch=='x')) { vi_reset(); return 0; }
    if (sel_active && ch=='c') { selection_copy(1); vi_normal=0; vi_reset(); return 1; }
    if (!vi.prefix && (ch=='d' || ch=='y' || ch=='c')) {
        if (vi.op!=ch) {
            vi.op=ch; vi.op_count=vi.count; vi.count=0; vi_pending=ch;
            snprintf(status_msg,sizeof(status_msg),"vi %c: motion or text object",ch);
            return 1;
        }
        int first=cur_line,last=vi_line_target(cur_line,count-1);
        int a=editor_offset(first,0), b=last+1<code_line_count?editor_offset(last+1,0):editor_offset(last,(int)strlen(code_lines[last]));
        vi_apply(a,b,1,ch); vi_reset(); return 1;
    }
    if (!vi.prefix && (ch=='/' || ch=='?')) {
        vi.search_edit=ch=='/'?1:-1; vi.draft[0]=0;
        vi.count=count; vi.op_count=0;
        snprintf(status_msg,sizeof(status_msg),"%c (literal search)",ch); return 1;
    }
    if (!vi.prefix && (strchr("fFtTg",ch) || (ch=='r' && !vi.op && !sel_active) ||
        ((vi.op || sel_active) && (ch=='i' || ch=='a')))) {
        vi.prefix=ch; return 1;
    }
    if (!vi.prefix && !vi.op && ch=='u') { while (count-- && undo_n) editor_undo(); vi_reset(); return 1; }
    if (!vi.prefix && !vi.op && (ch=='j' || ch=='k')) {
        editor_move_rows(ch=='j'?count:-count); vi_reset(); snprintf(status_msg,sizeof(status_msg),"vi normal"); return 1;
    }
    if (!vi.prefix && !vi.op && !sel_active && (ch=='p' || ch=='P')) {
        snprintf(status_msg,sizeof(status_msg),"vi normal");
        size_t length=strlen(vi_yank);
        if (length && (size_t)count<=SOURCE_MAX/length) {
            char *repeated=malloc(length*(size_t)count+1);
            if (repeated) {
                for (int i=0;i<count;i++) memcpy(repeated+length*(size_t)i,vi_yank,length);
                repeated[length*(size_t)count]=0;
                char *saved=vi_yank; vi_yank=repeated;
                editor_paste(ch=='P'); vi_yank=saved; free(repeated);
            } else snprintf(status_msg,sizeof(status_msg),"out of memory");
        }
        vi_reset(); return 1;
    }
    if (!vi.prefix && !vi.op && (ch=='i' || ch=='a' || ch=='I' || ch=='A' || ch=='o' || ch=='O' || ch=='p' || ch=='P' || ch=='v' || ch=='V')) {
        vi_reset(); return 0;
    }
    char *text=editor_flatten();
    if (!text) { vi_reset(); return 1; }
    int n=(int)strlen(text)-1; text[n]=0;
    int origin=editor_offset(cur_line,cur_col), target=origin, inclusive=0,linewise=0,handled=1;
    int pending=vi.op || vi.prefix;
    if (vi.prefix=='r') {
        snprintf(status_msg,sizeof(status_msg),"vi normal");
        int end=origin;
        for (int i=0;i<count;i++) {
            if (end>=n || text[end]=='\n') { end=-1; break; }
            end=vi_next(text,end,n);
        }
        if (end>=0) {
            char *replacement=malloc((size_t)count+1);
            if (replacement) {
                memset(replacement,ch,(size_t)count); replacement[count]=0;
                if (editor_replace(origin,end,replacement)) vi_position(origin+count-1);
                free(replacement);
            } else snprintf(status_msg,sizeof(status_msg),"out of memory");
        }
        vi_reset(); free(text); return 1;
    }
    if (vi.prefix=='i' || vi.prefix=='a') {
        int a,b;
        if (vi_object(text,n,origin,ch,vi.prefix=='a',count,&a,&b)) {
            if (vi.op) vi_apply(a,b,0,vi.op);
            else { vi_position(a); selection_start(0); vi_position(b); sel_inclusive=0; }
        } else snprintf(status_msg,sizeof(status_msg),"vi: text object not found");
        vi_reset(); free(text); return 1;
    }
    if (vi.prefix && strchr("fFtT",vi.prefix)) {
        int key=vi.prefix;
        target=ch>=32 && ch<127?vi_find(text,n,origin,key,ch,count):-1;
        if (target>=0) { vi.last_find=key; vi.last_char=ch; }
        inclusive=1;
    } else if (vi.prefix=='g') {
        if (ch=='g') { int line=had_count?count-1:0; if (line>=code_line_count) line=code_line_count-1; target=editor_offset(line,0); linewise=1; }
        else if ((ch=='j' || ch=='k') && !vi.op) { editor_move_rows(ch=='j'?count:-count); vi_reset(); free(text); return 1; }
        else target=-1;
    } else if (strchr("wWbBeE",ch)) {
        int key=vi.op=='c' && (ch=='w' || ch=='W') && vi_class(text[origin],0)?(ch=='w'?'e':'E'):ch;
        target=vi_word(text,n,origin,key,count); inclusive=key=='e' || key=='E';
        if (vi.op=='d' && count==1 && (key=='w' || key=='W')) {
            int end=origin; while (end<n && text[end]!='\n') end++;
            if (target>end && origin<end) target=end;
        }
    } else if (ch=='h' || ch=='l' || ch=='0' || ch=='^' || ch=='$') {
        int start=editor_offset(cur_line,0),end=start+(int)strlen(code_lines[cur_line]);
        if (ch=='0') target=start;
        else if (ch=='^') target=start+vi_first_nonblank(code_lines[cur_line]);
        else if (ch=='$') {
            int last=vi_line_target(cur_line,count-1);
            start=editor_offset(last,0); end=start+(int)strlen(code_lines[last]);
            target=end>start?vi_prev(text,end):start; inclusive=1;
        } else while (count--) {
            int next=ch=='h'?vi_prev(text,target):vi_next(text,target,n);
            if (next<start || next>end || (next==end && !vi.op)) break;
            target=next;
        }
    } else if (ch=='G' || ch=='j' || ch=='k') {
        int line=ch=='G'?(had_count?count-1:code_line_count-1):vi_line_target(cur_line,ch=='j'?count:-count);
        if (line<0) line=0;
        if (line>=code_line_count) line=code_line_count-1;
        target=editor_offset(line,0); linewise=1;
    } else if (ch=='{' || ch=='}') {
        int line=cur_line,dir=ch=='}'?1:-1;
        while (count--) {
            if (line+dir<0 || line+dir>=code_line_count) break;
            line+=dir;
            while (line+dir>=0 && line+dir<code_line_count && !code_lines[line][vi_first_nonblank(code_lines[line])]) line+=dir;
            while (line+dir>=0 && line+dir<code_line_count && code_lines[line][vi_first_nonblank(code_lines[line])]) line+=dir;
        }
        target=editor_offset(line,0);
    } else if (ch=='%') { target=vi_match(text,n,origin); inclusive=1; }
    else if (ch==';' || ch==',') {
        int key=vi.last_find;
        if (ch==',') { if (key=='f') key='F'; else if (key=='F') key='f'; else if (key=='t') key='T'; else if (key=='T') key='t'; }
        int start=origin;
        if (key=='t') start=vi_next(text,start,n);
        if (key=='T') start=vi_prev(text,start);
        target=key?vi_find(text,n,start,key,vi.last_char,count):-1; inclusive=1;
    } else if (ch=='n' || ch=='N' || ch=='*' || ch=='#') {
        int direction=vi.search_dir?vi.search_dir:1;
        if (ch=='N') direction=-direction;
        if (ch=='*' || ch=='#') {
            int a,b;
            if (vi_class(text[origin],0)!=1 || !vi_object(text,n,origin,'w',0,1,&a,&b) || b-a>127) target=-1;
            else { snprintf(vi.search,sizeof(vi.search),"%.*s",b-a,text+a); vi.search_whole=1; direction=ch=='*'?1:-1; vi.search_dir=direction; }
        }
        if (target>=0) target=vi_search_at(text,n,origin,direction,count);
    } else if (!vi.op && (ch=='D' || ch=='C' || ch=='Y')) {
        vi.op=ch=='D'?'d':ch=='C'?'c':'y';
        int last=vi_line_target(cur_line,count-1);
        target=editor_offset(last,(int)strlen(code_lines[last]));
        if (ch=='Y') { target=editor_offset(last,0); linewise=1; }
    } else if (!vi.op && (ch=='x' || ch=='X' || ch=='s')) {
        int start=editor_offset(cur_line,0),end=start+(int)strlen(code_lines[cur_line]);
        while (count--) {
            int next=ch=='X'?vi_prev(text,target):vi_next(text,target,n);
            if (next<start || next>end) break;
            target=next;
        }
        vi.op=ch=='s'?'c':'d';
    } else handled=0;
    if (handled) vi_finish(text,n,origin,target,inclusive,linewise);
    else vi_reset();
    free(text); return handled || pending;
}
