#include "javascript.h"
#include "js_audio.h"
#include "slider.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static cJSON *query(const char *code, JsProgram **program);

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

int main(void) {
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
    assert(javascript_is_code("const p = note('c4'); p"));
    assert(!javascript_is_code("bpm 120\ndrum bd sd"));
    int line = 0;
    assert(javascript_check("const p = x => x.fast(2);\np(note('c4'))",error,sizeof(error),&line));
    assert(!javascript_check("const p = ;",error,sizeof(error),&line));
    assert(!javascript_check("const p = 1;\nconst q = ;",error,sizeof(error),&line) && line == 2);
    puts("JavaScript/Strudel runtime ok");
    return 0;
}
