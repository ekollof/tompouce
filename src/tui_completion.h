/* Foreground-only completion. Runtime vocabulary is generated at bundle time;
 * edited code is scanned as text and is never executed for suggestions. */
#include "../js/dist/completions.h"
#define COMPLETION_MAX 256
#define COMPLETION_WORD 1024
static struct {
    int target, count, index, start, end, line, undo_depth, edited, prefix_len;
    unsigned hash;
    char words[COMPLETION_MAX][COMPLETION_WORD], prompt[sizeof(input_buf)];
} completion;
static void completion_reset(void) { completion.target=completion.count=completion.edited=0; }
static int completion_identifier(int c) {
    unsigned char ch=(unsigned char)c;
    return isalnum(ch) || ch=='_' || ch=='$' || ch>=128;
}
static void completion_add(const char *word, const char *prefix) {
    if (!word || !*word || completion.count>=COMPLETION_MAX || strlen(word)>=COMPLETION_WORD ||
        strncmp(word,prefix,strlen(prefix))) return;
    for (int i=0;i<completion.count;i++) if (!strcmp(word,completion.words[i])) return;
    snprintf(completion.words[completion.count++],COMPLETION_WORD,"%s",word);
}
static void completion_list(const char *const *words, const char *prefix) {
    for (int i=0;words[i];i++) completion_add(words[i],prefix);
}
static int completion_current(int target) {
    if (completion.target!=target || !completion.count) return 0;
    if (target==1) return !sel_active && cur_line==completion.line && cur_col==completion.end && hash_code()==completion.hash;
    return !strcmp(input_buf,completion.prompt);
}
static int completion_apply(const char *word) {
    int length=(int)strlen(word);
    if (completion.target==1) {
        if (length==completion.end-completion.start &&
            !memcmp(code_lines[cur_line]+completion.start,word,(size_t)length)) {
            cur_col=completion.end; return 1;
        }
        int coalesce=completion.edited && undo_n==completion.undo_depth;
        if (coalesce) undo_suspend++;
        int a=editor_offset(cur_line,completion.start), b=editor_offset(cur_line,completion.end);
        int ok=editor_replace(a,b,word);
        if (coalesce) undo_suspend--;
        if (!ok) return 0;
        completion.edited=1; completion.undo_depth=undo_n;
        completion.end=cur_col; completion.hash=hash_code();
    } else {
        int limit=input_mode==2?(int)sizeof(input_buf)-1:AI_PROMPT_MAX;
        if (completion.start+length>limit) { snprintf(status_msg,sizeof(status_msg),"completion is too long"); return 0; }
        memcpy(input_buf+completion.start,word,(size_t)length+1);
        input_len=completion.end=completion.start+length;
        snprintf(completion.prompt,sizeof(completion.prompt),"%s",input_buf);
    }
    return 1;
}
static void completion_hint(void) {
    char words[160]={0}; size_t used=0;
    for (int i=0;i<completion.count && i<4;i++) {
        int n=snprintf(words+used,sizeof(words)-used,"%s%.32s",i?" | ":"",completion.words[i]);
        if (n<0 || (size_t)n>=sizeof(words)-used) break;
        used+=(size_t)n;
    }
    snprintf(status_msg,sizeof(status_msg),"%d%s: %s; Tab cycles",completion.count,
             completion.count==COMPLETION_MAX?"+":" matches",words);
}
static void completion_choose(int backwards, int fresh) {
    if (!completion.count) { snprintf(status_msg,sizeof(status_msg),"no completion"); return; }
    if (fresh && !backwards && completion.count>1) {
        int common=(int)strlen(completion.words[0]);
        for (int i=1;i<completion.count;i++) {
            int n=0;
            while (n<common && completion.words[0][n]==completion.words[i][n]) n++;
            common=n;
        }
        if (common>completion.prefix_len) {
            char word[COMPLETION_WORD]; memcpy(word,completion.words[0],(size_t)common); word[common]=0;
            if (completion_apply(word)) {
                completion_hint(); return;
            }
        }
    }
    if (fresh) completion.index=backwards?completion.count-1:0;
    else completion.index=(completion.index+(backwards?-1:1)+completion.count)%completion.count;
    if (completion_apply(completion.words[completion.index]))
        snprintf(status_msg,sizeof(status_msg),"%d/%d: %.180s",completion.index+1,completion.count,completion.words[completion.index]);
}
/* Bounded lexical context, including strings/comments and multiline calls.
 * A depth limit or a very large prefix simply declines completion. */
static int completion_context(char call[64], int *quote, int *comment) {
    char stack[32][64]={{0}}, word[64]={0};
    int depth=0, escaped=0, block=0, single=0, wn=0, word_end=0;
    size_t scanned=0;
    *quote=*comment=0; call[0]=0;
    for (int row=0;row<=cur_line;row++) {
        const char *s=code_lines[row]; int end=row==cur_line?cur_col:(int)strlen(s);
        for (int at=0;at<end;at++) {
            if (++scanned>1024u*1024u) return 0;
            unsigned char ch=(unsigned char)s[at], next=at+1<end?(unsigned char)s[at+1]:0;
            if (single) continue;
            if (block) { if (ch=='*' && next=='/') { block=0; at++; } continue; }
            if (*quote) {
                if (escaped) escaped=0;
                else if (ch=='\\') escaped=1;
                else if (ch==*quote) { *quote=0; word[0]=0; wn=0; }
                continue;
            }
            if (ch=='/' && next=='/') { single=1; at++; continue; }
            if (ch=='/' && next=='*') { block=1; at++; continue; }
            if (ch=='\'' || ch=='"' || ch=='`') {
                *quote=ch;
                snprintf(call,64,"%s",depth?stack[depth-1]:"");
                word[0]=0; wn=0; continue;
            }
            if (completion_identifier(ch)) {
                if (word_end) { wn=0; word[0]=0; word_end=0; }
                if (wn<63) { word[wn++]=(char)ch; word[wn]=0; }
                continue;
            }
            if (ch=='(') {
                if (depth==32) return 0;
                snprintf(stack[depth++],64,"%s",word);
            } else if (ch==')' && depth) depth--;
            if (!isspace(ch)) { word[0]=0; wn=0; word_end=0; }
            else word_end=1;
        }
        if (row<cur_line) { single=0; word_end=1; }
        if (*quote!='`' && *quote && !escaped && row<cur_line) *quote=0;
        escaped=0;
    }
    *comment=block || single;
    return 1;
}
static void completion_sounds(const char *prefix, int bank) {
    SoundInfo *sounds=calloc(4096,sizeof(*sounds));
    if (!sounds) return;
    int count=sounds_search(bank?SOUND_MACHINES:-1,NULL,sounds,4096);
    for (int i=0;i<count && i<4096;i++) {
        if (bank) { char *p=strrchr(sounds[i].name,'_'); if (!p) continue; *p=0; }
        completion_add(sounds[i].name,prefix);
    }
    free(sounds);
}
static int editor_complete(int backwards, int explicit_request) {
    if (!lines_ready() || sel_active || cur_line<0 || cur_line>=code_line_count) return 0;
    if (completion_current(1)) { completion_choose(backwards,0); return 1; }
    completion_reset();
    const char *line=code_lines[cur_line]; int n=(int)strlen(line);
    if (cur_col<0 || cur_col>n) return 0;
    char call[64]; int quote,comment;
    if (!completion_context(call,&quote,&comment) || comment) return 0;
    int start=cur_col, end=cur_col;
    while (start>0 && (completion_identifier(line[start-1]) || (quote && strchr(":#-/",line[start-1])))) start--;
    while (end<n && (completion_identifier(line[end]) || (quote && strchr(":#-/",line[end])))) end++;
    int size=cur_col-start;
    int after_dot=start>0 && line[start-1]=='.';
    int known_string=quote && (!strcmp(call,"s") || !strcmp(call,"sound") || !strcmp(call,"bank") ||
                              !strcmp(call,"note") || !strcmp(call,"scale"));
    if (size>=COMPLETION_WORD || (!size && !explicit_request && !after_dot && !known_string)) return 0;
    char prefix[COMPLETION_WORD]; memcpy(prefix,line+start,(size_t)size); prefix[size]=0;
    completion.target=1; completion.start=start; completion.end=end; completion.line=cur_line;
    completion.index=-1; completion.hash=hash_code();
    completion.prefix_len=size;
    if (quote) {
        if (!strcmp(call,"s") || !strcmp(call,"sound") || !strcmp(call,"bank")) completion_sounds(prefix,!strcmp(call,"bank"));
        else if (!strcmp(call,"note")) {
            const char *notes[]={"c","c#","d","d#","e","f","f#","g","g#","a","a#","b"};
            const int octaves[]={4,3,5,2,6,1,7,0,8,-1,9};
            for (unsigned o=0;o<sizeof(octaves)/sizeof(*octaves);o++) for (int pitch=0;pitch<12;pitch++) {
                int octave=octaves[o];
                if ((octave+1)*12+pitch>127) continue;
                char word[16]; snprintf(word,sizeof(word),"%s%d",notes[pitch],octave); completion_add(word,prefix);
            }
            completion_add("~",prefix); completion_add("rest",prefix);
        } else if (!strcmp(call,"scale")) {
            static const char *const scales[]={"C:major","C:minor","C:dorian","C:mixolydian","A:minor:pentatonic","C:blues",NULL};
            completion_list(scales,prefix);
        } else { completion_reset(); return 0; }
    } else if (after_dot) completion_list(strudel_method_words,prefix);
    else {
        completion_list(strudel_global_words,prefix);
    }
    if (!completion.count) {
        completion_reset();
        snprintf(status_msg,sizeof(status_msg),"no completion");
        return 1;
    }
    /* Match the prefix, but replace the full token if the cursor is inside it. */
    if (completion.count>1 && !size) {
        completion_hint(); return 1;
    }
    completion_choose(backwards,1); return 1;
}
