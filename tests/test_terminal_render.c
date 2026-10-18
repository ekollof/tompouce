/* Inspect S-Lang's backing cells: no audio, input or screen refresh needed. */
#include "../src/tui.c"
#include <assert.h>

static void state_text(char *out, size_t size) {
    size_t at=0;
    for (int r=1;r<19;r++) {
        for (int c=0;c<78 && at+1<size;c++) {
            SLsmg_Char_Type cell; SLsmg_gotorc(r,c); assert(SLsmg_char_at(&cell)==0);
            out[at++]=cell.nchars && cell.wchars[0]<128?(char)cell.wchars[0]:' ';
        }
        if (at+1<size) out[at++]='\n';
    }
    out[at]=0;
}

static void check_state_modes(void) {
    replace_code_with("note('c4').gain(0)");
    char *code=editor_flatten(); assert(code && current_code_set(code)); free(code);
    mark_editor_clean();
    vi_mode=vi_normal=1; input_mode=0; graphics_disabled=1;
    SLsmg_cls(); draw_state_at(1,0,18,78);
    char text[1600]; state_text(text,sizeof(text));
    assert(strstr(text,"mode=VI:N") && strstr(text,"source=evaluated") && strstr(text,"file=saved"));
    assert(strstr(text,"graphics=braille (forced)") && strstr(text,"history undo=") && strstr(text,"song="));
    assert(editor_replace(0,0,"// pending\n"));
    input_mode=1;
    draw_state_at(1,0,18,78); state_text(text,sizeof(text));
    assert(strstr(text,"mode=PROMPT") && strstr(text,"source=pending F5") && strstr(text,"file=unsaved"));
    SLsmg_cls(); draw_state_at(1,0,8,30); /* Compact terminal clipping. */
    input_mode=vi_mode=vi_normal=graphics_disabled=0;
}

static void check_line(const char *source) {
    SLsmg_cls();
    SLsmg_gotorc(0,0);
    hl_quote = 0; hl_block_comment = hl_force_comment = 0;
    hl_play = NULL; sel_active = 0;
    int length = (int)strlen(source);
    assert(draw_hl_line(source,70) == length);
    for (int i = 0; i < length; i++) {
        SLsmg_Char_Type cell;
        SLsmg_gotorc(0,i);
        assert(SLsmg_char_at(&cell) == 0);
        assert(cell.nchars == 1 && cell.wchars[0] == (unsigned char)source[i]);
    }
}

static void check_inline_bindings(void) {
    const char *code = "// é\n$: note('c4')._scope()\n$: note('g4')._scope()";
    replace_code_with(code);
    assert(current_code_set(code));
    char error[512]; cJSON *metadata = NULL;
    JsProgram *program = javascript_compile(code,120,&metadata,error,sizeof(error));
    assert(program); cJSON_Delete(metadata);
    track_count = 1; memset(&tracks[0],0,sizeof(tracks[0]));
    tracks[0].active = 1; tracks[0].javascript = program; tracks[0].compile_generation = 11;
    inline_refresh(); inline_bind_feeds();
    assert(inline_feeds[1].mask[0] && inline_feeds[2].mask[0]);
    assert(inline_feeds[1].mask[0] != inline_feeds[2].mask[0]);
    assert(inline_feeds[1].generation == 11);
    assert(editor_replace(0,0,"// changed\n"));
    inline_refresh(); inline_bind_feeds();
    assert(!inline_feeds[2].mask[0] && !inline_feeds[2].generation);
    tracks[0].javascript = NULL; track_count = 0;
    javascript_free(program);
}

static void check_live_mode_highlights(void) {
    replace_code_with("note(\"c4 e4\").gain(0)");
    char *code = editor_flatten();
    assert(code && current_code_set(code));
    char error[512]; cJSON *metadata = NULL;
    JsProgram *program = javascript_compile(code,120,&metadata,error,sizeof(error));
    assert(program); cJSON_Delete(metadata);
    cJSON *haps = javascript_query(program,0,1);
    assert(cJSON_IsArray(haps));
    javascript_cache_spans(program,haps,audio_time_now(),2);
    cJSON_Delete(haps);
    track_count = 1; memset(&tracks[0],0,sizeof(tracks[0]));
    tracks[0].active = 1; tracks[0].javascript = program;
    audio_toggle_play(1);
    vi_mode = vi_normal = sel_active = 0; cur_line = cur_col = 0;
    draw_code();
    SLsmg_Char_Type note;
    SLsmg_gotorc(2,12); assert(SLsmg_char_at(&note) == 0 && note.color == 10);
    selection_start(0); cur_col = (int)strlen(code_lines[0]);
    draw_code();
    SLsmg_gotorc(2,12); assert(SLsmg_char_at(&note) == 0 && note.color == 11);
    editor_toggle_mode();
    assert(vi_mode && vi_normal && !sel_active && audio_is_playing());
    draw_code();
    SLsmg_gotorc(2,12); assert(SLsmg_char_at(&note) == 0 && note.color == 10);
    fill_play_mask(1.1);
    assert(!play_rows[0][6] && play_rows[0][9]);
    hl_play = play_rows[0]; hl_play_len = (int)strlen(code_lines[0]); hl_bias = 0;
    SLsmg_gotorc(2,6);
    (void)draw_hl_line(code_lines[0],70);
    SLsmg_gotorc(2,12); assert(SLsmg_char_at(&note) == 0 && note.color != 10);
    SLsmg_gotorc(2,15); assert(SLsmg_char_at(&note) == 0 && note.color == 10);
    editor_toggle_mode();
    assert(!vi_mode && !sel_active);
    draw_code();
    SLsmg_gotorc(2,12); assert(SLsmg_char_at(&note) == 0 && note.color == 10);
    assert(!strcmp(code,current_code));
    audio_toggle_play(0);
    tracks[0].javascript = NULL; track_count = 0;
    javascript_free(program); free(code);
}

int main(void) {
    SLutf8_enable(1);
    SLtt_get_terminfo();
    assert(SLsmg_init_smg() == 0);
    SLtt_set_color(2,NULL,"yellow","black");
    SLtt_set_color(3,NULL,"brightcyan","black");
    SLtt_set_color(5,NULL,"brightblue","black");
    SLtt_set_color(6,NULL,"gray","black");
    SLtt_set_color(8,NULL,"brightgreen","black");
    SLtt_set_color(10,NULL,"black","yellow");
    SLtt_set_color(11,NULL,"black","brightcyan");
    check_line("  note(\"c4 e4\").gain(.5)");
    check_line("    .gain(.5)");
    check_line("  const melody = note(\"c4\")");
    check_line("  // comment");
    check_line("  notes c4 e4");
    check_line("  bpm 120");
    check_line("  stop");
    check_line("  ");
    check_line("  const amount = 0.5");
    SLsmg_Char_Type keyword, number;
    SLsmg_gotorc(0,2); assert(SLsmg_char_at(&keyword) == 0);
    SLsmg_gotorc(0,17); assert(SLsmg_char_at(&number) == 0);
    assert(keyword.color != number.color);
    SLsmg_gotorc(1,0);
    const char *unicode = "  const café = \"♫\";";
    hl_src = unicode;
    assert(draw_hl_line(unicode,70) == 19);
    SLsmg_Char_Type accent, music, semicolon;
    SLsmg_gotorc(1,11); assert(SLsmg_char_at(&accent) == 0 && accent.wchars[0] == 0xe9);
    SLsmg_gotorc(1,16); assert(SLsmg_char_at(&music) == 0 && music.wchars[0] == 0x266b);
    SLsmg_gotorc(1,18); assert(SLsmg_char_at(&semicolon) == 0 && semicolon.wchars[0] == ';');
    hl_quote = 0;
    highlight_context("note(\"unfinished");
    SLsmg_gotorc(1,0);
    assert(draw_hl_line("const amount = 0.5",70) == 18);
    SLsmg_gotorc(1,0); assert(SLsmg_char_at(&keyword) == 0);
    assert(keyword.color == 2);
    /* Render wrapped rows through the real editor, including typing a quote. */
    replace_code_with("  const melody = note(\"c4 e4 g4 b4 c5 e5 g5 b5\").gain(.5)\n  note(\"unfinished\n  const amount = 0.5");
    cols = 80; rows = 24; split_ratio = .6;
    cur_line = cur_col = view_top = view_col = 0;
    editor_scope = 0; inline_visuals = 0; editor_wrap = 1;
    draw_code();
    int row = 2, pane = 48;
    for (int line = 0; line < code_line_count; line++) {
        int width = line_textw(pane,line), count = line_text_rows(line,width);
        for (int sub = 0; sub < count; sub++, row++) {
            int a, b;
            editor_row_span(code_lines[line],width,sub,&a,&b);
            for (int i = a; i < b; i++) {
                SLsmg_Char_Type cell;
                SLsmg_gotorc(row,6+i-a); assert(SLsmg_char_at(&cell) == 0);
                if (cell.nchars != 1 || cell.wchars[0] != (unsigned char)code_lines[line][i])
                    fprintf(stderr,"render mismatch line=%d sub=%d byte=%d row=%d want=%c got=%u\n",line,sub,i,row,code_lines[line][i],cell.wchars[0]);
                assert(cell.nchars == 1 && cell.wchars[0] == (unsigned char)code_lines[line][i]);
            }
        }
    }
    check_inline_bindings();
    check_live_mode_highlights();
    check_state_modes();
    SLsmg_reset_smg();
    puts("terminal highlight cells and indentation ok");
    return 0;
}
