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

int main(void) {
    test_control_aliases();
    test_tutorial();
    test_live_visuals(); test_scope_defaults();
    test_slider_highlights();
    test_visual_highlight_mapping();
    test_local_modules();
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
    puts("JavaScript/Strudel runtime ok");
    return 0;
}
