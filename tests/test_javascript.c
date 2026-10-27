#include "javascript.h"
#include "js_audio.h"
#include "slider.h"
#include "common.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static cJSON *query(const char *code, JsProgram **program);

static void test_scope_defaults(void) {
    const char *code="note(60)._scope()";
    JsProgram *program; cJSON *haps=query(code,&program); cJSON_Delete(haps);
    unsigned mask=javascript_live_scope_mask(program,code,(int)(strstr(code,"_scope")-code),VISUAL_SCOPE);
    VisualOptions options;
    assert(mask && javascript_live_visual_options(program,mask,VISUAL_SCOPE,&options));
    assert(options.scale==1 && options.pos==.5 && options.thickness==3);
    const char *editors[]={"note(60).scope()","note(60)._scope({scale:0,pos:.25})",
        "note(60)._scope({scale:-.25})","note(60)._scope()"};
    const double scales[]={.25,0,-.25,1},positions[]={.75,.25,.5,.5};
    for (int i=0;i<4;i++) {
        javascript_request_visuals(program,editors[i]); javascript_update_visuals(program,.1,0,2);
        const char *name=strstr(editors[i],i==0?"scope":"_scope");
        mask=javascript_live_scope_mask(program,editors[i],(int)(name-editors[i]),VISUAL_SCOPE);
        assert(mask && javascript_live_visual_options(program,mask,VISUAL_SCOPE,&options));
        assert(options.scale==scales[i] && options.pos==positions[i]);
    }
    javascript_free(program);
}

static void test_slider_highlights(void) {
    const char *source="const level=slider(.5); note(\"c4 e4\").gain(level)._scope()";
    JsProgram *program; cJSON *haps=query(source,&program);
    javascript_activate_sliders(program);
    assert(slider_set(0,.6));
    const char *editor="const level=slider(0.6); note(\"c4 e4\").gain(level)._scope()";
    javascript_cache_spans(program,haps,0,2);
    javascript_request_visuals(program,editor); javascript_update_visuals(program,.1,0,2);
    assert(javascript_live_scope_mask(program,editor,(int)(strstr(editor,"_scope")-editor),VISUAL_SCOPE));
    int offsets[8], lengths[8];
    assert(javascript_editor_play_styles(program,editor,.1,offsets,lengths,NULL,NULL,8)>0);
    assert(offsets[0]==(int)(strstr(editor,"c4 e4")-editor));
    const char *unapplied="const level=slider(0.9); note(\"c4 e4\").gain(level)._scope()";
    javascript_request_visuals(program,unapplied); javascript_update_visuals(program,.1,0,2);
    assert(!javascript_editor_play_styles(program,unapplied,.1,offsets,lengths,NULL,NULL,8));
    cJSON_Delete(haps); javascript_free(program);
}

static void test_live_visuals(void) {
    const char *code="globalThis.liveRuns=(globalThis.liveRuns??0)+1; $: note(60+liveRuns).slow(4); $: note(67)";
    JsProgram *program; cJSON *before=query(code,&program);
    const char *editor="// é\nglobalThis.liveRuns=99; setcpm(99); while(true){}; $: note(60+liveRuns)._pianoroll({cycles:2,vertical:1}).slow(4); $: note(67)._scope()";
    javascript_request_visuals(program,editor);
    javascript_update_visuals(program,1,0,2);
    unsigned roll=javascript_live_scope_mask(program,editor,(int)(strstr(editor,"_pianoroll")-editor),VISUAL_PIANOROLL);
    unsigned scope=javascript_live_scope_mask(program,editor,(int)(strstr(editor,"_scope")-editor),VISUAL_SCOPE);
    assert(roll && scope && roll!=scope && javascript_visual_revision(program));
    VisualOptions options; assert(javascript_live_visual_options(program,roll,VISUAL_PIANOROLL,&options));
    assert(options.cycles==2 && options.vertical);
    VisualNote notes[64];
    int count=javascript_live_visual_copy(program,roll,VISUAL_PIANOROLL,notes,64,NULL,NULL);
    assert(count>0);
    for (int i=0;i<count;i++) assert(notes[i].pitch==61 && notes[i].end-notes[i].start==8);
    cJSON *after=javascript_query(program,0,1), *audio=javascript_query_audio(program,0,1,.5);
    char *a=cJSON_PrintUnformatted(before), *b=cJSON_PrintUnformatted(after);
    assert(a && b && !strcmp(a,b)); free(a); free(b);
    assert(((unsigned)cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(audio,0),"scopeMask")->valuedouble&roll)!=0);
    assert(((unsigned)cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(audio,1),"scopeMask")->valuedouble&roll)==0);
    cJSON_Delete(audio); cJSON_Delete(before); cJSON_Delete(after); javascript_free(program);

    before=query("const melody=note(60).slow(4); melody",&program);
    editor="const melody=note(60).slow(4); melody._pianoroll()._punchcard()";
    javascript_request_visuals(program,editor); javascript_update_visuals(program,1,0,2);
    roll=javascript_live_scope_mask(program,editor,(int)(strstr(editor,"_pianoroll")-editor),VISUAL_PIANOROLL);
    unsigned card=javascript_live_scope_mask(program,editor,(int)(strstr(editor,"_punchcard")-editor),VISUAL_PUNCHCARD);
    assert(roll && roll==card);
    count=javascript_live_visual_copy(program,roll,VISUAL_PIANOROLL,notes,64,NULL,NULL);
    int cards=javascript_live_visual_copy(program,card,VISUAL_PUNCHCARD,notes,64,NULL,NULL);
    assert(count>0 && count==cards); /* Each view receives one copy, sharing the audio feed. */
    javascript_request_visuals(program,"const melody=note(60).slow(4); melody");
    javascript_update_visuals(program,1,0,2);
    assert(!javascript_live_scope_mask(program,editor,(int)(strstr(editor,"_pianoroll")-editor),VISUAL_PIANOROLL));
    cJSON_Delete(before); javascript_free(program);

    code="const options={cycles:2,vertical:1}; note(60)._pianoroll(options)";
    before=query(code,&program);
    editor="// moved\nconst options={cycles:8,vertical:0}; note(60)._pianoroll(options)";
    javascript_request_visuals(program,editor); javascript_update_visuals(program,1,0,2);
    roll=javascript_live_scope_mask(program,editor,(int)(strstr(editor,"_pianoroll")-editor),VISUAL_PIANOROLL);
    assert(roll && javascript_live_visual_options(program,roll,VISUAL_PIANOROLL,&options));
    assert(options.cycles==2 && options.vertical); /* Variables retain evaluated values until F5. */
    cJSON_Delete(before); javascript_free(program);
}

static void test_visual_highlight_mapping(void) {
    const char *code="// café♫\nnote(\"c4 e4\").color(\"red blue\").markcss('font-weight:bold')";
    const char *editor="// 🔊\nnote(\"c4 e4\")._scope({scale:2}).color(\"red blue\").markcss('font-weight:bold')";
    JsProgram *program; cJSON *haps=query(code,&program);
    javascript_cache_spans(program,haps,0,2); cJSON_Delete(haps);
    javascript_request_visuals(program,editor); javascript_update_visuals(program,.1,0,2);
    int off[8],len[8]; uint32_t colors[8]; unsigned flags[8];
    int count=javascript_editor_play_styles(program,editor,.1,off,len,colors,flags,8);
    assert(count>0);
    int expected=(int)(strstr(editor,"c4 e4")-editor), found=0;
    for (int i=0;i<count;i++) if (off[i]==expected) {
        assert(len[i]==2 && colors[i]==0xff0000ffu && (flags[i]&VISUAL_BOLD)); found=1;
    }
    assert(found);
    count=javascript_editor_play_styles(program,editor,1.1,off,len,colors,flags,8); found=0;
    for (int i=0;i<count;i++) if (off[i]==expected+3) {
        assert(len[i]==2 && colors[i]==0x0000ffffu && (flags[i]&VISUAL_BOLD)); found=1;
    }
    assert(found);
    const char *changed="// 🔊\nnote(\"c5 e4\")._scope({scale:2}).color(\"red blue\").markcss('font-weight:bold')";
    javascript_request_visuals(program,changed); javascript_update_visuals(program,.1,0,2);
    assert(!javascript_editor_play_styles(program,changed,.1,off,len,colors,flags,8));
    assert(javascript_editor_play_styles(program,code,.1,off,len,colors,flags,8)>0); /* Undo can restore original offsets immediately. */
    javascript_free(program);

    haps=query("note(`c4 e4`).gain(0)",&program); javascript_cache_spans(program,haps,0,2); cJSON_Delete(haps);
    editor="all(scope); note(`c4 e4`).gain(0)";
    javascript_request_visuals(program,editor); javascript_update_visuals(program,.1,0,2);
    count=javascript_editor_play_styles(program,editor,.1,off,len,colors,flags,8);
    assert(count==1 && off[0]==(int)(strstr(editor,"c4")-editor) && len[0]==2);
    /* Musical labels are not display-only; changing .p() must not inherit old spans. */
    changed="all(scope); note(`c4 e4`).gain(0).p('_muted')";
    javascript_request_visuals(program,changed); javascript_update_visuals(program,.1,0,2);
    assert(!javascript_editor_play_styles(program,changed,.1,off,len,colors,flags,8));
    javascript_free(program);
}

static void write_module(const char *path, const char *source) {
    FILE *file = fopen(path,"w");
    assert(file && fputs(source,file) >= 0 && fclose(file) == 0);
}

static void test_prebake(void) {
    char root[]="/tmp/tompouce-prebake-XXXXXX",path[512],linked[1024];
    assert(mkdtemp(root));
    const char *previous=getenv("XDG_CONFIG_HOME");
    char *saved=previous?strdup(previous):NULL;
    assert(!previous || saved);
    assert(!setenv("XDG_CONFIG_HOME",root,1));
    snprintf(path,sizeof(path),"%s/tompouce",root); assert(!mkdir(path,0700));
    JsProgram *program; cJSON *haps=query("note(60)",&program);
    cJSON_Delete(haps); javascript_free(program);
    char error[1024]; cJSON *metadata=NULL;
    char directory[512]; snprintf(directory,sizeof(directory),"%s/tompouce/prebakes",root);
    snprintf(linked,sizeof(linked),"%s/outside",root); assert(!mkdir(linked,0700));
    assert(!symlink(linked,directory));
    assert(!javascript_store_prebake("void 0",error,sizeof(error)));
    assert(!unlink(directory) && !rmdir(linked));
    const char *starter="window.UISlider=()=>{throw Error('DOM control must be skipped')};\n"
        "window.UISlider(()=>{},'Volume',.8); window.fixtureCount=(window.fixtureCount??0)+1;\n"
        "setGainCurve(x=>x*x); setDefault('gain',1); $: note(99);\n";
    assert(javascript_store_prebake(starter,error,sizeof(error)));
    assert(!javascript_store_prebake("throw Error('must not replace')",error,sizeof(error)));
    haps=query("await loadPrebake('switchangel.strudel'); await loadPrebake('switchangel.strudel'); note(fixtureCount).gain(.5)",&program);
    const cJSON *value=cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(haps,0),"value");
    assert(cJSON_GetObjectItemCaseSensitive(value,"note")->valueint==1);
    assert(cJSON_GetObjectItemCaseSensitive(value,"gain")->valuedouble==.25);
    cJSON_Delete(haps); javascript_free(program);
    haps=query("await loadPrebake('switchangel.strudel'); $: note(60); $: note(64)",&program);
    assert(cJSON_GetArraySize(haps)==2); cJSON_Delete(haps); javascript_free(program);
    const char *rejected[]={"await loadPrebake('../prebake.strudel'); note(60)",
        "await loadPrebake('/etc/passwd'); note(60)","await loadPrebake('switchangel.strudel\\0x'); note(60)",
        "for(let i=0;i<17;i++) __hostPrebake('switchangel.strudel'); note(60)"};
    for (size_t i=0;i<sizeof(rejected)/sizeof(*rejected);i++) {
        program=javascript_compile(rejected[i],120,&metadata,error,sizeof(error)); assert(!program && !metadata);
    }
    snprintf(linked,sizeof(linked),"%s/link.strudel",directory); assert(!symlink("switchangel.strudel",linked));
    program=javascript_compile("await loadPrebake('link.strudel'); note(60)",120,&metadata,error,sizeof(error));
    assert(!program); assert(!unlink(linked));
    program=javascript_compile("new Pattern(()=>{__hostPrebake('switchangel.strudel');return []})",120,&metadata,error,sizeof(error));
    assert(program); cJSON_Delete(metadata); metadata=NULL;
    haps=javascript_query(program,0,1);
    assert(!haps || !cJSON_GetArraySize(haps)); cJSON_Delete(haps); javascript_free(program);
    for (int i=0;i<5;i++) {
        snprintf(linked,sizeof(linked),"%s/big%d.strudel",directory,i);
        FILE *file=fopen(linked,"w"); assert(file);
        char *padding=malloc(256*1024); assert(padding); memset(padding,' ',256*1024); padding[0]=padding[1]='/';
        assert(fwrite(padding,1,256*1024,file)==256*1024 && !fclose(file)); free(padding);
    }
    program=javascript_compile("for(let i=0;i<5;i++) await loadPrebake('big'+i+'.strudel'); note(60)",120,&metadata,error,sizeof(error));
    assert(!program && strstr(error,"1 MiB"));
    for (int i=0;i<5;i++) { snprintf(linked,sizeof(linked),"%s/big%d.strudel",directory,i); assert(!unlink(linked)); }
    snprintf(linked,sizeof(linked),"%s/switchangel.strudel",directory); assert(!unlink(linked)); assert(!rmdir(directory));
    snprintf(path,sizeof(path),"%s/tompouce/prebake.strudel",root);
    write_module(path,
        "await Promise.resolve(); const pitches = \"60 64\";\n"
        "window.preLead = () => note(pitches);\n"
        "register('preDouble', pat => pat.fast(2));\n"
        "window.preCount = (window.preCount ?? 0)+1;\n"
        "setcps(.75); samples({preSound:['https://example.com/pre.wav']});\n"
        "$: note(99);\n");
    program=javascript_compile("preLead().preDouble()",120,&metadata,error,sizeof(error));
    if (!program) fprintf(stderr,"prebake error: %s\n",error);
    assert(program && metadata);
    assert(cJSON_GetObjectItemCaseSensitive(metadata,"bpm")->valuedouble==180);
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(metadata,"samples"))==1);
    cJSON_Delete(metadata); metadata=NULL;
    haps=javascript_query(program,0,1);
    if (!haps || cJSON_GetArraySize(haps)!=4) {
        char *dump=haps?cJSON_PrintUnformatted(haps):NULL;
        fprintf(stderr,"prebake haps: %s\n",dump?dump:"null"); free(dump);
    }
    assert(haps && cJSON_GetArraySize(haps)==4);
    assert(!cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(haps,0),"locations")));
    cJSON_Delete(haps);
    /* Editing the file does not mutate an accepted runtime. Reevaluation
       reads the replacement and uses a fresh set of globals/prototypes. */
    write_module(path,"window.preLead=()=>note(72); window.preCount=(window.preCount??0)+1;");
    haps=javascript_query(program,0,1); assert(cJSON_GetArraySize(haps)==4);
    cJSON_Delete(haps); javascript_free(program);
    haps=query("note(preCount)",&program);
    assert(cJSON_GetArraySize(haps)==1);
    assert(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(haps,0),"value"),"note")->valueint==1);
    cJSON_Delete(haps); javascript_free(program);
    haps=query("preLead()",&program); assert(cJSON_GetArraySize(haps)==1);
    cJSON_Delete(haps); javascript_free(program);
    write_module(path,"// tompouce-prebake: switchangel\nwindow.UISlider=()=>{throw Error('unsupported widget')};window.UISlider(()=>{},'Other',.5)");
    program=javascript_compile("note(60)",120,&metadata,error,sizeof(error));
    assert(!program && strstr(error,"unsupported widget"));
    write_module(path,"register('oops',");
    program=javascript_compile("note(60)",120,&metadata,error,sizeof(error));
    assert(!program && !metadata && strstr(error,"prebake.strudel"));
    write_module(path,"throw new Error('fixture setup failed')");
    program=javascript_compile("note(60)",120,&metadata,error,sizeof(error));
    if (program || !strstr(error,"fixture setup failed")) fprintf(stderr,"prebake rejection: %s\n",error);
    assert(!program && strstr(error,"prebake.strudel") && strstr(error,"fixture setup failed"));
    assert(!unlink(path));
    snprintf(linked,sizeof(linked),"%s/elsewhere.strudel",root);
    write_module(linked,"window.preLead=()=>note(99)");
    assert(!symlink(linked,path));
    program=javascript_compile("note(60)",120,&metadata,error,sizeof(error));
    assert(!program && !metadata); assert(!unlink(path)); assert(!unlink(linked));
    assert(!mkfifo(path,0600));
    program=javascript_compile("note(60)",120,&metadata,error,sizeof(error));
    assert(!program); assert(!unlink(path));
    FILE *large=fopen(path,"w");
    assert(large && !fseek(large,256*1024,SEEK_SET) && fputc(' ',large)!=EOF && !fclose(large));
    program=javascript_compile("note(60)",120,&metadata,error,sizeof(error));
    assert(!program); assert(!unlink(path));
    snprintf(path,sizeof(path),"%s/tompouce",root); assert(!rmdir(path));
    assert(!rmdir(root));
    if (saved) { assert(!setenv("XDG_CONFIG_HOME",saved,1)); free(saved); }
    else assert(!unsetenv("XDG_CONFIG_HOME"));
}

static void test_local_modules(void) {
    char root[] = "/tmp/tompouce-modules-XXXXXX", path[512];
    assert(mkdtemp(root));
    const char *previous = getenv("XDG_CONFIG_HOME");
    char *saved = previous ? strdup(previous) : NULL;
    assert(!previous || saved);
    assert(setenv("XDG_CONFIG_HOME",root,1) == 0);
    snprintf(path,sizeof(path),"%s/tompouce",root); assert(mkdir(path,0700) == 0);
    snprintf(path,sizeof(path),"%s/tompouce/modules",root); assert(mkdir(path,0700) == 0);
    snprintf(path,sizeof(path),"%s/tompouce/modules/sub",root); assert(mkdir(path,0700) == 0);
    snprintf(path,sizeof(path),"%s/tompouce/modules/sub/notes.mjs",root);
    write_module(path,"export default [60,64];");
    snprintf(path,sizeof(path),"%s/tompouce/modules/sub/melody.mjs",root);
    write_module(path,"import notes from './notes.mjs'; import {note,stack} from '@strudel/core'; export const melody=()=>stack(...notes.map(n=>note(n)));");
    JsProgram *program;
    cJSON *haps = query("import {melody} from './sub/melody.mjs'; melody()",&program);
    assert(cJSON_GetArraySize(haps) == 2);
    cJSON_Delete(haps);
    snprintf(path,sizeof(path),"%s/tompouce/modules/sub/notes.mjs",root);
    write_module(path,"export default [65];");
    haps = javascript_query(program,0,1);
    assert(cJSON_GetArraySize(haps) == 2); /* compiled programs keep their modules */
    cJSON_Delete(haps); javascript_free(program);
    haps = query("import {melody} from './sub/melody.mjs'; melody()",&program);
    assert(cJSON_GetArraySize(haps) == 1); /* reevaluation reloads local files */
    cJSON_Delete(haps); javascript_free(program);
    write_module(path,"export default [60,64];");
    haps = query("const {melody}=await import('local:sub/melody.mjs'); melody()",&program);
    assert(cJSON_GetArraySize(haps) == 2);
    cJSON_Delete(haps); javascript_free(program);
    snprintf(path,sizeof(path),"%s/tompouce/modules/link.mjs",root);
    assert(symlink("sub/notes.mjs",path) == 0);
    snprintf(path,sizeof(path),"%s/tompouce/modules/linked",root);
    assert(symlink("sub",path) == 0);
    snprintf(path,sizeof(path),"%s/tompouce/modules/large.mjs",root);
    FILE *large = fopen(path,"w");
    assert(large && fseek(large,256*1024,SEEK_SET) == 0 && fputc(' ',large) != EOF && fclose(large) == 0);
    const char *rejected[] = {"import './link.mjs'; silence", "import '../outside.mjs'; silence",
        "import './linked/notes.mjs'; silence", "import './large.mjs'; silence",
        "import 'https://example.com/a.mjs'; silence", "import '/etc/passwd'; silence", "import 'npm:example'; silence"};
    char error[512]; cJSON *metadata = NULL;
    for (size_t i = 0; i < sizeof(rejected)/sizeof(rejected[0]); i++) {
        program = javascript_compile(rejected[i],120,&metadata,error,sizeof(error));
        assert(!program && !metadata);
    }
    assert(unlink(path) == 0);
    snprintf(path,sizeof(path),"%s/tompouce/modules/link.mjs",root); assert(unlink(path) == 0);
    snprintf(path,sizeof(path),"%s/tompouce/modules/linked",root); assert(unlink(path) == 0);
    snprintf(path,sizeof(path),"%s/tompouce/modules/sub/melody.mjs",root); assert(unlink(path) == 0);
    snprintf(path,sizeof(path),"%s/tompouce/modules/sub/notes.mjs",root); assert(unlink(path) == 0);
    snprintf(path,sizeof(path),"%s/tompouce/modules/sub",root); assert(rmdir(path) == 0);
    snprintf(path,sizeof(path),"%s/tompouce/modules",root); assert(rmdir(path) == 0);
    snprintf(path,sizeof(path),"%s/tompouce",root); assert(rmdir(path) == 0);
    assert(rmdir(root) == 0);
    if (saved) { assert(setenv("XDG_CONFIG_HOME",saved,1) == 0); free(saved); }
    else assert(unsetenv("XDG_CONFIG_HOME") == 0);
}

static cJSON *query(const char *code, JsProgram **program) {
    char error[1024];
    cJSON *metadata = NULL;
    slider_begin();
    *program = javascript_compile(code, 120, &metadata, error, sizeof(error));
    if (!*program) fprintf(stderr, "%s\n%s\n", code, error);
    assert(*program);
    cJSON_Delete(metadata);
    cJSON *haps = javascript_query(*program, 0, 1);
    assert(cJSON_IsArray(haps));
    return haps;
}

static void test_control_aliases(void) {
    JsProgram *program;
    cJSON *noisy=query("note(69).csound('ClockQA').gain(.5).slow(4).late(1)",&program);
    cJSON_Delete(noisy);
    const double clock[][2]={{.60000000000000009,.62500000000000011},
        {.75000000000000022,.77500000000000024},{.9750000000000002,1.0000000000000002}};
    for (int i=0;i<3;i++) {
        noisy=javascript_query_audio(program,clock[i][0],clock[i][1],.5);
        assert(cJSON_IsArray(noisy) && cJSON_GetArraySize(noisy)==0);
        cJSON_Delete(noisy);
    }
    noisy=javascript_query_audio(program,1.0000000000000002,1.0250000000000001,.5);
    assert(cJSON_IsArray(noisy) && cJSON_GetArraySize(noisy)==1);
    assert(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(noisy,0),"begin")->valuedouble==1);
    cJSON_Delete(noisy); javascript_free(program);
    cJSON *haps=query("const control=slider; note(60).gain(control(.7))",&program);
    javascript_activate_sliders(program);
    assert(slider_count()==0 && cJSON_GetArraySize(haps)==1);
    const cJSON *value=cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(haps,0),"value");
    assert(cJSON_GetObjectItemCaseSensitive(value,"gain")->valuedouble==.7);
    cJSON_Delete(haps); javascript_free(program);
    haps=query("note(60).gain(globalThis['slider'](.4))",&program);
    javascript_activate_sliders(program); assert(slider_count()==0);
    cJSON_Delete(haps); javascript_free(program);
    haps=query("note(60).cpm(slider(60,30,120))",&program);
    javascript_activate_sliders(program); assert(slider_count()==1 && cJSON_GetArraySize(haps)==2);
    cJSON_Delete(haps);
    haps=javascript_query_cps(program,0,1,1);
    assert(cJSON_GetArraySize(haps)==1);
    assert(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(haps,0),"end")->valuedouble==1);
    cJSON_Delete(haps); assert(slider_set(0,30));
    haps=javascript_query_cps(program,0,1,1);
    assert(cJSON_GetArraySize(haps)==1);
    assert(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(haps,0),"end")->valuedouble==2);
    cJSON_Delete(haps);
    haps=javascript_query_cps(program,0,1,.25);
    assert(cJSON_GetArraySize(haps)==2);
    cJSON_Delete(haps); javascript_free(program);
    /* A numeric cpm transform is computed at evaluation, as in web repl.mjs. */
    haps=query("note(60).cpm(60)",&program); assert(cJSON_GetArraySize(haps)==2); cJSON_Delete(haps);
    haps=javascript_query_cps(program,0,1,1); assert(cJSON_GetArraySize(haps)==2);
    cJSON_Delete(haps); javascript_free(program);
    cJSON *metadata=NULL; char error[1024];
    program=javascript_compile("setCpm(slider(30,20,60)); note(60)",120,&metadata,error,sizeof(error));
    if (!program) fprintf(stderr,"setcpm Pattern: %s\n",error);
    assert(program && cJSON_GetObjectItemCaseSensitive(metadata,"bpm")->valuedouble==120);
    javascript_activate_sliders(program); assert(slider_count()==1 && slider_set(0,60));
    haps=javascript_query(program,0,1); assert(cJSON_GetArraySize(haps)==1);
    cJSON_Delete(haps); cJSON_Delete(metadata); javascript_free(program);
}

static void test_tutorial(void) {
    FILE *file=fopen("strudeltutor.json","rb"); assert(file);
    assert(!fseek(file,0,SEEK_END)); long length=ftell(file); assert(length>0);
    rewind(file); char *text=malloc((size_t)length+1); assert(text);
    assert(fread(text,1,(size_t)length,file)==(size_t)length); fclose(file); text[length]=0;
    cJSON *song=cJSON_Parse(text); free(text); assert(song);
    cJSON *tracks=cJSON_GetObjectItemCaseSensitive(song,"tracks");
    assert(cJSON_GetArraySize(tracks)==22);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(song,"code")),
                   cJSON_GetStringValue(cJSON_GetArrayItem(tracks,0))));
    for (int lesson=0;lesson<22;lesson++) {
        const char *source=cJSON_GetStringValue(cJSON_GetArrayItem(tracks,lesson)); assert(source);
        char answer[8192]={0}; size_t used=0;
        for (const char *line=source;line && *line;) {
            const char *end=strchr(line,'\n'); size_t n=end?(size_t)(end-line):strlen(line);
            if (n>=5 && !strncmp(line,"// | ",5)) {
                assert(used+n-5+2<sizeof(answer));
                memcpy(answer+used,line+5,n-5); used+=n-5; answer[used++]='\n'; answer[used]=0;
            }
            line=end?end+1:NULL;
        }
        assert(used);
        for (int variant=0;variant<2;variant++) {
            JsProgram *program; cJSON *initial=query(variant?answer:source,&program);
            cJSON_Delete(initial);
            for (int cycle=0;cycle<5;cycle++) {
                cJSON *haps=javascript_query_cps(program,cycle,cycle+1,.45);
                assert(cJSON_IsArray(haps) && cJSON_GetArraySize(haps)>0);
                int transposed=0, first=0;
                for (int i=0;i<cJSON_GetArraySize(haps);i++) {
                    cJSON *hap=cJSON_GetArrayItem(haps,i);
                    double begin=cJSON_GetObjectItemCaseSensitive(hap,"begin")->valuedouble;
                    assert(begin>=cycle && begin<cycle+1);
                    if (lesson==0 && !variant) {
                        cJSON *value=cJSON_GetObjectItemCaseSensitive(hap,"value");
                        assert(cJSON_GetObjectItemCaseSensitive(value,"gain")->valuedouble==0);
                        continue; /* Silent introductory events are deliberately not scheduled. */
                    }
                    AudioEvent event; assert(js_audio_event(hap,0,1/.45,&event));
                    assert(isfinite(event.freq) && isfinite(event.gain));
                    if (lesson==10 && variant) {
                        const int roots[]={48,53,55,48}, intervals[]={0,4,7};
                        assert(i<3);
                        int midi=roots[cycle%4]+intervals[i];
                        assert(fabs(event.freq-440*pow(2,(midi-69)/12.0))<.01);
                    }
                    if (lesson==11) {
                        int step=(int)lround((begin-cycle)*4);
                        cJSON *value=cJSON_GetObjectItemCaseSensitive(hap,"value");
                        assert(step>=0 && step<4);
                        assert(fabs(cJSON_GetObjectItemCaseSensitive(value,"gain")->valuedouble-
                                    (step%2?.15:.6))<1e-9);
                        if (variant) assert(cJSON_GetObjectItemCaseSensitive(value,"pan")->valuedouble==step%2);
                    }
                    if (begin==cycle) {
                        first=1;
                        if ((lesson==10 || lesson==17) && variant) {
                            /* Raw degree arithmetic follows a four-cycle progression. */
                            const int roots[]={48,53,55,48};
                            int midi=lesson==10?roots[cycle%4]:67;
                            if (fabs(event.freq-440*pow(2,(midi-69)/12.0))<.01) transposed=1;
                        }
                        if (lesson==16 && !variant) {
                            int midi=cycle%4==0?67:60;
                            assert(fabs(event.freq-440*pow(2,(midi-69)/12.0))<.01);
                        }
                    }
                }
                if (variant && (lesson==10 || (lesson==17 && cycle%2==0))) assert(transposed);
                if (lesson==10 && variant) assert(cJSON_GetArraySize(haps)==3);
                if (lesson==11) assert(cJSON_GetArraySize(haps)==4);
                if (lesson==16) {
                    assert(first);
                    assert(cJSON_GetArraySize(haps)==(variant && cycle%2==0?8:4));
                }
                cJSON_Delete(haps);
            }
            for (int i=0;i<console_count_get();i++) {
                const char *message=console_line(i);
                assert(!strstr(message,"[warn]") && !strstr(message,"[error]"));
            }
            javascript_free(program);
        }
        if (lesson==10) {
            const char *bonus="note(\"c3 e3 g3\".add(\"<0 5 7 0>\")).s('triangle').gain(0.5)";
            assert(strstr(source,bonus));
            JsProgram *program; cJSON *initial=query(bonus,&program); cJSON_Delete(initial);
            for (int cycle=0;cycle<4;cycle++) {
                cJSON *haps=javascript_query_cps(program,cycle,cycle+1,.45);
                assert(cJSON_GetArraySize(haps)==3);
                const int offsets[]={0,5,7,0}, notes[]={48,52,55};
                for (int i=0;i<3;i++) {
                    cJSON *value=cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(haps,i),"value");
                    assert(cJSON_GetObjectItemCaseSensitive(value,"note")->valueint==notes[i]+offsets[cycle]);
                }
                cJSON_Delete(haps);
            }
            javascript_free(program);
        }
    }
    cJSON_Delete(song);
    puts("All 22 tutorial starters/answers query five cycles without warnings; control timing, periodic transforms and transposition verified");
}

int main(int argc, char **argv) {
    if ((argc==4 || argc==5) && !strcmp(argv[1],"--prebake-reference")) {
        int cycles=1;
        if (argc==5) {
            char *end=NULL; long count=strtol(argv[4],&end,10);
            if (!end || *end || count<1 || count>16) return 1;
            cycles=(int)count;
        }
        char root[]="/tmp/tompouce-prebake-reference-XXXXXX",directory[512],path[512],error[1024];
        assert(mkdtemp(root) && !setenv("XDG_CONFIG_HOME",root,1));
        snprintf(directory,sizeof(directory),"%s/tompouce",root); assert(!mkdir(directory,0700));
        snprintf(path,sizeof(path),"%s/tompouce/prebake.strudel",root); write_module(path,argv[2]);
        cJSON *metadata=NULL; JsProgram *program=javascript_compile(argv[3],120,&metadata,error,sizeof(error));
        cJSON *haps=program?cJSON_CreateArray():NULL;
        for (int cycle=0;haps && cycle<cycles;cycle++) {
            cJSON *part=javascript_query(program,cycle,cycle+1);
            if (!part) { cJSON_Delete(haps); haps=NULL; snprintf(error,sizeof(error),"prebake reference query failed at cycle %d",cycle); break; }
            while (part->child) cJSON_AddItemToArray(haps,cJSON_DetachItemFromArray(part,0));
            cJSON_Delete(part);
        }
        char *text=haps?cJSON_PrintUnformatted(haps):NULL;
        int ok=text!=NULL;
        if (text) { puts(text);free(text); } else fprintf(stderr,"%s\n",error);
        cJSON_Delete(haps);cJSON_Delete(metadata);javascript_free(program);
        assert(!unlink(path) && !rmdir(directory) && !rmdir(root)); return ok?0:1;
    }
    if (argc>1 && !strcmp(argv[1],"--prebake")) {
        test_prebake(); puts("prebake setup, refresh, rejection and confinement ok"); return 0;
    }
    /* Coastline's final .size(4) is a room duration, not an unrelated size. */
    JsProgram *room_alias_program;
    cJSON *room_alias_haps=query("note(60).s('sine').room(.5).size(4)",&room_alias_program);
    cJSON *room_alias_value=cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(room_alias_haps,0),"value");
    cJSON *room_alias_size=cJSON_GetObjectItemCaseSensitive(room_alias_value,"roomsize");
    assert(cJSON_IsNumber(room_alias_size) && room_alias_size->valuedouble==4);
    cJSON_Delete(room_alias_haps); javascript_free(room_alias_program);
    test_control_aliases();
    test_tutorial();
    test_live_visuals(); test_scope_defaults();
    test_slider_highlights();
    test_visual_highlight_mapping();
    test_local_modules();
    test_prebake();
    JsProgram *program;
    cJSON *midi_haps = query("import {csoundm} from '@strudel/csound'; note(60.5).csoundm(1.25)",&program);
    const cJSON *midi_value = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(midi_haps,0),"value");
    assert(cJSON_GetObjectItemCaseSensitive(midi_value,"csound")->valuedouble == 1.25);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(midi_value,"_csoundMidi")));
    assert(cJSON_GetObjectItemCaseSensitive(midi_value,"note")->valuedouble == 60.5);
    cJSON_Delete(midi_haps); javascript_free(program);
    cJSON *piano_haps = query("note(60).clip(.5).piano()",&program);
    const cJSON *piano_value = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(piano_haps,0),"value");
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(piano_value,"s")->valuestring,"piano"));
    assert(cJSON_GetObjectItemCaseSensitive(piano_value,"clip")->valuedouble == .5);
    assert(cJSON_GetObjectItemCaseSensitive(piano_value,"release")->valuedouble == .1);
    assert(fabs(cJSON_GetObjectItemCaseSensitive(piano_value,"pan")->valuedouble-(60.0/108*.5+.25)) < 1e-9);
    cJSON_Delete(piano_haps); javascript_free(program);
    /* "Bass fuge", Felix Roos, CC BY-NC-SA 4.0:
       https://creativecommons.org/licenses/by-nc-sa/4.0/
       Exercise the layered degree/sample-index distinction and echoes. */
    cJSON *bass_haps = query(
        "setcps(1); \"<8(3,8) <7 7*2> [4 5@3] 8>\".sub(1)"
        ".layer(x=>x,x=>x.add(7).off(1/8,x=>x.add(\"2,4\").off(1/8,x=>x.add(5).echo(4,.125,.5))).slow(2))"
        ".n().scale('A1 minor').s('flbass').n(0).mul(gain(.3))"
        ".cutoff(sine.slow(7).range(200,4000)).resonance(10).clip(1)"
        ".stack(s(\"bd:1*2,~ sd:0,[~ hh:0]*2\")).pianoroll({vertical:1})", &program);
    assert(cJSON_GetArraySize(bass_haps) == 16);
    const cJSON *bass_value = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(bass_haps,0),"value");
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(bass_value,"note")->valuestring,"A2"));
    assert(cJSON_GetObjectItemCaseSensitive(bass_value,"n")->valueint == 0);
    assert(cJSON_GetObjectItemCaseSensitive(bass_value,"gain")->valuedouble == .3);
    cJSON_Delete(bass_haps); javascript_free(program);
    cJSON *haps = query("s(\"misc:2\").delay(.5).delaytime(1/3).gain(.4)", &program);
    {
        const cJSON *misc = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(haps, 0), "value");
        const cJSON *sound = cJSON_GetObjectItemCaseSensitive(misc, "s");
        const cJSON *index = cJSON_GetObjectItemCaseSensitive(misc, "n");
        const cJSON *send = cJSON_GetObjectItemCaseSensitive(misc, "delay");
        const cJSON *time = cJSON_GetObjectItemCaseSensitive(misc, "delaytime");
        assert(cJSON_IsString(sound) && !strcmp(sound->valuestring, "misc"));
        assert(cJSON_IsNumber(index) && index->valuedouble == 2);
        assert(cJSON_IsNumber(send) && fabs(send->valuedouble - 0.5) < 1e-9);
        assert(cJSON_IsNumber(time) && fabs(time->valuedouble - (1.0 / 3.0)) < 1e-6);
    }
    cJSON_Delete(haps); javascript_free(program);
    haps = query("note(\"c4 e4\").hpf(200).ply(2)", &program);
    assert(cJSON_GetArraySize(haps) == 4);
    const cJSON *first = cJSON_GetArrayItem(haps, 0);
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(first, "value");
    assert(cJSON_GetObjectItemCaseSensitive(value, "hcutoff")->valueint == 200);
    javascript_cache_spans(program, haps, 0, 2);
    int off[8], len[8];
    assert(javascript_play_spans(program, .1, off, len, 8) == 1);
    assert(off[0] == 6 && len[0] == 2);
    cJSON_Delete(haps); javascript_free(program);

    const char *scoped_code = "// é\nstack(note('c4')._scope().gain(.2),note('g4')._scope().gain(.8))";
    haps = query(scoped_code,&program);
    assert(cJSON_GetArraySize(haps) == 2);
    const char *scope_a = strstr(scoped_code,"_scope"), *scope_b = strstr(scope_a+1,"_scope");
    unsigned mask_a = javascript_scope_mask(program,(int)(scope_a-scoped_code),1);
    unsigned mask_b = javascript_scope_mask(program,(int)(scope_b-scoped_code),1);
    assert(mask_a && mask_b && mask_a != mask_b);
    for (int i = 0; i < 2; i++) {
        const cJSON *hap = cJSON_GetArrayItem(haps,i);
        AudioEvent event;
        assert(js_audio_event(hap,0,2,&event));
        assert(event.scope_mask == (i == 0 ? mask_a : mask_b));
        assert(!cJSON_HasObjectItem(cJSON_GetObjectItemCaseSensitive(hap,"value"),"scopeMask"));
    }
    cJSON_Delete(haps); javascript_free(program);
    haps = query("note('c4')._scope()._pianoroll()",&program);
    assert(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(haps,0),"scopeMask")->valueint == 3);
    cJSON_Delete(haps); javascript_free(program);

    const char *views="note('60').pianoroll({cycles:2}).add(note(12)).punchcard().spiral().pitchwheel().spectrum().wordfall().tscope().color('red')";
    haps=query(views,&program);
    javascript_update_visuals(program,10,0,2);
    VisualNote drawn[64]; double visual_origin,visual_seconds;
    for (int kind=1;kind<=VISUAL_KINDS;kind++) {
        const char *method=kind==VISUAL_SCOPE?strstr(views,"tscope"):strstr(views,visual_name(kind));
        unsigned mask=javascript_scope_mask(program,(int)(method-views),kind);
        assert(mask);
        int count=javascript_visual_copy(program,mask,drawn,64,&visual_origin,&visual_seconds);
        if (kind==VISUAL_SCOPE) { assert(count==0); continue; }
        assert(count>0 && visual_seconds==2);
        for (int i=0;i<count;i++) {
            assert(drawn[i].pitch==72);
            assert(drawn[i].color==0xff0000ffu);
        }
    }
    cJSON *again=javascript_query(program,0,1);
    char *before=cJSON_PrintUnformatted(haps), *after=cJSON_PrintUnformatted(again);
    assert(before && after && !strcmp(before,after));
    free(before); free(after); cJSON_Delete(again); cJSON_Delete(haps); javascript_free(program);

    haps=query("all(punchcard); $: note('60').gain(0); $: note('67').gain(0)",&program);
    javascript_update_visuals(program,1,0,2);
    assert(javascript_visual_copy(program,1,drawn,64,NULL,NULL)>1);
    cJSON_Delete(haps); javascript_free(program);
    haps=query("note(\"60 64\").color(\"red blue\").markcss('text-decoration:underline; font-weight:bold')",&program);
    javascript_cache_spans(program,haps,0,2);
    uint32_t color[8]; unsigned styles[8];
    assert(javascript_play_styles(program,.1,off,len,color,styles,8)>0);
    assert(color[0]==0xff0000ffu && (styles[0]&(VISUAL_COLOR|VISUAL_UNDERLINE|VISUAL_BOLD))==
           (VISUAL_COLOR|VISUAL_UNDERLINE|VISUAL_BOLD));
    cJSON_Delete(haps); javascript_free(program);

    haps=query("stack(note(60).slow(8),note(60).slow(8))._pianoroll()",&program);
    javascript_update_visuals(program,10,0,2);
    assert(javascript_visual_copy(program,1,drawn,64,NULL,NULL)==2);
    /* A held hap crosses many cache tiles. Deduplicate tiles, retaining two
       genuinely identical simultaneous notes within the original pattern. */
    cJSON_Delete(haps); javascript_free(program);

    haps = query("$: s(\"bd sd\"); $: note(\"c3 e3\")", &program);
    assert(cJSON_GetArraySize(haps) == 4);
    cJSON_Delete(haps); javascript_free(program);

    haps = query("import { note as tone } from '@strudel/core'; const twice=x=>x.fast(2); twice(tone('c4'))", &program);
    assert(cJSON_GetArraySize(haps) == 2);
    cJSON_Delete(haps); javascript_free(program);

    haps = query("const {note: tone}=await import('@strudel/core'); tone('c4').ply(3)", &program);
    assert(cJSON_GetArraySize(haps) == 3);
    cJSON_Delete(haps); javascript_free(program);

    haps = query("const degrees = [0,2,4,6]; stack(...degrees.map(x=>n(x).scale('C:minor')))", &program);
    assert(cJSON_GetArraySize(haps) == 4);
    cJSON_Delete(haps); javascript_free(program);

    haps = query("note(\"c4 e4 g4 b4\").every(2, x=>x.rev()).zoom(0,.5).ply(2)", &program);
    assert(cJSON_GetArraySize(haps) == 4);
    cJSON_Delete(haps); javascript_free(program);

    haps = query("_: s(\"bd*4\"); melody: note(\"c4 e4\"); each(x=>x.fast(2)); all(x=>x.rev())", &program);
    assert(cJSON_GetArraySize(haps) == 4);
    cJSON_Delete(haps); javascript_free(program);

    haps = query("$: note(60); S$: note(64); S$: note(67); Sbad_: note(72); each(x=>x.fast(2))", &program);
    assert(cJSON_GetArraySize(haps) == 4);
    for (int i=0; i<4; i++) {
        value = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(haps,i),"value");
        int note = cJSON_GetObjectItemCaseSensitive(value,"note")->valueint;
        assert(note == 64 || note == 67);
    }
    cJSON_Delete(haps); javascript_free(program);

    haps = query("each(x=>x.fast(3)); note(60)", &program);
    assert(cJSON_GetArraySize(haps) == 3);
    cJSON_Delete(haps); javascript_free(program);

    haps = query("lead: new Pattern(state=>note(60).set({observedId:state.controls.id}).query(state))", &program);
    assert(cJSON_GetArraySize(haps) == 1);
    value = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(haps,0),"value");
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(value,"observedId")->valuestring,"lead"));
    cJSON_Delete(haps); javascript_free(program);

    haps = query("const p = note('c4'); p.cpm(60)", &program);
    assert(cJSON_GetArraySize(haps) == 2);
    cJSON_Delete(haps); javascript_free(program);

    haps = query("note('c4').partials([1,0,.5]).phases([0,0,.25]).fm(2).fmh(1.5).phaser(2)", &program);
    value = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(haps,0),"value");
    assert(cJSON_GetObjectItemCaseSensitive(value,"fmi")->valueint == 2);
    assert(cJSON_GetObjectItemCaseSensitive(value,"phaserrate")->valueint == 2);
    assert(cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(value,"partials")) == 3);
    cJSON_Delete(haps); javascript_free(program);

    haps = query("// é\nnote(\"c4\")", &program);
    javascript_cache_spans(program,haps,0,2);
    assert(javascript_play_spans(program,.1,off,len,8) == 1);
    assert(off[0] == 12 && len[0] == 2);
    cJSON_Delete(haps); javascript_free(program);

    haps = query("stack(cat(note(\"c4 e4\"),note('g4')),seq(s('bd'),s('sd')))", &program);
    assert(cJSON_GetArraySize(haps) == 4);
    cJSON_Delete(haps);
    haps = javascript_query(program,1,2);
    assert(cJSON_GetArraySize(haps) == 3);
    cJSON_Delete(haps); javascript_free(program);

    haps = query("note(\"c4\").off(1/8,x=>x.add(note(7))).jux(rev)", &program);
    assert(cJSON_GetArraySize(haps) == 4);
    cJSON_Delete(haps); javascript_free(program);

    haps = query("n(sine.range(0,7).segment(4)).scale('C:minor')", &program);
    assert(cJSON_GetArraySize(haps) == 4);
    cJSON_Delete(haps); javascript_free(program);

    haps = query("note('c4').when(\"<1 0>\",x=>x.fast(2))", &program);
    assert(cJSON_GetArraySize(haps) == 2);
    cJSON_Delete(haps); javascript_free(program);

    haps = query("function* steps(){yield 60;yield 64} class Melody {make(){return note(seq(...steps()))}} new Melody().make()", &program);
    assert(cJSON_GetArraySize(haps) == 2);
    cJSON_Delete(haps); javascript_free(program);

    haps = query("soundAlias('bd','kick');s('kick')", &program);
    value = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(haps,0),"value");
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(value,"s")->valuestring,"bd"));
    cJSON_Delete(haps); javascript_free(program);

    char error[512]; cJSON *meta = NULL;
    const char *bad_csound[]={
        "__hostCsound('other','text')",
        "__hostCsound('code\\0other','text')",
        "__hostCsound('code',null)",
        "__hostCsound('code','abc\\0def')",
        "__hostCsound('code','x'.repeat(65537))",
        "__hostCsound('url','x'.repeat(481))",
        "for(let i=0;i<17;i++) __hostCsound('code','')"
    };
    for (unsigned i=0;i<sizeof(bad_csound)/sizeof(*bad_csound);i++) {
        program=javascript_compile(bad_csound[i],120,&meta,error,sizeof(error));
        assert(!program && !meta);
    }
    program = javascript_compile("while(true){}; note('c4')", 120, &meta, error, sizeof(error));
    assert(!program && !meta);
    program = javascript_compile("import { missing } from '@strudel/core'; note('c4')",120,&meta,error,sizeof(error));
    assert(!program && strstr(error,"does not export"));
    {
        size_t n = 70000;
        char *big = malloc(n + 1);
        assert(big);
        memset(big, ' ', n);
        memcpy(big + n - 11, "note('c4')\n", 11);
        big[n] = 0;
        assert(n > 65535);
        program = javascript_compile(big, 120, &meta, error, sizeof(error));
        if (!program) fprintf(stderr, "%s\n", error);
        assert(program);
        cJSON_Delete(meta);
        haps = javascript_query(program, 0, 1);
        assert(cJSON_GetArraySize(haps) == 1);
        cJSON_Delete(haps); javascript_free(program);
        free(big);
    }
    program = javascript_compile("new Pattern(()=>{while(true){}})",120,&meta,error,sizeof(error));
    assert(program);
    cJSON_Delete(meta);
    assert(!javascript_query(program,0,1));
    javascript_free(program);
    int line = 0;
    assert(javascript_check("const p = x => x.fast(2);\np(note('c4'))",error,sizeof(error),&line));
    assert(!javascript_check("const p = ;",error,sizeof(error),&line));
    assert(!javascript_check("const p = 1;\nconst q = ;",error,sizeof(error),&line) && line == 2);
    int logged = console_count_get();
    haps = query("console.log('%c[voicing]: unknown chord \"Dx\"', 'background-color: black;color:white;border-radius:15px', 'tail'); note(60)", &program);
    assert(cJSON_GetArraySize(haps) == 1);
    int found_style = 0;
    for (int i = logged; i < console_count_get(); i++) {
        const char *message = console_line(i);
        if (!strstr(message, "[voicing]: unknown chord \"Dx\" tail")) continue;
        found_style = 1;
        assert(!strstr(message, "%c") && !strstr(message, "background-color"));
    }
    assert(found_style);
    cJSON_Delete(haps); javascript_free(program);
    logged = console_count_get();
    haps = query("chord('Dx').anchor('g5').voicing()", &program);
    int found_chord = 0;
    for (int i = logged; i < console_count_get(); i++) {
        const char *message = console_line(i);
        if (!strstr(message, "[voicing]: unknown chord \"Dx\"")) continue;
        found_chord = 1;
        assert(!strstr(message, "%c") && !strstr(message, "background-color"));
    }
    assert(found_chord);
    cJSON_Delete(haps); javascript_free(program);
    haps = query(
        "const F=Fraction._original;"
        "if (String(F.prototype.add).indexOf('native code')<0) throw new Error('fraction method is not native');"
        "const add=F(1,6).add(F(1,3));"
        "if (add.show()!=='1/2') throw new Error('add '+add.show());"
        "if (F(1,2).sub(F(1,3)).show()!=='1/6') throw new Error('sub');"
        "if (F(2,3).mul(3,4).show()!=='1/2') throw new Error('mul');"
        "if (Number(F(-5,2).floor())!==-3 || Number(F(5,2).ceil())!==3) throw new Error('whole');"
        "if (F(7,3).mod(1).show()!=='1/3' || F(-5,3).mod().show()!=='-2/1') throw new Error('mod');"
        "if (F(1,6).gcd(F(1,4)).show()!=='1/12') throw new Error('gcd');"
        "if (F(5,8).lcm(3,7).show()!=='15/1') throw new Error('lcm');"
        "if (F(2,3).lcm(4,5).show()!=='4/1') throw new Error('lcm shared numerator gcd');"
        "for(const op of ['div','mod']){let threw=false;try{F(1)[op](0)}catch(e){threw=true}"
        "if(!threw)throw new Error(op+' zero must throw');}"
        "if(F(1,1099511627776).mul(F(1,1099511627776)).show()!=='1/1208925819614629174706176')throw new Error('multiply bigint fallback');"
        "if(F(BigInt('9223372036854775807')).add(BigInt(1)).show()!=='9223372036854775808/1')throw new Error('add bigint fallback');"
        "if (!F(1,2).equals(F(2,4)) || F(1,3).compare(F(1,2))!==-1) throw new Error('order');"
        "if (Number(F(1,2).add(0.5))!==1 || F(1,3).add('1/6').show()!=='1/2') throw new Error('fallback');"
        "if (F(-4).abs().show()!=='4/1') throw new Error('abs');"
        "note(60)", &program);
    assert(cJSON_GetArraySize(haps)==1);
    cJSON_Delete(haps); javascript_free(program);
    haps=query(
        "const check=(v,m)=>{if(!v)throw Error(m)};const F=Fraction._original;"
        "for(const x of [-7,-1,0,1,7]){const f=F(x,3),s=Math.floor(x/3);"
        "check(Number(f.sam())===s&&Number(f.nextSam())===s+1,'cycle bounds');"
        "check(f.cyclePos().equals(f.sub(s)),'cycle position');"
        "check(f.wholeCycle().equals(new TimeSpan(s,s+1)),'whole cycle');"
        "check(f.lt(F(9),0)&&f.lte(F(9),0)&&!f.gt(F(9),0),'ignored comparison arguments');}"
        "const f=F(1,3),g=F(1,3),clone=F(f);"
        "class CustomFraction extends F{};const custom=new CustomFraction(1,3);check(custom instanceof CustomFraction&&custom.equals(f),'subclass constructor');"
        "check(clone!==f&&clone.equals(f)&&clone instanceof F,'clone');clone.n=BigInt(2);check(f.n===BigInt(1),'clone independence');"
        "check(f.min(g)===g&&f.max(g)===g&&f.min(F(1))===f,'min max identity');"
        "const sect=(a,b)=>{const l=a.begin.max(b.begin),r=a.end.min(b.end);"
        "if(l.gt(r)||(l.equals(r)&&((l.equals(a.end)&&a.begin.lt(a.end))||(l.equals(b.end)&&b.begin.lt(b.end)))))return;"
        "return new TimeSpan(l,r)};"
        "for(let i=-4;i<=4;i++)for(let j=i;j<=4;j++){const a=new TimeSpan(F(i,3),F(j,3));"
        "check(a.duration.equals(a.end.sub(a.begin)),'duration');"
        "check(a.midpoint().equals(a.begin.add(a.end).div(2)),'midpoint');"
        "check(a.cycleArc().begin.equals(a.begin.sub(a.begin.floor())),'cycle arc');"
        "for(let k=-3;k<=3;k++){const b=new TimeSpan(F(k,2),F(k+1,2)),x=a.intersection(b),y=sect(a,b);"
        "check((x===undefined&&y===undefined)||(x&&y&&x.equals(y)),'intersection');}"
        "const pieces=a.spanCycles;check(pieces.length>0,'split');"
        "check(pieces[0].begin.equals(a.begin)&&pieces[pieces.length-1].end.equals(a.end),'split endpoints');"
        "for(let k=0;k<pieces.length;k++){check(pieces[k].duration.lte(1),'split length');"
        "if(k)check(pieces[k-1].end.equals(pieces[k].begin),'split continuity');}}"
        "check(new TimeSpan(2,1).spanCycles.length===0,'reversed');"
        "const wide=new TimeSpan(F(BigInt(2)**BigInt(80)),F(BigInt(2)**BigInt(80)+BigInt(1)));check(wide.duration.equals(1),'wide fallback');"
        "const span=new TimeSpan(0,1);let calls=0;const changed=span.withTime(t=>{if(!calls++)span.end=F(2);return t.add(1)});"
        "check(calls===2&&changed.begin.equals(1)&&changed.end.equals(3),'callback ordering');"
        "calls=0;const numeric=span.withTime(t=>{calls++;return Number(t)+1});"
        "check(calls===2&&numeric.equals(new TimeSpan(1,3)),'numeric callback fallback');"
        "let reads=0;const accessor=new TimeSpan(0,1);Object.defineProperty(accessor,'begin',{get(){reads++;return F(0)}});"
        "check(accessor.intersection(new TimeSpan(0,2)).equals(new TimeSpan(0,1))&&reads>0,'accessor fallback');"
        "let threw=false;try{span.withTime(()=>{throw Error('sentinel')})}catch(e){threw=e.message==='sentinel'}check(threw,'callback exception');"
        "const context={locations:[]},h=new Hap(span,span,3,context);h.stateful=true;"
        "const v=h.withValue(x=>{h.context=undefined;return x+1});"
        "check(v.value===4&&v.whole===span&&v.part===span&&v.stateful===false&&v.context!==context,'hap value');"
        "h.context=context;calls=0;const q=h.withSpan(s=>{calls++;return new TimeSpan(s.begin,s.end)});"
        "check(calls===2&&q.whole!==span&&q.part!==span&&q.value===3&&q.context===context&&!q.stateful,'hap span');"
        "const continuous=new Hap(undefined,span,1,context);calls=0;continuous.withSpan(s=>{calls++;return s});check(calls===1,'continuous span');"
        "note(60)",&program);
    assert(cJSON_GetArraySize(haps)==1);
    cJSON_Delete(haps); javascript_free(program);
    haps = query(
        "const expected=chord('C').voicing().queryArc(0,1).map(h=>h.value.note);"
        "if(expected.length!==5)throw new Error('default voicing');"
        "setDefaultVoicings('lefthand');resetVoicings();"
        "const actual=chord('C').voicing().queryArc(0,1).map(h=>h.value.note);"
        "if(JSON.stringify(actual)!==JSON.stringify(expected))throw new Error('voicing reset');"
        "chord('C').voicing()", &program);
    assert(cJSON_GetArraySize(haps)==5);
    cJSON_Delete(haps); javascript_free(program);
    haps = query(
        "const nativeTriangle=__hostTriangle;"
        "if(nativeTriangle(null,Fraction(1),Fraction(0),0,1,[])!==null)throw Error('invalid triangle span');"
        "if(nativeTriangle(new TimeSpan(0,1),Fraction(0),Fraction(0),0,1,[])!==null)throw Error('zero triangle rate');"
        "if(nativeTriangle(new TimeSpan(0,1),Fraction(1),Fraction(0),0,1,new Array(257))!==null)throw Error('triangle operation bound');"
        "for(const wave of [tri,itri,tri2,itri2])for(const rate of [1,3,0.5])for(const shift of [-0.25,0,0.3]){"
        "const base=wave.fast(rate).early(shift);for(const p of [base,base.range(-3,7)]){"
        "const native=p.queryArc(-0.7,0.8);__hostTriangle=()=>null;"
        "let reference;try{reference=p.queryArc(-0.7,0.8)}finally{__hostTriangle=nativeTriangle}"
        "if(native.length!==reference.length)throw Error('triangle count');"
        "for(let i=0;i<native.length;i++)if(!native[i].part.equals(reference[i].part)||!Object.is(native[i].value,reference[i].value))throw Error('triangle fallback difference');}}"
        "note(60)",&program);
    assert(cJSON_GetArraySize(haps)==1);
    cJSON_Delete(haps); javascript_free(program);
    haps = query(
        "function same(a,b,label){"
        "if(a.length!==b.length)throw new Error(label+' count '+a.length+'/'+b.length);"
        "for(let i=0;i<a.length;i++){"
        "if(a[i].whole!=null||b[i].whole!=null)throw new Error(label+' whole');"
        "if(!a[i].part.begin.equals(b[i].part.begin)||!a[i].part.end.equals(b[i].part.end))"
        "throw new Error(label+' span '+a[i].part.show()+' vs '+b[i].part.show());"
        "if(!Object.is(a[i].value,b[i].value))throw new Error(label+' value '+a[i].value+' vs '+b[i].value+' neg '+Object.is(a[i].value,-0)+'/'+Object.is(b[i].value,-0));"
        "}}"
        "const arc=(pat,b,e)=>pat.queryArc(b,e);"
        "same(arc(tri.fast(2),0.25,0.25),arc(tri.fast(2),0.25,0.25),'point triangle');"
        "same(arc(tri.fast(1),0,1),arc(tri,0,1),'tri');"
        "same(arc(tri.fast(1),-0.3,0.4),arc(tri,-0.3,0.4),'tri-neg');"
        "same(arc(cosine,0,1),arc(sine.early(Fraction(1).div(4)),0,1),'cosine');"
        "same(arc(saw2,0,1),arc(saw.toBipolar(),0,1),'saw2');"
        "same(arc(perlin.fast(1),0.2,1.2),arc(perlin,0.2,1.2),'perlin');"
        "same(arc(berlin.fast(1),-0.2,0.4),arc(berlin,-0.2,0.4),'berlin');"
        "same(arc(rand.fast(1),10.25,10.5),arc(rand,10.25,10.5),'rand');"
        "same(arc(sine2.fast(1),0.2,0.3),arc(sine2,0.2,0.3),'sine2');"
        "same(arc(square.fast(1),-1,-0.2),arc(square,-1,-0.2),'square');"
        "const quarter=perlin.range(1,2).slow(4).queryArc(2,2.1);"
        "const raw=perlin.queryArc(0.5,0.6)[0].value;"
        "if(quarter.length!==1||!Object.is(quarter[0].value,raw+1))throw new Error('perlin-range '+(quarter[0]&&quarter[0].value)+' vs '+(raw+1));"
        "const lpf=tri.range(0,5000).fast(10).queryArc(0,1/10);"
        "if(lpf.length!==2||!Object.is(lpf[0].value,0)||!Object.is(lpf[1].value,5000))throw new Error('lpf '+lpf.length+' '+(lpf[0]&&lpf[0].value)+','+(lpf[1]&&lpf[1].value));"
        "const neg=saw.fast(4).queryArc(-1,-0.5);"
        "if(neg.length!==1||!Object.is(neg[0].value,-0))throw new Error('neg0 len '+neg.length+' '+Object.is(neg[0]&&neg[0].value,-0));"
        "const ranged=sine.slow(7).range(200,4000).queryArc(3.5,4.5);"
        "const sineRef=sine.queryArc(0.5,0.51)[0].value*(4000-200)+200;"
        "if(ranged.length!==2||!Object.is(ranged[0].value,ranged[1].value)||!Object.is(ranged[0].value,sineRef))throw new Error('sine-range '+ranged.length+' '+(ranged[0]&&ranged[0].value)+' vs '+sineRef);"
        "const shifted=isaw.late(0.2).queryArc(0,1);"
        "if(!shifted.length||!Object.is(shifted[0].value,isaw.queryArc(-0.2,0.8)[0].value))throw new Error('late');"
        "useRNG('precise');"
        "same(arc(perlin.fast(1),0.2,0.7),arc(perlin,0.2,0.7),'precise');"
        "useRNG('legacy');"
        "same(arc(perlin.fast(1),0.2,0.7),arc(perlin,0.2,0.7),'legacy');"
        "same(arc(tri.early(0),0,1),arc(tri,0,1),'tri-early');"
        "same(arc(itri.fast(1),-0.3,0.4),arc(itri,-0.3,0.4),'itri');"
        "const hostSaw=__hostSignal([0],[-1],0)[0];"
        "const hostSquare=__hostSignal([3],[-1],0)[0];"
        "const hostSine=__hostSignal([2],[0.2],0)[0];"
        "if(!Object.is(hostSaw,-0)||!Object.is(hostSquare,-0))throw new Error('host zero');"
        "if(!Object.is(hostSine,Math.sin(Math.PI*2*0.2)))throw new Error('host sine '+hostSine);"
        "const xor=x=>{const a=(x<<13)^x;const b=(a>>17)^a;return (b<<5)^b};"
        "const frac=x=>x-Math.trunc(x);"
        "const randAt=(t,seed)=>Math.abs((xor(Math.trunc(frac((t+seed)/300)*536870912))%536870912)/536870912);"
        "const perlinAt=(t,seed)=>{const ta=Math.floor(t),x=t-ta,s=6*x**5-15*x**4+10*x**3;return randAt(ta,seed)+s*(randAt(ta+1,seed)-randAt(ta,seed))};"
        "const hostNoise=__hostSignal([4,5,6],[0.2,-0.2,10.25],1);"
        "if(!Object.is(hostNoise[0],perlinAt(0.2,1)))throw new Error('host perlin '+hostNoise[0]+' vs '+perlinAt(0.2,1));"
        "if(!Object.is(hostNoise[2],randAt(10.25,1)))throw new Error('host rand');"
        "const berlinAt=(t,seed)=>{const prev=Math.floor(t);return (randAt(prev,seed)+(t-prev)*randAt(prev+1,seed))/2};"
        "if(!Object.is(hostNoise[1],berlinAt(-0.2,1)))throw new Error('host berlin '+hostNoise[1]);"
        "note(60)", &program);
    assert(cJSON_GetArraySize(haps)==1);
    cJSON_Delete(haps); javascript_free(program);
    int hydra_log_start=console_count_get();
    haps = query(
        "await initHydra({feedStrudel:1,detectAudio:true});await initHydra();"
        "let called=0;const callback=()=>{called++;return 1};"
        "await s0.initCam();s1.init({src:'https://example.invalid/never-fetch'});"
        "src(s0).kaleid(H('<4 5 6>')).diff(osc(callback,.5,5)).modulateScale(noise(2)).out(o0);"
        "shape(3).add(solid(1)).blend(voronoi(2)).mult(gradient()).out(o1);"
        "H(callback);render(o0);hush();"
        "if(called)throw new Error('Hydra callback executed');"
        "if(a.fft[0]!==0)throw new Error('Hydra placeholder FFT');"
        "$: note(60).s('sine');$: note(64).s('triangle')", &program);
    assert(cJSON_GetArraySize(haps)==2);
    int hydra_warnings=0;
    for(int i=hydra_log_start;i<console_count_get();i++)
        if(strstr(console_line(i),"Hydra visuals are unavailable")) hydra_warnings++;
    assert(hydra_warnings==1);
    cJSON_Delete(haps); javascript_free(program);
    haps=query("await initHydra();osc(10).color(1,0,0).out()",&program);
    assert(cJSON_GetArraySize(haps)==0);
    cJSON_Delete(haps); javascript_free(program);
    haps=query("await initHydra();src(s0)",&program);
    assert(cJSON_GetArraySize(haps)==0);
    cJSON_Delete(haps); javascript_free(program);
    puts("JavaScript/Strudel runtime ok");
    return 0;
}
