/* Exercise editor transactions without initializing a terminal or audio. */
#define scheduler_start editor_test_scheduler_start
#include "../src/tui.c"
#undef scheduler_start
#include <assert.h>
#include <stdlib.h>

static int scheduler_starts;
void editor_test_scheduler_start(void) { scheduler_starts++; }

static void test_performance_activation(void) {
    scheduler_transport_set(0); event_queue_clear();
    assert(track_load_from_code("note(60).gain(slider(.2))"));
    assert(replace_code_with("note(72).gain(slider(.7))"));
    int starts=scheduler_starts;
    do_perform();
    assert(audio_is_playing() && scheduler_starts==starts+1);
    assert(strstr(current_code,"note(72)") && strstr(current_code,"slider(.7)"));
    JsProgram *program=tracks[0].javascript;
    tracks[0].js_cursor=.37;
    assert(replace_code_with("note(76).gain(slider(.8))"));
    do_perform();
    assert(audio_is_playing() && tracks[0].javascript!=program && tracks[0].js_cursor==.37);
    scheduler_transport_set(0);
    program=tracks[0].javascript;
    unsigned generation=event_queue_generation();
    starts=scheduler_starts;
    assert(replace_code_with("note(99); throw Error('bad performance edit')"));
    do_perform();
    assert(!audio_is_playing() && scheduler_starts==starts);
    assert(tracks[0].javascript==program && event_queue_generation()==generation);
    assert(strstr(status_msg,"Evaluation failed"));
    assert(replace_code_with("note(60).gain(0)"));
    assert(do_evaluate() && !audio_is_playing()); /* Document loading stays separate. */
}

static void vi_setup(const char *text) {
    assert(replace_code_with(text));
    undo_hist_clear(&undo_s,&undo_n); undo_hist_clear(&redo_s,&redo_n);
    memset(&vi,0,sizeof(vi)); vi_reset();
    vi_mode=vi_normal=1; editor_wrap=0; cur_line=cur_col=0; sel_active=0;
}
static void vi_keys(const char *keys) {
    while (*keys) { undo_tick++; assert(vi_handle_normal((unsigned char)*keys++)); }
}
static void vi_expect(const char *expected) {
    char *flat=editor_flatten(); assert(flat);
    size_t n=strlen(expected);
    if (strlen(flat)!=n+1 || memcmp(flat,expected,n) || flat[n]!='\n')
        fprintf(stderr,"vi buffer: wanted [%s], got [%s]\n",expected,flat);
    assert(strlen(flat)==n+1 && !memcmp(flat,expected,n) && flat[n]=='\n'); free(flat);
}

static void test_incomplete_slider_source(void) {
    const char text[]="s(\"sawtooth\").lpf(200).lpenv(slider(2.28,0,8))";
    assert(track_load_from_code("note(60).gain(0)"));
    char prefix[sizeof(text)];
    for (size_t length=0;length<sizeof(text);length++) {
        memcpy(prefix,text,length); prefix[length]=0;
        assert(replace_code_with(prefix));
        slider_refresh_sites();
        assert(slider_sites_count==0);
    }
    assert(track_load_from_code(text)); assert(replace_code_with(text));
    slider_refresh_sites(); assert(slider_sites_count==1);
    const char *partial[]={"slider()","slider(","slider(/* pending */)",
        "slider(2,)","slider(-)","slider(,0,8)","slider?.()"};
    for (size_t i=0;i<sizeof(partial)/sizeof(partial[0]);i++) {
        assert(replace_code_with(partial[i]));
        slider_refresh_sites();
        char *actual=editor_flatten(); assert(actual);
        assert(!strncmp(actual,partial[i],strlen(partial[i]))); free(actual);
    }
}

static void test_slider_source(void) {
    const char *text="// slider(.9) é\nconst label='slider(.8)';\nconst first=()=>slider(.2); const second=slider(.7);\nnote(60).gain(first()).pan(second)";
    assert(track_load_from_code(text));
    assert(replace_code_with(text));
    slider_refresh_sites();
    assert(slider_count()==2 && slider_sites_count==2);
    /* Evaluation order is second, then first. Source order must not bind them. */
    assert(slider_on_line(0,0)==-1 && slider_on_line(1,0)==-1);
    int first=slider_on_line(2,20), second=slider_on_line(2,(int)(strstr(code_lines[2],"slider(.7)")-code_lines[2]));
    assert(first==1 && second==0);
    assert(slider_set(first,.4) && slider_write_value(first));
    char *updated=editor_flatten(); assert(updated);
    assert(strstr(updated,"first=()=>slider(0.4)") && strstr(updated,"second=slider(.7)"));
    free(updated);
    /* New, unevaluated controls may not hijack an existing live binding. */
    assert(editor_replace(0,0,"note(99).gain(slider(.1));\n"));
    assert(slider_on_line(0,0)==-1);
    first=slider_on_line(3,20); assert(first==1);
    assert(slider_set(first,.6) && slider_write_value(first));
    updated=editor_flatten(); assert(updated);
    assert(strstr(updated,"slider(.1)") && strstr(updated,"first=()=>slider(0.6)"));
    free(updated);
    unsigned generation=tracks[0].compile_generation;
    assert(generation && slider_source_offset(first)>0);
    assert(track_load_from_code("const get=()=>slider(.3); stack(note(60).gain(get()),note(64).gain(get()))"));
    assert(slider_count()==1); /* One source site, shared by both calls. */
    assert(replace_code_with("note(60).gain(0)"));
    assert(track_load_from_code("note(60).gain(0)"));
}
static void test_vi_motions(void) {
    vi_setup("one.foo  two\n  three four");
    vi_keys("w"); assert(cur_col==3);
    vi_keys("w"); assert(cur_col==4);
    vi_keys("W"); assert(cur_col==9);
    vi_keys("e"); assert(cur_col==11);
    vi_keys("bB"); assert(cur_col==0);
    vi_keys("2G^"); assert(cur_line==1 && cur_col==2);
    vi_keys("$"); assert(cur_col==11);
    vi_keys("gg"); assert(cur_line==0 && cur_col==0);
    vi_setup("one two three four five six seven");
    vi_keys("2d3w"); vi_expect("seven");
    vi_keys("u"); vi_expect("one two three four five six seven");
    vi_keys("\022"); vi_expect("seven");
    vi_setup("one.two three"); vi_keys("de"); vi_expect(".two three");
    vi_setup("abc"); vi_keys("$dl"); vi_expect("ab");
    vi_setup("one two"); vi_keys("cw"); vi_expect(" two"); assert(!vi_normal);
    vi_setup("foo(\"hello\")"); cur_col=8; vi_keys("ci\""); vi_expect("foo(\"\")"); assert(!vi_normal);
    vi_setup("note(\"a\\\"b\")"); cur_col=9; vi_keys("ci\""); vi_expect("note(\"\")");
    vi_setup("foo(abc, [def]) tail"); cur_col=10; vi_keys("di["); vi_expect("foo(abc, []) tail");
    vi_setup("foo(abc, [def]) tail"); cur_col=10; vi_keys("da("); vi_expect("foo tail");
    vi_setup("é λ x"); vi_keys("ll"); assert(cur_col==3);
    vi_keys("h"); assert(cur_col==2);
    vi_keys("0x"); vi_expect(" λ x");
    vi_setup("abc, def, ghi"); vi_keys("f,"); assert(cur_col==3);
    vi_keys(";"); assert(cur_col==8);
    vi_keys(","); assert(cur_col==3);
    vi_keys("0dt,"); vi_expect(", def, ghi");
    vi_setup("abc, def, ghi"); vi_keys("df,"); vi_expect(" def, ghi");
    vi_setup("foo(a,(b))"); vi_keys("%"); assert(cur_col==9);
    vi_keys("%"); assert(cur_col==3);
    vi_setup("one two one\nsomeone one"); vi_keys("/one\r"); assert(cur_col==8);
    vi_keys("n"); assert(cur_line==1 && cur_col==4);
    vi_keys("N"); assert(cur_line==0 && cur_col==8);
    vi_keys("gg*"); assert(cur_col==8);
    vi_keys("n"); assert(cur_line==1 && cur_col==8);
    vi_setup("one\ntwo\nthree\nfour"); vi_keys("2dd"); vi_expect("three\nfour");
    vi_keys("Gdd"); vi_expect("three");
    vi_setup("one\ntwo\nthree"); vi_keys("2yy");
    assert(yank_lines && !strcmp(vi_yank,"one\ntwo\n") && cur_line==0);
    vi_setup("one\nline\n\nthree\n\nlast"); vi_keys("}"); assert(cur_line==2);
    vi_keys("}"); assert(cur_line==4);
    vi_keys("{"); assert(cur_line==2);
    vi_setup("one two"); cur_col=1; selection_start(0); vi_keys("iw");
    int a,b; selection_bounds(&a,&b); assert(a==0 && b==3);
    vi_setup("one two"); vi_keys("d\033w"); vi_expect("one two"); assert(cur_col==4);
    vi_setup("abc"); vi_keys("dfz"); vi_expect("abc"); assert(cur_col==0);
    vi_keys("99999dw"); vi_expect("");
    vi_setup("é"); vi_keys("9999w"); assert(cur_col==0);
    vi_setup("abc\ndef"); vi_keys("cc"); vi_expect("\ndef"); assert(!vi_normal);
    vi_setup("abc\ndef"); vi_keys("Gcc"); vi_expect("abc\n"); assert(!vi_normal);
    vi_setup("one\ntwo"); vi_keys("yyG2p"); vi_expect("one\ntwo\none\none");
    vi_mode=vi_normal=0;
}

static void completion_setup(const char *code, int at) {
    vi_setup(code); vi_mode=vi_normal=0; completion_reset();
    cur_col=at<0?(int)strlen(code_lines[0]):at;
}
static void test_completion(void) {
    completion_setup("note(60).ga(0)",11);
    assert(editor_complete(0,1)); vi_expect("note(60).gain(0)");
    assert(cur_col==13); assert(editor_undo()); vi_expect("note(60).ga(0)");
    completion_setup("note(60).gain(0)",11);
    assert(editor_complete(0,1)); vi_expect("note(60).gain(0)"); assert(cur_col==13);
    completion_setup("note(\"é\").ga",-1);
    assert(editor_complete(0,0)); vi_expect("note(\"é\").gain");
    completion_setup("note(60).f",-1);
    assert(editor_complete(0,1));
    char *first=editor_flatten(); assert(first);
    assert(editor_complete(0,1));
    char *second=editor_flatten(); assert(second && strcmp(first,second));
    assert(editor_complete(1,1));
    char *back=editor_flatten(); assert(back && !strcmp(first,back));
    free(first); free(second); free(back);
    assert(editor_undo()); vi_expect("note(60).f");
    completion_setup("// .ga",-1); assert(!editor_complete(0,1)); vi_expect("// .ga");
    completion_setup("/* comment\n.ga",-1); cur_line=1; cur_col=3;
    assert(!editor_complete(0,1)); vi_expect("/* comment\n.ga");
    completion_setup("note(60).s(\"squ",-1);
    assert(editor_complete(0,1)); vi_expect("note(60).s(\"square");
    completion_setup("bp",-1); assert(editor_complete(0,1)); vi_expect("bpm");
    completion_setup("note(60).",-1); assert(editor_complete(0,0)); vi_expect("note(60).");
    assert(completion.count>1 && strstr(status_msg,"Tab cycles"));
    completion_setup("note c4 | sl",-1); assert(editor_complete(0,1)); vi_expect("note c4 | slow");
}
static void test_commands(void) {
    char root[]="/tmp/tompouce-ex-XXXXXX", file[1024], missing[1024], saved[1024];
    assert(mkdtemp(root));
    const char *old=getenv("XDG_CONFIG_HOME"); char *keep=old?strdup_safe(old):NULL;
    assert(setenv("XDG_CONFIG_HOME",root,1)==0);
    const char *old_home=getenv("HOME"); char *home=old_home?strdup_safe(old_home):NULL;
    assert(setenv("HOME",root,1)==0);
    assert(command_path("~/My song.json",file,sizeof(file)));
    snprintf(saved,sizeof(saved),"%s/My song.json",root); assert(!strcmp(file,saved));
    assert(home?setenv("HOME",home,1)==0:unsetenv("HOME")==0); free(home);
    vi_setup("one\ntwo\nthree");
    free(editor_written); editor_written=editor_flatten(); assert(editor_written);
    command_execute("2"); assert(cur_line==1 && cur_col==0);
    command_execute("$"); assert(cur_line==2);
    command_execute("99999"); assert(cur_line==2 && strstr(status_msg,"E16"));
    command_prompt_open(); assert(input_mode==2 && !input_buf[0]);
    snprintf(input_buf,sizeof(input_buf),"set no"); input_len=(int)strlen(input_buf);
    assert(command_complete(0) && !strcmp(input_buf,"set nowrap"));
    assert(command_complete(0) && !strcmp(input_buf,"set nomouse"));
    assert(command_complete(1) && !strcmp(input_buf,"set nowrap"));
    command_execute(input_buf); assert(!editor_wrap);
    completion_setup("before",-1);
    free(editor_written); editor_written=editor_flatten(); assert(editor_written);
    assert(editor_replace(0,6,"after"));
    command_execute("q"); assert(!quit_requested && strstr(status_msg,"E37"));
    mark_editor_clean(); /* F5's evaluated baseline must not clear file dirtiness. */
    command_execute("q"); assert(!quit_requested && strstr(status_msg,"E37"));
    command_execute("tutor"); assert(strstr(status_msg,"E37")); vi_expect("after");
    do_command_or_ai("/tutor"); assert(strstr(status_msg,"E37")); vi_expect("after");
    command_execute("tutor! extra"); assert(strstr(status_msg,"E488")); vi_expect("after");
    do_command_or_ai("/tutor! extra"); assert(strstr(status_msg,"usage:")); vi_expect("after");
    snprintf(saved,sizeof(saved),"%s",project_path); project_path[0]=0;
    assert(!project_save(NULL) && strstr(status_msg,"No filename"));
    snprintf(project_path,sizeof(project_path),"%s",saved);
    snprintf(file,sizeof(file),"%s/My song.json",root);
    snprintf(saved,sizeof(saved),"%s",project_path);
    snprintf(missing,sizeof(missing),"%s/missing/song.json",root);
    assert(!project_save(missing) && !strcmp(project_path,saved));
    char command[1200]; snprintf(command,sizeof(command),"wq %s",missing);
    command_execute(command); assert(!quit_requested && strstr(status_msg,"save failed"));
    snprintf(command,sizeof(command),"wr \"%s\"",file);
    command_execute(command); assert(!command_modified() && !strcmp(project_path,file));
    assert(editor_replace(0,5,"changed"));
    command_execute("undo"); vi_expect("after");
    command_execute("redo"); vi_expect("changed");
    snprintf(command,sizeof(command),"e %s",file);
    command_execute(command); assert(strstr(status_msg,"E37")); vi_expect("changed");
    snprintf(command,sizeof(command),"e! %s",missing);
    command_execute(command); assert(!strcmp(project_path,file)); vi_expect("changed");
    command_execute("!touch /tmp/should-not-run"); assert(strstr(status_msg,"E492"));
    input_mode=0; completion_reset(); quit_requested=0;
    assert(unlink(file)==0);
    char config[1200]; snprintf(config,sizeof(config),"%s/tompouce/config.json",root); assert(unlink(config)==0);
    snprintf(config,sizeof(config),"%s/tompouce",root); assert(rmdir(config)==0);
    assert(rmdir(root)==0);
    assert(keep?setenv("XDG_CONFIG_HOME",keep,1)==0:unsetenv("XDG_CONFIG_HOME")==0); free(keep);
    snprintf(project_path,sizeof(project_path),"tompouce-song.json");
}

static void test_tutor_copy(void) {
    char root[]="/tmp/tompouce-tutor-copy-XXXXXX",master[512],working[512],other[512],path[1024];
    assert(mkdtemp(root));
    snprintf(master,sizeof(master),"%s/master.json",root);
    snprintf(working,sizeof(working),"%s/working.json",root);
    snprintf(other,sizeof(other),"%s/other.json",root);
    FILE *file=fopen(master,"w"); assert(file && fputs("master",file)>=0 && !fclose(file));
    assert(tutor_copy(master,working));
    struct stat st; assert(!stat(working,&st) && !(st.st_mode&077));
    char content[32]={0}; file=fopen(working,"r");
    assert(file && fread(content,1,sizeof(content)-1,file)==6 && !fclose(file) && !strcmp(content,"master"));
    file=fopen(working,"w"); assert(file && fputs("progress",file)>=0 && !fclose(file));
    assert(tutor_copy(other,working)); /* Existing progress survives even without the master. */
    memset(content,0,sizeof(content)); file=fopen(working,"r");
    assert(file && fread(content,1,sizeof(content)-1,file)==8 && !fclose(file) && !strcmp(content,"progress"));
    assert(!tutor_copy(other,other) && access(other,F_OK));
    assert(!symlink(master,other)); assert(!tutor_copy(master,other)); assert(!unlink(other));
    assert(!link(master,other)); assert(!tutor_copy(master,other)); assert(!unlink(other));
    char keep[sizeof(project_path)]; snprintf(keep,sizeof(keep),"%s",project_path);
    snprintf(project_path,sizeof(project_path),"%s",working);
    file_mode=1; snprintf(file_buf,sizeof(file_buf),"working.json");
    assert(file_pick_path(path,sizeof(path)) && !strcmp(path,working));
    snprintf(file_buf,sizeof(file_buf),"copy.json");
    assert(file_pick_path(path,sizeof(path)) && !strcmp(path,"copy.json"));
    file_mode=2; snprintf(file_buf,sizeof(file_buf),"working.json");
    assert(file_pick_path(path,sizeof(path)) && !strcmp(path,"working.json"));
    file_mode=0; snprintf(project_path,sizeof(project_path),"%s",keep);
    assert(!unlink(master) && !unlink(working) && !rmdir(root));
}

int main(void) {
    graphics_supported = 1;
    assert(queue_inline_graphic(0,1,2,5,6,40,1,123));
    assert(queue_inline_graphic(0,1,3,6,6,40,1,123));
    assert(graphics_pending_count == 1 && graphics_pending[0].first == 1 && graphics_pending[0].count == 2);
    assert(queue_inline_graphic(0,VISUAL_PIANOROLL,1,8,6,40,2,123));
    assert(graphics_pending_count == 2); /* Scope and roll on one line stay separate. */
    graphics_disabled = 1;
    assert(!queue_inline_graphic(1,1,1,10,6,40,1,123));
    graphics_disabled = 0;
    for (int i = 2; i < GRAPHICS_IMAGES; i++) assert(queue_inline_graphic(i,1,1,10+i,6,40,1,123));
    assert(!queue_inline_graphic(99,1,1,24,6,40,1,123)); /* Extra widgets retain braille. */
    graphics_supported = 0; graphics_pending_count = 0;
    char root[] = "/tmp/tompouce-config-XXXXXX", path[1024], expected[1024];
    const char *original = getenv("XDG_CONFIG_HOME");
    char *saved = original ? strdup_safe(original) : NULL;
    assert(!original || saved);
    assert(mkdtemp(root) && setenv("XDG_CONFIG_HOME",root,1) == 0);
    user_dir(path,sizeof(path),"XDG_CONFIG_HOME","tompouce",".config/tompouce");
    snprintf(expected,sizeof(expected),"%s/tompouce",root);
    assert(strcmp(path,expected) == 0);
    snprintf(path,sizeof(path),"%s/strudelterm",root);
    assert(mkdir(path,0700) == 0);
    user_dir(path,sizeof(path),"XDG_CONFIG_HOME","tompouce/sessions",".config/tompouce/sessions");
    snprintf(expected,sizeof(expected),"%s/tompouce/sessions",root);
    assert(strcmp(path,expected) == 0);
    snprintf(path,sizeof(path),"%s/strudelterm",root);
    assert(mkdir(path,0700) == 0);
    snprintf(path,sizeof(path),"%s/tompouce",root);
    user_dir(path,sizeof(path),"XDG_CONFIG_HOME","tompouce/sounds",".config/tompouce/sounds");
    snprintf(expected,sizeof(expected),"%s/tompouce/sounds",root);
    assert(strcmp(path,expected) == 0);
    snprintf(path,sizeof(path),"%s/tompouce",root); assert(rmdir(path) == 0);
    snprintf(path,sizeof(path),"%s/strudelterm",root); assert(rmdir(path) == 0);
    assert(rmdir(root) == 0);
    assert(saved ? setenv("XDG_CONFIG_HOME",saved,1) == 0 : unsetenv("XDG_CONFIG_HOME") == 0);
    free(saved);
    replace_code_with("one\ntwo\nthree");
    cur_line = 0; cur_col = 1; selection_start(0);
    cur_line = 2; cur_col = 2;
    selection_copy(0);
    assert(strcmp(vi_yank, "ne\ntwo\nth") == 0);
    cur_line = 0; cur_col = 1; selection_start(0);
    cur_line = 2; cur_col = 2;
    selection_copy(1);
    assert(code_line_count == 1 && strcmp(code_lines[0], "oree") == 0);
    editor_paste(1);
    assert(code_line_count == 3);
    assert(strcmp(code_lines[0], "one") == 0);
    assert(strcmp(code_lines[2], "three") == 0);

    cur_line = 2; cur_col = 3; selection_start(1);
    cur_line = 0; cur_col = 0;
    selection_copy(0);
    assert(yank_lines && strcmp(vi_yank, "one\ntwo\nthree\n") == 0);
    vi_mode = vi_normal = 1;
    cur_line = 2;
    editor_paste(0);
    assert(code_line_count == 6 && strcmp(code_lines[5], "three") == 0);

    assert(!editor_replace(-1, 0, "nope"));
    assert(code_line_count == 6 && strcmp(code_lines[0], "one") == 0);
    char *wide = malloc(3000);
    assert(wide);
    memset(wide, 'x', 2999);
    wide[2999] = 0;
    assert(editor_replace(0, 0, wide));
    free(wide);
    assert(code_line_count == 6);
    assert((int)strlen(code_lines[0]) == 2999 + 3);
    assert(memcmp(code_lines[0], "xxx", 3) == 0);
    assert(strcmp(code_lines[0] + 2999, "one") == 0);
    assert(editor_replace(0, 0, "new\n"));
    assert(code_line_count == 7 && strcmp(code_lines[0], "new") == 0);

    replace_code_with("one\ntwo");
    cur_line = 1; selection_start(1); selection_copy(1);
    assert(code_line_count == 1 && strcmp(code_lines[0], "one") == 0);
    assert(strcmp(vi_yank, "two\n") == 0);
    cur_line = 0; cur_col = 1; selection_start(0); sel_inclusive = 1;
    selection_copy(0);
    assert(strcmp(vi_yank, "n") == 0);

    hl_quote = 0;
    highlight_context("note(`c4 // still music");
    assert(hl_quote == '`');
    highlight_context("e4`).gain(1) // comment's quote");
    assert(hl_quote == 0);
    highlight_context("// don't open a quote");
    assert(hl_quote == 0);
    highlight_context("/* don't open a quote");
    assert(hl_quote == 0 && hl_block_comment);
    highlight_context("# block comment */ note(\"c4\")");
    assert(hl_quote == 0 && !hl_block_comment);
    highlight_context("note(\"unfinished");
    assert(hl_quote == 0);
    highlight_context("const value = 'unfinished");
    assert(hl_quote == 0);
    highlight_context("note(\"continued\\");
    assert(hl_quote == '"');
    highlight_context("c4\")");
    assert(hl_quote == 0);

    slider_begin();
    slider_bind(.5, 0, 1, .1);
    assert(slider_set(0, .77));
    double v;
    assert(slider_info(0, &v, NULL, NULL, NULL) && fabs(v - .8) < 1e-9);
    assert(slider_set(0, 99));
    assert(slider_info(0, &v, NULL, NULL, NULL) && v == 1);
    assert(!slider_set(0, NAN));

    char *longline = malloc(4096);
    assert(longline);
    memset(longline, 'a', 4095);
    longline[4095] = 0;
    replace_code_with("");
    assert(editor_replace(0, 0, longline));
    assert((int)strlen(code_lines[0]) == 4095);
    free(longline);

    char *many = malloc((size_t)2500 * 40 + 1);
    assert(many);
    size_t used = 0;
    for (int i = 0; i < 2500; i++) {
        memcpy(many + used, "0123456789abcdef0123456789abcdef0123456\n", 40);
        used += 40;
    }
    many[used] = 0;
    assert(used > 65536);
    replace_code_with(many);
    assert(code_line_count == 2501);
    free(many);

    int row = 0, cell = 0;
    editor_place("abcdef", 6, 4, &row, &cell);
    assert(row == 1 && cell == 2);
    editor_place("abcdef", 4, 4, &row, &cell);
    assert(row == 1 && cell == 0);
    editor_place("abcd", 4, 4, &row, &cell);
    assert(row == 1 && cell == 0);
    editor_place("", 0, 4, &row, &cell);
    assert(row == 0 && cell == 0);
    editor_place("\t", 1, 8, &row, &cell);
    assert(row == 0 && cell == 2);
    assert(editor_content_rows("abcd", 4) == 1);
    assert(editor_content_rows("abcdef", 4) == 2);
    assert(editor_content_rows("\xe4\xb8\x80X", 2) == 2);
    editor_place("\xe4\xb8\x80", 3, 2, &row, &cell);
    assert(row == 1 && cell == 0);
    assert(editor_byte_at("abcdef", 1, 1, 4) == 5);
    assert(editor_content_rows("hello world", 8) == 2);
    editor_place("hello world", 6, 8, &row, &cell);
    assert(row == 1 && cell == 0);
    assert(editor_content_rows("note(\"c3\")", 20) == 1);
    assert(editor_content_rows("xx \"abcd\"", 6) == 2);
    editor_place("xx \"abcd\"", 3, 6, &row, &cell);
    assert(row == 1 && cell == 0);
    assert(editor_content_rows("note.gain", 6) == 2);
    editor_place("note.gain", 4, 6, &row, &cell);
    assert(row == 1 && cell == 0);
    assert(editor_content_rows("1.5", 3) == 1);
    assert(editor_content_rows("a.b.c", 3) == 2);
    editor_place("a.b.c", 3, 3, &row, &cell);
    assert(row == 1 && cell == 0);
    assert(editor_content_rows("note(\"c3 e3 g3\")", 12) == 2);
    editor_place("note(\"c3 e3 g3\")", 5, 12, &row, &cell);
    assert(row == 1 && cell == 0);
    replace_code_with("abcd");
    cur_line = 0;
    cur_col = 4;
    assert(line_screen_rows(0, 4) == 2);
    cur_col = 0;
    assert(line_screen_rows(0, 4) == 1);
    editor_wrap = 0;
    assert(line_screen_rows(0, 4) == 1);
    editor_wrap = 1;

    undo_hist_clear(&undo_s, &undo_n);
    undo_hist_clear(&redo_s, &redo_n);
    undo_group = 0;
    undo_suspend = 1;
    assert(replace_code_with("ab"));
    undo_suspend = 0;
    assert(undo_n == 0);
    cur_line = 0;
    cur_col = 2;
    undo_tick = 1;
    undo_begin(UNDO_TYPE);
    assert(line_splice(0, 2, 0, "c", 1));
    cur_col = 3;
    undo_commit();
    undo_tick = 2;
    undo_begin(UNDO_TYPE);
    assert(line_splice(0, 3, 0, "d", 1));
    cur_col = 4;
    undo_commit();
    assert(undo_n == 1);
    assert(strcmp(code_lines[0], "abcd") == 0);
    assert(editor_undo());
    assert(strcmp(code_lines[0], "ab") == 0);
    assert(cur_line == 0 && cur_col == 2);
    assert(editor_redo());
    assert(strcmp(code_lines[0], "abcd") == 0);
    assert(cur_col == 4);
    assert(!editor_redo());
    undo_tick = undo_group_tick + 1;
    undo_group_time = now_seconds() - 5.0;
    undo_begin(UNDO_TYPE);
    assert(line_splice(0, 4, 0, "e", 1));
    cur_col = 5;
    undo_commit();
    assert(undo_n == 2);
    assert(strcmp(code_lines[0], "abcde") == 0);
    assert(editor_undo());
    assert(redo_n == 1);
    assert(editor_replace(0, 0, "Q"));
    assert(redo_n == 0);
    assert(code_lines[0][0] == 'Q');
    assert(editor_undo());
    assert(code_lines[0][0] != 'Q');
    int depth = undo_n;
    assert(!editor_replace(-1, 0, "z"));
    assert(undo_n == depth);
    undo_hist_clear(&undo_s, &undo_n);
    undo_hist_clear(&redo_s, &redo_n);
    assert(!editor_undo());
    assert(!editor_redo());
    replace_code_with("note('c4')._scope()\nnote('e4')\n// x._scope()\nconst text = 'x._pianoroll()';\n/* x._scope()\nx._pianoroll() */\nnote('g4')._pianoroll()._scope()\nobj._scope;\nnote('c4')._scope(\n{})");
    inline_visuals = 1;
    inline_refresh();
    assert(inline_mask(0) == 1 && inline_rows(0) == 4);
    for (int i = 1; i < 6; i++) assert(!inline_mask(i));
    assert(inline_mask(6) == 3 && inline_rows(6) == 9);
    graphics_supported = 1;
    assert(inline_rows(6) == 13 && inline_piano_rows() == 8);
    graphics_disabled = 1;
    assert(inline_rows(6) == 9 && inline_piano_rows() == 4);
    graphics_supported = graphics_disabled = 0;
    assert(!inline_mask(7) && !inline_mask(8) && inline_mask(9) == 1);
    assert(editor_footer(32) == 0);
    cols = 120; rows = 32; split_ratio = .6;
    editor_wrap = 1; cur_line = 0; cur_col = 0;
    assert(line_screen_rows(0,64) == 5);
    editor_move_rows(1);
    assert(cur_line == 1); // Keyboard movement skips display-only rows.
    editor_move_rows(-1);
    assert(cur_line == 0);
    editor_wrap = 0; cur_line = 1; view_top = 0;
    editor_reveal(3,72);
    assert(view_top == 3); // Horizontal-scroll mode uses the same visual layout.
    inline_visuals = 0;
    assert(line_screen_rows(0,64) == 1);
    assert(editor_footer(32) == 2);
    inline_visuals = 1;
    unsigned char dots[4][240] = {{0}};
    for (int x = 0; x < 2; x++) for (int y = 0; y < 4; y++) inline_dot(dots,x,y,1,4);
    assert(dots[0][0] == 255);
    inline_dot(dots,2,4,1,4);
    assert(dots[1][1] == 0);
    test_vi_motions();
    test_completion();
    test_commands();
    test_tutor_copy();
    test_performance_activation();
    test_slider_source();
    test_incomplete_slider_source();
    puts("editor, vi motions and inline braille layout ok");
    return 0;
}
