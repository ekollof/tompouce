/* Inspect Notcurses backing cells without audio or a screen refresh. */
#include "../src/tui.c"
#include <assert.h>

static void check_input_readiness(void) {
    while (SLang_input_pending(0)) (void)SLang_getkey();
    ncinput events[3]={{.id=NCKEY_RESIZE},{.id='z',.evtype=NCTYPE_RELEASE},
                      {.id='x',.evtype=NCTYPE_PRESS}};
    assert(write(terminal_event_pipe[1],events,sizeof(events))==(ssize_t)sizeof(events));
    /* Non-text events preceding queued text must not cancel a paste. */
    assert(SLang_input_pending(0));
    assert(SLang_getkey()=='x');
}

static void theme_fixture(const char *path, int wal, unsigned seed) {
    FILE *f=fopen(path,"w"); assert(f);
    if (wal) {
        fprintf(f,"{\"special\":{\"foreground\":\"#123456\",\"background\":\"#654321\"},\"colors\":{");
        for (int i=0;i<16;i++) fprintf(f,"%s\"color%d\":\"#%06x\"",i?",":"",i,seed+(unsigned)i);
        fputs("}}",f);
    } else {
        fputs("foreground = \"#123456\"\nbackground = \"#654321\"\n",f);
        for (int i=0;i<16;i++) fprintf(f,"color%d = \"#%06x\"\n",i,seed+(unsigned)i);
    }
    assert(!fclose(f));
}

static void check_theme_watch(void) {
    char named[]="background=\"#000001\"\nforeground=\"#000002\"\nred=\"#000003\"\ngreen=\"#000004\"\nyellow=\"#000005\"\nblue=\"#000006\"\nmagenta=\"#000007\"\ncyan=\"#000008\"\ndark_foreground=\"#000009\"\nbright_red=\"#00000a\"\nbright_green=\"#00000b\"\nbright_yellow=\"#00000c\"\nbright_blue=\"#00000d\"\nbright_magenta=\"#00000e\"\nbright_cyan=\"#00000f\"\nbright_foreground=\"#000010\"\n";
    uint32_t colors[18]; assert(theme_parse(named,0,colors));
    assert(colors[0]==1 && colors[15]==16 && colors[16]==2);
    char bad[]="{\"colors\":{\"color0\":\"#ffffff\"}}";
    assert(!theme_parse(bad,1,colors));
    assert(!theme_hex("#12345g",colors));
    char root[]="/tmp/tompouce-theme-XXXXXX"; assert(mkdtemp(root));
    char *old_config=getenv("XDG_CONFIG_HOME")?strdup(getenv("XDG_CONFIG_HOME")):NULL;
    char *old_cache=getenv("XDG_CACHE_HOME")?strdup(getenv("XDG_CACHE_HOME")):NULL;
    char *old_state=getenv("XDG_STATE_HOME")?strdup(getenv("XDG_STATE_HOME")):NULL;
    assert(!setenv("XDG_CONFIG_HOME",root,1) && !setenv("XDG_CACHE_HOME",root,1));
    assert(!setenv("XDG_STATE_HOME",root,1));
    char omarchy[512], wal[512], replacement[512], linkpath[512], target[512];
    snprintf(omarchy,sizeof(omarchy),"%s/omarchy/current/theme",root); mkdir_p(omarchy);
    snprintf(wal,sizeof(wal),"%s/wal",root); mkdir_p(wal);
    snprintf(omarchy,sizeof(omarchy),"%s/omarchy/current/theme/colors.toml",root);
    snprintf(wal,sizeof(wal),"%s/wal/colors.json",root);
    snprintf(replacement,sizeof(replacement),"%s/replacement",root);
    theme_mode=0; theme_init();
    uint32_t original=terminal_ansi[3];
    theme_fixture(wal,1,0x100000);
    theme_mode=3; theme_poll(1); assert(terminal_ansi[3]==0x100003);
    theme_fixture(omarchy,0,0x200000);
    theme_next_poll=0; theme_poll(0); assert(terminal_ansi[3]==0x200003);
    assert(ncchannels_fg_rgb(terminal_colors[2])==0x20000b); /* keyword yellow */
    assert(ncchannels_fg_rgb(terminal_colors[0])==0x123456);
    unsigned revision=theme_revision;
    theme_poll(1); assert(theme_revision==revision);
    theme_fixture(replacement,0,0x300000); assert(!rename(replacement,omarchy));
    theme_next_poll=0; theme_poll(0); assert(terminal_ansi[3]==0x300003);
    FILE *f=fopen(omarchy,"w"); assert(f); fputs("foreground = \"#ffffff",f); fclose(f);
    theme_mode=1; theme_poll(1); assert(terminal_ansi[3]==0x300003);
    assert(!unlink(omarchy));
    snprintf(linkpath,sizeof(linkpath),"%s/omarchy/current/theme",root);
    assert(!rmdir(linkpath));
    snprintf(target,sizeof(target),"%s/alternate",root); assert(!mkdir(target,0700));
    assert(!symlink(target,linkpath));
    theme_fixture(omarchy,0,0x400000); theme_next_poll=0; theme_poll(0);
    assert(terminal_ansi[3]==0x400003);
    theme_mode=2; theme_poll(1); assert(terminal_ansi[3]==0x100003);
    theme_mode=0; theme_poll(1); assert(terminal_ansi[3]==original);
    assert(!unlink(omarchy) && !unlink(linkpath) && !rmdir(target));
    assert(!unlink(wal)); snprintf(wal,sizeof(wal),"%s/wal",root); assert(!rmdir(wal));
    snprintf(target,sizeof(target),"%s/omarchy/current",root); assert(!rmdir(target));
    snprintf(target,sizeof(target),"%s/omarchy",root); assert(!rmdir(target)); assert(!rmdir(root));
    if (old_config) { setenv("XDG_CONFIG_HOME",old_config,1); free(old_config); } else unsetenv("XDG_CONFIG_HOME");
    if (old_cache) { setenv("XDG_CACHE_HOME",old_cache,1); free(old_cache); } else unsetenv("XDG_CACHE_HOME");
    if (old_state) { setenv("XDG_STATE_HOME",old_state,1); free(old_state); } else unsetenv("XDG_STATE_HOME");
}

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
    free(editor_written); editor_written=editor_flatten(); assert(editor_written);
    vi_mode=vi_normal=1; input_mode=0; graphics_disabled=1;
    SLsmg_cls(); draw_state_at(1,0,18,78);
    char text[1600]; state_text(text,sizeof(text));
    assert(strstr(text,"mode=VI:N") && strstr(text,"source=evaluated") && strstr(text,"file=saved"));
    assert(strstr(text,"graphics=braille (forced)") && strstr(text,"history undo=") && strstr(text,"song="));
    assert(editor_replace(0,0,"// pending\n"));
    input_mode=1;
    draw_state_at(1,0,18,78); state_text(text,sizeof(text));
    assert(strstr(text,"mode=PROMPT") && strstr(text,"source=pending F5") && strstr(text,"file=unsaved"));
    input_mode=2; draw_state_at(1,0,18,78); state_text(text,sizeof(text));
    assert(strstr(text,"mode=COMMAND"));
    input_buf[0]=0; input_len=0; draw_input();
    SLsmg_Char_Type prompt; SLsmg_gotorc(rows-2,0); assert(SLsmg_char_at(&prompt)==0 && prompt.wchars[0]==':');
    snprintf(input_buf,sizeof(input_buf),"%s","e My café.json"); input_len=(int)strlen(input_buf); draw_input();
    SLsmg_gotorc(rows-2,0); assert(SLsmg_char_at(&prompt)==0 && prompt.wchars[0]==':');
    memset(input_buf,'x',sizeof(input_buf)-1); input_buf[sizeof(input_buf)-1]=0;
    input_len=(int)strlen(input_buf); assert(prompt_view_start()>0); draw_input();
    input_len=0; input_buf[0]=0;
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
    javascript_update_visuals(program,0,0,2); inline_bind_feeds();
    assert(inline_feeds[1].mask[0] && inline_feeds[2].mask[0]);
    assert(inline_feeds[1].mask[0] != inline_feeds[2].mask[0]);
    assert(inline_feeds[1].generation == 11);
    assert(editor_replace(0,0,"// changed\n"));
    inline_refresh(); inline_bind_feeds();
    javascript_update_visuals(program,0,0,2); inline_bind_feeds();
    assert(inline_feeds[2].mask[0] && inline_feeds[2].generation==11);
    tracks[0].javascript = NULL; track_count = 0;
    javascript_free(program);
}

static void check_live_visual_addition(void) {
    const char *code="$: note(60).gain(0).slow(4); $: note(67).gain(0)";
    assert(current_code_set(code));
    char error[512]; cJSON *metadata=NULL;
    JsProgram *program=javascript_compile(code,120,&metadata,error,sizeof(error));
    assert(program); cJSON_Delete(metadata);
    track_count=1; memset(&tracks[0],0,sizeof(tracks[0]));
    tracks[0].javascript=program; tracks[0].compile_generation=77;
    tracks[0].next_cycle=123; tracks[0].cycle_index=45;
    unsigned generation=event_queue_generation();
    replace_code_with("// pending é\n$: note(60)._pianoroll().gain(0).slow(4); $: note(67).gain(0)._scope()");
    inline_refresh(); inline_bind_feeds();
    javascript_update_visuals(program,1,0,2); inline_bind_feeds();
    unsigned roll=inline_feeds[1].mask[VISUAL_PIANOROLL-1], scope=inline_feeds[1].mask[VISUAL_SCOPE-1];
    assert(roll && scope && roll!=scope && inline_feeds[1].generation==77);
    VisualNote notes[64];
    int count=javascript_live_visual_copy(program,roll,VISUAL_PIANOROLL,notes,64,NULL,NULL);
    assert(count>0);
    for (int i=0;i<count;i++) assert(notes[i].pitch==60 && notes[i].end-notes[i].start==8);
    assert(tracks[0].javascript==program && tracks[0].next_cycle==123 && tracks[0].cycle_index==45);
    assert(event_queue_generation()==generation && !strcmp(current_code,code));
    tracks[0].javascript=NULL; track_count=0; javascript_free(program);
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

static void check_stopped_highlight_growth(void) {
    replace_code_with("note(\"c4\").gain(0)");
    assert(current_code_set(code_lines[0]));
    free(play_rows[0]); play_rows[0]=NULL; play_cap[0]=0;
    audio_toggle_play(1); fill_play_mask(0);
    int old_capacity=play_cap[0]; assert(old_capacity>0);
    audio_toggle_play(0); sel_active=vi_mode=vi_normal=0; pane_focus=1;
    const char *comment="// contain a plain instrument name. Keep both quote pairs. This line grew while stopped.";
    replace_code_with(comment);
    assert((int)strlen(comment)>old_capacity);
    cur_line=cur_col=view_top=view_col=0;
    draw_code();
    assert(hl_play_len<=play_cap[hl_line]);
    int mid,h,w,sh,th; right_geom(&mid,&h,&w,&sh,&th);
    int width=line_textw(mid,0), nrows=line_text_rows(0,width);
    for (int sub=0;sub<nrows;sub++) {
        int a,b; editor_row_span(comment,width,sub,&a,&b);
        for (int i=a;i<b;i++) {
            SLsmg_Char_Type cell; SLsmg_gotorc(sub+2,6+i-a);
            assert(!SLsmg_char_at(&cell) && cell.color==6);
        }
    }
    /* A shorter replacement followed by another growth must stay cleared. */
    replace_code_with("// short"); draw_code();
    replace_code_with(comment); draw_code();
    assert(hl_play_len<=play_cap[hl_line]);
    pane_focus=0;
}

static void check_visual_edit_highlights(void) {
    const char *code="$: note(\"c4 e4\").gain(0)\n$: note(\"g4 a4\").gain(0)";
    replace_code_with(code); assert(current_code_set(code));
    char error[512]; cJSON *metadata=NULL;
    JsProgram *program=javascript_compile(code,120,&metadata,error,sizeof(error));
    assert(program); cJSON_Delete(metadata);
    cJSON *haps=javascript_query(program,0,1); assert(cJSON_IsArray(haps));
    javascript_cache_spans(program,haps,0,2); cJSON_Delete(haps);
    track_count=1; memset(&tracks[0],0,sizeof(tracks[0]));
    tracks[0].active=1; tracks[0].javascript=program; tracks[0].compile_generation=81;
    audio_toggle_play(1); sel_active=vi_mode=vi_normal=0;
    replace_code_with("// é♫\n$: note(\"c4 e4\")._scope().gain(0)\n  $: note(\"g4 a4\").gain(0)");
    inline_refresh(); inline_bind_feeds();
    javascript_update_visuals(program,.1,0,2); inline_bind_feeds();
    assert(inline_feeds[1].mask[VISUAL_SCOPE-1]);
    fill_play_mask(.1);
    assert(play_rows[1] && play_rows[1][9] && !play_rows[1][12]);
    assert(play_rows[2] && play_rows[2][11] && !play_rows[2][14]);
    hl_quote=hl_block_comment=hl_force_comment=0; hl_bias=0;
    hl_play=play_rows[1]; hl_play_len=(int)strlen(code_lines[1]);
    SLsmg_gotorc(0,0); (void)draw_hl_line(code_lines[1],70);
    SLsmg_Char_Type cell;
    SLsmg_gotorc(0,9); assert(SLsmg_char_at(&cell)==0 && cell.color==10 && cell.wchars[0]=='c');
    fill_play_mask(1.1);
    assert(!play_rows[1][9] && play_rows[1][12]);
    assert(!play_rows[2][11] && play_rows[2][14]);
    /* Moving/removing a view and changing literal options must preserve the
       same cached schedule, including spans after the edited source line. */
    replace_code_with("$: note(\"c4 e4\").gain(0)\n$: note(\"g4 a4\")._scope({thickness:2}).gain(0)");
    inline_refresh(); inline_bind_feeds(); javascript_update_visuals(program,.1,0,2); inline_bind_feeds();
    fill_play_mask(.1); assert(play_rows[0][9] && play_rows[1][9]);
    replace_code_with("$: note(\"c5 e4\").gain(0)\n$: note(\"g4 a4\")._scope().gain(0)");
    inline_refresh(); inline_bind_feeds(); javascript_update_visuals(program,.1,0,2); inline_bind_feeds();
    fill_play_mask(.1); assert(!play_rows[0][9] && !play_rows[1][9]);
    assert(!strcmp(current_code,code));
    audio_toggle_play(0); tracks[0].javascript=NULL; track_count=0; javascript_free(program);
}

int main(void) {
    SLutf8_enable(1);
    SLtt_get_terminfo();
    assert(SLsmg_init_smg() == 0);
    /* Use an owned plane so the harness is independent of physical geometry. */
    ncplane_options plane_options={.rows=24,.cols=80,.name="render-test"};
    terminal_plane=ncplane_create(notcurses_stdplane(terminal_nc),&plane_options);
    assert(terminal_plane);
    SLtt_Screen_Cols=80;
    SLtt_Screen_Rows=24;
    SLtt_set_color(2,NULL,"yellow","black");
    SLtt_set_color(3,NULL,"brightcyan","black");
    SLtt_set_color(5,NULL,"brightblue","black");
    SLtt_set_color(6,NULL,"gray","black");
    SLtt_set_color(8,NULL,"brightgreen","black");
    SLtt_set_color(10,NULL,"black","yellow");
    SLtt_set_color(11,NULL,"black","brightcyan");
    /* Notcurses owns complete EGCs, including marks sent in separate writes. */
    SLsmg_gotorc(0,0);
    SLsmg_write_string("e"); SLsmg_write_string("\xcc\x81");
    char *egc=ncplane_at_yx(terminal_plane,0,0,NULL,NULL);
    assert(egc && !strcmp(egc,"e\xcc\x81")); free(egc);
    SLsmg_gotorc(0,1); SLsmg_write_string("界x");
    egc=ncplane_at_yx(terminal_plane,0,3,NULL,NULL);
    assert(egc && !strcmp(egc,"x")); free(egc);
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
    check_live_visual_addition();
    check_live_mode_highlights();
    check_visual_edit_highlights();
    check_stopped_highlight_growth();
    check_state_modes();
    check_theme_watch();
    check_input_readiness();
    SLsmg_reset_smg();
    puts("terminal highlight cells and indentation ok");
    return 0;
}
