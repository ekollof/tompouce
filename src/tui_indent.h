/* Syntax-aware foreground indentation. Never executes editor JavaScript. */
typedef struct { int row,col,at,close; } EditorIndentOpen;
typedef struct {
    EditorIndentOpen opens[256];
    int count,literal;
    unsigned visited;
} EditorIndentContext;

static int editor_indent_leading(int row) {
    int n=0;while(code_lines[row][n]==' ' || code_lines[row][n]=='\t') n++;
    return n;
}

static int editor_indent_walk(TSNode node,int at,int depth,EditorIndentContext *context) {
    if(ts_node_is_null(node) || ts_node_is_missing(node)) return 1;
    if(depth>128 || ++context->visited>65536) return 0;
    if(ts_node_start_byte(node)>=(uint32_t)at) return 1;
    const char *type=ts_node_type(node);
    if(!strcmp(type,"string") || !strcmp(type,"comment") || !strcmp(type,"regex")) return 1;
    uint32_t count=ts_node_child_count(node);
    if(!count) {
        int open=!strcmp(type,"${")?'{':(type[0] && !type[1]?(unsigned char)type[0]:0);
        int close=open=='(' || open=='[' || open=='{'?editor_pair_close(open):0;
        if(close) {
            if(context->count==256) return 0;
            TSPoint point=ts_node_start_point(node);
            int offset=!strcmp(type,"${");
            context->opens[context->count++]=(EditorIndentOpen){(int)point.row,(int)point.column+offset,
                (int)ts_node_start_byte(node)+offset,close};
        } else if((open==')' || open==']' || open=='}') && context->count &&
                  context->opens[context->count-1].close==open) context->count--;
        return 1;
    }
    for(uint32_t i=0;i<count;i++) {
        TSNode child=ts_node_child(node,i);
        if(ts_node_is_null(child)) continue;
        if(!strcmp(type,"template_string") && strcmp(ts_node_type(child),"template_substitution")) continue;
        if(!editor_indent_walk(child,at,depth+1,context)) return 0;
    }
    return 1;
}

static void editor_indent_context(int at,EditorIndentContext *context) {
    memset(context,0,sizeof(*context));
    /* Large/deep or incomplete trees fall back to the existing line indent. */
    size_t size=0;const size_t limit=256u*1024u;
    for(int i=0;i<code_line_count;i++) {
        size_t length=strnlen(code_lines[i],limit-size);
        if(length>=limit-size) return;
        size+=length+1;if(size>=limit) return;
    }
    char *source=editor_flatten();if(!source) return;
    TSParser *parser=ts_parser_new();TSTree *tree=NULL;
    if(parser && ts_parser_set_language(parser,tree_sitter_javascript()))
        tree=ts_parser_parse_string(parser,NULL,source,(uint32_t)size);
    if(tree) {
        TSNode root=ts_tree_root_node(tree);
        if(!ts_node_is_null(root)) {
            TSNode node=ts_node_descendant_for_byte_range(root,(uint32_t)at,(uint32_t)at);
            for(int depth=0;depth<128 && !ts_node_is_null(node);depth++,node=ts_node_parent(node)) {
                const char *type=ts_node_type(node);
                if(!strcmp(type,"template_substitution")) break;
                if((!strcmp(type,"string") || !strcmp(type,"template_string") || !strcmp(type,"comment") ||
                    !strcmp(type,"regex") || !strcmp(type,"string_fragment")) &&
                   ts_node_start_byte(node)<(uint32_t)at && ts_node_end_byte(node)>(uint32_t)at) {
                    context->literal=1;break;
                }
            }
            if(!editor_indent_walk(root,at,0,context)) context->count=0;
        }
        ts_tree_delete(tree);
    }
    if(parser) ts_parser_delete(parser);
    free(source);
}

static int editor_indent_generated(int at,int ch) {
    for(int i=0;i<editor_pair_count;i++) if(editor_pairs[i].at==at && editor_pairs[i].close==ch) return 1;
    return 0;
}

static int editor_smart_newline_range(int from,int to) {
    if(from<0 || to<from) return 1;
    int row=0,col=0,right_row=0,right_col=0;
    editor_pair_position(from,&row,&col);editor_pair_position(to,&right_row,&right_col);
    int leading=editor_indent_leading(row);if(leading>col) leading=col;
    int base_row=row,base=leading,extra=0,split=0;
    EditorIndentContext context;editor_indent_context(from,&context);
    int right=right_col;
    if(!context.literal) while(code_lines[right_row][right]==' ' || code_lines[right_row][right]=='\t') right++;
    if(!context.literal && context.count) {
        EditorIndentOpen open=context.opens[context.count-1];
        base_row=open.row;base=editor_indent_leading(base_row);extra=2;
        if(code_lines[right_row][right]==open.close) {
            int left=col;while(left>0 && isspace((unsigned char)code_lines[row][left-1])) left--;
            split=row==open.row && left>0 && editor_offset(row,left-1)==open.at;
            if(!split) extra=0;
        }
    }
    size_t indent=(size_t)base+(size_t)extra;
    size_t length=1+indent+(split?1+(size_t)base:0);
    if(length>SOURCE_MAX || to>INT_MAX-(right-right_col)) {
        snprintf(status_msg,sizeof(status_msg),"edit exceeds the 2 GiB offset limit");return 1;
    }
    char *text=malloc(length+1);
    if(!text) {snprintf(status_msg,sizeof(status_msg),"out of memory");return 1;}
    size_t n=0;text[n++]='\n';memcpy(text+n,code_lines[base_row],(size_t)base);n+=(size_t)base;
    if(extra) {text[n++]=' ';text[n++]=' ';}
    if(split) {text[n++]='\n';memcpy(text+n,code_lines[base_row],(size_t)base);n+=(size_t)base;}
    text[n]=0;
    if(editor_replace_grouped(from,to+(right-right_col),text,UNDO_TYPE)) {
        cur_line=row+1;cur_col=(int)indent;
    }
    free(text);return 1;
}

static int editor_smart_newline(void) {
    int from=editor_offset(cur_line,cur_col),to=from;
    if(sel_active) selection_bounds(&from,&to);
    return editor_smart_newline_range(from,to);
}

static int editor_smart_open_line(int above) {
    int old_line=cur_line,old_col=cur_col,count=code_line_count;
    sel_active=0;
    if(!above || cur_line>0) {
        int line=cur_line-(above?1:0);
        int at=editor_offset(line,(int)strlen(code_lines[line]));
        (void)editor_smart_newline_range(at,at);
    } else {
        int leading=editor_indent_leading(0);
        if(leading==INT_MAX) return 0;
        char *text=malloc((size_t)leading+2);
        if(!text) {snprintf(status_msg,sizeof(status_msg),"out of memory");return 0;}
        memcpy(text,code_lines[0],(size_t)leading);text[leading]='\n';text[leading+1]=0;
        if(editor_replace_grouped(0,0,text,UNDO_TYPE)) {cur_line=0;cur_col=leading;}
        free(text);
    }
    if(code_line_count>count) return 1;
    cur_line=old_line;cur_col=old_col;return 0;
}

/* A typed closer on an otherwise blank line returns to its opener's indent. */
static int editor_smart_closer(int ch) {
    if(ch!=')' && ch!=']' && ch!='}') return 0;
    for(int i=0;i<cur_col;i++) if(code_lines[cur_line][i]!=' ' && code_lines[cur_line][i]!='\t') return 0;
    int at=editor_offset(cur_line,cur_col);if(at<0) return 0;
    EditorIndentContext context;editor_indent_context(at,&context);
    if(context.literal || !context.count) return 0;
    EditorIndentOpen open=context.opens[context.count-1];
    if(open.close!=ch || open.row==cur_line) return 0;
    int base=editor_indent_leading(open.row);
    int to=at,skip=code_lines[cur_line][cur_col]==ch && editor_indent_generated(at,ch);
    int right=cur_col;while(code_lines[cur_line][right]==' ' || code_lines[cur_line][right]=='\t') right++;
    if(!code_lines[cur_line][right] && cur_line+1<code_line_count) {
        int next=editor_indent_leading(cur_line+1),next_at=editor_offset(cur_line+1,next);
        if(code_lines[cur_line+1][next]==ch && editor_indent_generated(next_at,ch)) {
            to=next_at;skip=1;
        }
    }
    if(to==at && base==cur_col && !memcmp(code_lines[cur_line],code_lines[open.row],(size_t)base)) return 0;
    char *text=malloc((size_t)base+2);
    if(!text) {snprintf(status_msg,sizeof(status_msg),"out of memory");return 1;}
    memcpy(text,code_lines[open.row],(size_t)base);text[base]=(char)ch;text[base+1]=0;
    (void)editor_replace_grouped(at-cur_col,to+skip,text,UNDO_TYPE);
    free(text);return 1;
}
