/* Foreground-only offline reference; immutable, generated documentation. */
#include "../js/dist/reference.h"
enum { REFERENCE_COUNT=sizeof(strudel_reference)/sizeof(strudel_reference[0]) };
static char reference_query[128];
static int reference_matches[REFERENCE_COUNT],reference_count=-1,reference_selected;
static int reference_focus,reference_scroll,reference_rows=8;
static int reference_list_row,reference_list_start,reference_list_visible;

static int reference_query_start(int width) {
    int available=width-(reference_focus?9:12)-1;
    int length=(int)strlen(reference_query);
    if(available<0) available=0;
    return length>available?length-available:0;
}

static int reference_contains(const char *text,const char *query) {
    if(!*query) return 1;
    for(;*text;text++) {
        const char *a=text,*b=query;
        while(*a && *b && tolower((unsigned char)*a)==tolower((unsigned char)*b)) {a++;b++;}
        if(!*b) return 1;
    }
    return 0;
}

static int reference_word(const char *text,const char *query,int exact_case) {
    while(*text) {
        while(*text==' ') text++;
        const char *a=text,*b=query;
        while(*a && *a!=' ' && *b && (exact_case?*a==*b:tolower((unsigned char)*a)==tolower((unsigned char)*b))) {a++;b++;}
        if(!*b && (!*a || *a==' ')) return 1;
        while(*text && *text!=' ') text++;
    }
    return 0;
}

static void reference_filter(void) {
    reference_count=reference_selected=reference_scroll=0;
    for(int rank=0;rank<5;rank++) for(int i=0;i<REFERENCE_COUNT;i++) {
        const StrudelReference *entry=&strudel_reference[i];
        int category;
        if(!*reference_query) category=!strcmp(entry->origin,"Embedded runtime vocabulary")?4:0;
        else if(reference_word(entry->name,reference_query,1) || reference_word(entry->aliases,reference_query,1)) category=0;
        else if(reference_word(entry->name,reference_query,0) || reference_word(entry->aliases,reference_query,0)) category=1;
        else if(reference_contains(entry->name,reference_query) || reference_contains(entry->aliases,reference_query)) category=2;
        else if(reference_contains(entry->body,reference_query)) category=3;
        else continue;
        if(category==rank) reference_matches[reference_count++]=i;
    }
}

static void reference_open(const char *query) {
    if(query) {snprintf(reference_query,sizeof(reference_query),"%s",query);reference_filter();}
    if(reference_count<0) reference_filter();
    show_reference=reference_focus=1;show_docs=search_mode=file_mode=pane_focus=0;
    input_mode=0;completion_reset();
    snprintf(status_msg,sizeof(status_msg),"Strudel reference: type to search; Tab/Esc returns to editor; Alt+H closes");
}

static int reference_key(int ch) {
    if(!show_reference || !reference_focus) return 0;
    if(ch==17 || ch==16 || ch==12 || ch==KEY_STRUDEL_EVALUATE || ch==KEY_STRUDEL_STOP ||
       ch==((int)NCKEY_F00+1) || ch==((int)NCKEY_F00+5)) return 0;
    if(ch==27 || ch==9) {reference_focus=0;pane_focus=0;return 1;}
    if(ch==(int)NCKEY_UP || ch==(int)NCKEY_DOWN) {
        if(reference_count) reference_selected=(reference_selected+(ch==(int)NCKEY_DOWN?1:reference_count-1))%reference_count;
        reference_scroll=0;return 1;
    }
    if(ch==(int)NCKEY_PGUP || ch==(int)NCKEY_PGDOWN) {
        reference_scroll+=ch==(int)NCKEY_PGDOWN?reference_rows:-reference_rows;
        if(reference_scroll<0) reference_scroll=0;
        return 1;
    }
    size_t length=strlen(reference_query);
    if(ch==21) reference_query[0]=0;
    else if(ch==127 || ch==8 || ch==(int)NCKEY_BACKSPACE) {if(length) reference_query[length-1]=0;}
    else if(ch>=32 && ch<=126 && length<sizeof(reference_query)-1) {
        reference_query[length]=(char)ch;reference_query[length+1]=0;
    } else return 1;
    reference_filter();return 1;
}

/* ASCII generated text, hard/word wrapped into bounded foreground rows. */
static void reference_body(const char *text,int r0,int c0,int h,int w) {
    char line[256];if(w>250) w=250;
    if(w<1 || h<1) return;
    reference_rows=h;
    for(int pass=0;pass<2;pass++) {
        int logical=0,row=0;const char *p=text;
        while(*p) {
            int length=0;
            while(p[length] && p[length]!='\n' && length<w) length++;
            int take=length;
            if(length==w && p[length] && p[length]!='\n') {
                for(int i=length-1;i>length/2;i--) if(p[i]==' ') {take=i+1;break;}
            }
            if(pass && logical>=reference_scroll && row<h) {
                memcpy(line,p,(size_t)take);line[take]=0;
                (void)draw_text(r0+row++,c0,line,w,0);
            }
            logical++;p+=take;if(*p=='\n') p++;
        }
        if(!pass) {
            int maximum=logical>h?logical-h:0;
            if(reference_scroll>maximum) reference_scroll=maximum;
        }
    }
}

static void draw_reference_at(int r0,int c0,int h,int w) {
    reference_list_visible=0;
    if(h<4 || w<10) return;
    terminal_box(r0,c0,h,w);
    for(int row=1;row<h-1;row++) blank_inner(r0+row,c0+1,w-2);
    (void)draw_text(r0,c0+2," Strudel reference (Alt+H) ",w-4,0);
    char line[512];
    snprintf(line,sizeof(line),"%s /%s",reference_focus?"Search:":"Reference:",
        reference_query+reference_query_start(w-2));
    (void)draw_text(r0+1,c0+1,line,w-2,reference_focus?1:0);
    snprintf(line,sizeof(line),"%d results | Up/Down select | PgUp/PgDn scroll",reference_count);
    (void)draw_text(r0+2,c0+1,line,w-2,0);
    if(h<7) return;
    int list=h>=16?4:1;
    if(list>reference_count) list=reference_count;
    int start=reference_selected-list/2;if(start<0) start=0;
    if(start+list>reference_count) start=reference_count-list;
    reference_list_row=r0+3;reference_list_start=start;reference_list_visible=list;
    for(int i=0;i<list;i++) {
        const StrudelReference *entry=&strudel_reference[reference_matches[start+i]];
        snprintf(line,sizeof(line),"%s%s%s%s",start+i==reference_selected?"> ":"  ",entry->name,
            *entry->aliases?" / ":"",entry->aliases);
        (void)draw_text(r0+3+i,c0+1,line,w-2,start+i==reference_selected?1:0);
    }
    if(!reference_count) {(void)draw_text(r0+4,c0+1,"No matching functions. Ctrl+U clears search.",w-2,0);return;}
    const StrudelReference *entry=&strudel_reference[reference_matches[reference_selected]];
    int detail=r0+3+list;
    terminal_move(detail++,c0);terminal_write("├");
    for(int col=1;col<w-1;col++) terminal_write("─");
    terminal_write("┤");
    if(detail<r0+h-1) {(void)draw_text(detail++,c0+1,entry->origin,w-2,0);}
    reference_body(entry->body,detail,c0+1,r0+h-1-detail,w-2);
    (void)draw_text(r0+h-1,c0+1," Tab/Esc: editor | click pane: search ",w-2,0);
}
