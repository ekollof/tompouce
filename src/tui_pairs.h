/* Foreground editor pairing. Pasted/programmatic text is never interpreted. */
static int editor_pair_close(int ch) {
    switch(ch) {
    case '(':return ')';case '[':return ']';case '{':return '}';
    case '\'':case '"':case '`':return ch;
    default:return 0;
    }
}

static void editor_pair_mark(int at,int close) {
    if(editor_pair_count==EDITOR_PAIR_LIMIT) {
        memmove(editor_pairs,editor_pairs+1,(EDITOR_PAIR_LIMIT-1)*sizeof(*editor_pairs));
        editor_pair_count--;
    }
    editor_pairs[editor_pair_count].at=at;
    editor_pairs[editor_pair_count++].close=(unsigned char)close;
}

static int editor_pair_splice(int col,int remove,const char *text,int length) {
    int at=editor_offset(cur_line,col);
    if(at<0 || at>INT_MAX-length || remove>INT_MAX-at) {
        snprintf(status_msg,sizeof(status_msg),"edit exceeds the 2 GiB offset limit");return 0;
    }
    editor_pair_editing++;undo_begin(UNDO_TYPE);
    int ok=line_splice(cur_line,col,remove,text,length);
    if(ok) {editor_pairs_map(at,at+remove,length);undo_commit();}
    else {undo_abort();snprintf(status_msg,sizeof(status_msg),"out of memory");}
    editor_pair_editing--;return ok;
}

static void editor_pair_position(int at,int *line,int *col) {
    for(int i=0;i<code_line_count;i++) {
        int length=(int)strlen(code_lines[i]);
        if(at<=length || i==code_line_count-1) {*line=i;*col=at<length?at:length;return;}
        at-=length+1;
    }
}

#include "tui_indent.h"

static int editor_pair_wrap(int ch,int close) {
    int a,b;selection_bounds(&a,&b);
    if(a<0 || b<a) return 1;
    if(a==b) {sel_active=0;return 0;}
    size_t length=(size_t)(b-a);
    if(length>SOURCE_MAX-2 || (size_t)a>SOURCE_MAX-length-2) {
        snprintf(status_msg,sizeof(status_msg),"edit exceeds the 2 GiB offset limit");return 1;
    }
    char *text=malloc(length+3);
    if(!text) {snprintf(status_msg,sizeof(status_msg),"out of memory");return 1;}
    text[0]=(char)ch;editor_copy_range(text+1,(size_t)a,length);
    text[length+1]=(char)close;text[length+2]=0;
    int reverse=editor_offset(cur_line,cur_col)<editor_offset(sel_line,sel_col);
    if(editor_replace(a,b,text)) {
        editor_pair_position(reverse?a+1:b+1,&cur_line,&cur_col);
        editor_pair_position(reverse?b+1:a+1,&sel_line,&sel_col);
        sel_active=1;sel_linewise=sel_inclusive=0;
        editor_pair_mark(b+1,close);
    }
    free(text);return 1;
}

static int editor_pair_join(int previous) {
    int at=editor_offset(cur_line,cur_col),line=cur_line-previous;
    if(at<0 || line<0 || line+1>=code_line_count) return 1;
    if(!previous && at==INT_MAX) return 1;
    int column=previous?(int)strlen(code_lines[line]):cur_col;
    editor_pair_editing++;undo_begin(UNDO_TYPE);
    if(line_join_next(line)) {
        editor_pairs_map(at-previous,at+1-previous,0);
        cur_line=line;cur_col=column;undo_commit();
    } else {undo_abort();snprintf(status_msg,sizeof(status_msg),"out of memory");}
    editor_pair_editing--;return 1;
}

static int editor_pair_key(int ch) {
    if(!lines_ready() || cur_line<0 || cur_line>=code_line_count) return 0;
    if(ch==(int)NCKEY_ENTER || ch=='\r' || ch=='\n') return editor_smart_newline();
    int close=editor_pair_close(ch);
    if(sel_active) {
        if(close && editor_pair_wrap(ch,close)) return 1;
        if(sel_active) return 0;
    }
    const char *line=code_lines[cur_line];
    int length=(int)strlen(line),at=editor_offset(cur_line,cur_col);
    if(at<0 || cur_col<0 || cur_col>length) return 0;
    if(ch==127 || ch==8 || ch==(int)NCKEY_BACKSPACE) {
        if(!cur_col) return cur_line?editor_pair_join(1):1;
        int previous=vi_prev(line,cur_col),remove=cur_col-previous;
        if(remove==1 && line[cur_col] && editor_pair_close((unsigned char)line[previous])==(unsigned char)line[cur_col]) remove++;
        if(editor_pair_splice(previous,remove,"",0)) cur_col=previous;
        return 1;
    }
    if(ch==(int)NCKEY_DEL) {
        if(cur_col==length) return editor_pair_join(0);
        (void)editor_pair_splice(cur_col,vi_next(line,cur_col,length)-cur_col,"",0);return 1;
    }
    if(ch<32 || ch>255 || ch==127) return 0;
    if(editor_smart_closer(ch)) return 1;
    int quote=ch=='\'' || ch=='"' || ch=='`';
    int slashes=0;for(int i=cur_col;i>0 && line[i-1]=='\\';i--) slashes++;
    int escaped=quote && (slashes&1);
    if(line[cur_col]==ch && !escaped) for(int i=0;i<editor_pair_count;i++) {
        if(editor_pairs[i].at==at && editor_pairs[i].close==ch) {
            editor_pairs[i]=editor_pairs[--editor_pair_count];cur_col++;return 1;
        }
    }
    int pair=0;
    if(close && !escaped) {
        unsigned char next=(unsigned char)line[cur_col];
        if(quote) {
            unsigned char previous=cur_col?(unsigned char)line[cur_col-1]:0;
            if(!completion_identifier(previous) && !completion_identifier(next)) {
                char call[64];int context,comment;
                pair=completion_context(call,&context,&comment) && context!=ch;
            }
        } else pair=!next || isspace(next) || strchr(")]}:;>",next)!=NULL;
    }
    char text[3]={(char)ch,pair?(char)close:0,0};
    if(editor_pair_splice(cur_col,0,text,pair?2:1)) {
        cur_col++;
        if(pair) editor_pair_mark(at+1,close);
    }
    return 1;
}
