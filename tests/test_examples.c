#include "javascript.h"
#include "js_audio.h"
#include "common.h"
#include "slider.h"
#include "terminal_graphics.h"
#include <unistd.h>
#include <assert.h>

static cJSON *read_json(const char *path) {
    FILE *file = fopen(path,"rb");
    assert(file);
    assert(!fseek(file,0,SEEK_END));
    long size = ftell(file);
    assert(size > 0 && size < 4*1024*1024);
    rewind(file);
    char *text = malloc((size_t)size+1);
    assert(text);
    assert(fread(text,1,(size_t)size,file) == (size_t)size);
    assert(!fclose(file));
    text[size] = 0;
    cJSON *json = cJSON_Parse(text);
    free(text);
    assert(json);
    return json;
}

static const cJSON *get(const cJSON *object, const char *name) {
    return cJSON_GetObjectItemCaseSensitive(object,name);
}

/* Object order is immaterial, array/event order is significant. Allow
   small libm differences between Node/V8 and QuickJS/C. */
static int equivalent(const cJSON *actual, const cJSON *expected, int depth) {
    if (!actual || !expected) return actual == expected;
    if (depth > 64) return 0;
    if (cJSON_IsNumber(expected))
        return cJSON_IsNumber(actual) && isfinite(actual->valuedouble) &&
            fabs(actual->valuedouble-expected->valuedouble) <= 1e-7*fmax(1,fabs(expected->valuedouble));
    if (cJSON_IsString(expected))
        return cJSON_IsString(actual) && !strcmp(actual->valuestring,expected->valuestring);
    if (cJSON_IsNull(expected)) return cJSON_IsNull(actual);
    if (cJSON_IsBool(expected)) return cJSON_IsBool(actual) && cJSON_IsTrue(actual) == cJSON_IsTrue(expected);
    if (cJSON_IsArray(expected)) {
        if (!cJSON_IsArray(actual) || cJSON_GetArraySize(actual) != cJSON_GetArraySize(expected)) return 0;
        const cJSON *a = actual->child, *e = expected->child;
        for (; e; a=a->next,e=e->next) if (!equivalent(a,e,depth+1)) return 0;
        return 1;
    }
    if (cJSON_IsObject(expected)) {
        if (!cJSON_IsObject(actual) || cJSON_GetArraySize(actual) != cJSON_GetArraySize(expected)) return 0;
        const cJSON *field;
        cJSON_ArrayForEach(field,expected)
            if (!equivalent(get(actual,field->string),field,depth+1)) return 0;
        return 1;
    }
    return 0;
}

static void check_piano_roll_events(const char *code) {
    const char *call = strstr(code,".s('piano')"); assert(call);
    const char *tail = call+strlen(".s('piano')");
    size_t capacity = strlen(code)+32;
    char *scoped = malloc(capacity); assert(scoped);
    int prefix = (int)(tail-code);
    int written = snprintf(scoped,capacity,"%.*s._pianoroll()%s",prefix,code,tail);
    assert(written > 0 && (size_t)written < capacity);
    char error[512]; cJSON *metadata = NULL;
    JsProgram *plain = javascript_compile(code,120,&metadata,error,sizeof(error));
    assert(plain); cJSON_Delete(metadata); metadata = NULL;
    JsProgram *roll = javascript_compile(scoped,120,&metadata,error,sizeof(error));
    assert(roll); cJSON_Delete(metadata);
    unsigned mask = javascript_scope_mask(roll,prefix+1,2); assert(mask);
    VisualNote visual[VISUAL_CACHE_NOTES];
    double started=now_seconds();
    javascript_update_visuals(roll,10,0,2);
    int drawn=javascript_visual_copy(roll,mask,visual,VISUAL_CACHE_NOTES,NULL,NULL);
    printf("Arpoon visual query: %d notes in %.3fs\n",drawn,now_seconds()-started);
    assert(drawn>0);
    VisualNote scheduled[256];
    int piano = 0;
    for (int cycle = 0; cycle < 16; cycle++) {
        cJSON *baseline = javascript_query_cps(plain,cycle,cycle+1,.5);
        cJSON *display = javascript_query_cps(roll,cycle,cycle+1,.5);
        assert(cJSON_IsArray(baseline) && cJSON_IsArray(display));
        assert(cJSON_GetArraySize(baseline) == cJSON_GetArraySize(display));
        for (int i = 0; i < cJSON_GetArraySize(baseline); i++) {
            const cJSON *a = cJSON_GetArrayItem(baseline,i), *b = cJSON_GetArrayItem(display,i);
            assert(equivalent(get(a,"begin"),get(b,"begin"),0));
            assert(equivalent(get(a,"end"),get(b,"end"),0));
            assert(equivalent(get(a,"value"),get(b,"value"),0));
            const cJSON *sound = get(get(b,"value"),"s");
            int is_piano = cJSON_IsString(sound) && !strcmp(sound->valuestring,"piano");
            assert((unsigned)get(b,"scopeMask")->valuedouble == (is_piano ? mask : 0));
            if (is_piano) {
                AudioEvent event; assert(js_audio_event(b,0,2,&event));
                assert(event.scope_mask == mask); piano++;
                assert(piano<=(int)(sizeof(scheduled)/sizeof(*scheduled)));
                assert(visual_note(&scheduled[piano-1],b,0,2));
            }
        }
        cJSON_Delete(baseline); cJSON_Delete(display);
    }
    assert(piano > 0);
    for (int i=0;i<drawn;i++) {
        int matches=0;
        for (int j=0;j<piano;j++)
            if (fabs(visual[i].start-scheduled[j].start)<1e-9 &&
                fabs(visual[i].end-scheduled[j].end)<1e-9 &&
                fabs(visual[i].pitch-scheduled[j].pitch)<1e-9) matches++;
        assert(matches>0); /* Downstream slow(4), pitches and branch must match audio. */
    }
    printf("PASS Arpoon piano roll: all %d piano notes tagged, musical events unchanged\n",piano);
    javascript_free(plain); javascript_free(roll); free(scoped);
}

static int preview_bass_roll(void) {
    cJSON *song=read_json("bassfugue.json"), *metadata=NULL;
    const char *code=cJSON_GetStringValue(get(song,"code")); assert(code);
    char error[512];
    JsProgram *program=javascript_compile(code,240,&metadata,error,sizeof(error)); assert(program);
    const char *call=strstr(code,"pianoroll"); assert(call);
    unsigned mask=javascript_scope_mask(program,(int)(call-code),VISUAL_PIANOROLL); assert(mask);
    for (int i=0;i<24;i++) { javascript_update_visuals(program,10,0,1); usleep(130000); }
    static VisualNote notes[VISUAL_CACHE_NOTES]; static TerminalImage image;
    static TerminalFeedbackState state;
    VisualOptions options; assert(javascript_visual_options(program,mask,&options));
    int count=javascript_visual_copy(program,mask,notes,VISUAL_CACHE_NOTES,NULL,NULL); assert(count);
    image.width=768; image.height=128;
    terminal_graphics_feedback(&image,&state,VISUAL_PIANOROLL,&options,notes,count,NULL,0,10,0,1);
    printf("\033[2J\033[1;1HBass fugue: mixed pitch/drum roll (%d notes), no audio\n",count);
    assert(terminal_graphics_send(stdout,77,&image,2,1,110,9,0,128));
    fputs("\033[13;1HPress Enter to close.",stdout); fflush(stdout); (void)getchar();
    terminal_graphics_delete(stdout,77);
    javascript_free(program); cJSON_Delete(metadata); cJSON_Delete(song);
    return 0;
}

/* Exercise the published Markdown itself, rather than a duplicate fixture. */
static void check_gallery(void) {
    FILE *file=fopen("docs/examples.md","rb"); assert(file);
    assert(!fseek(file,0,SEEK_END)); long size=ftell(file); assert(size>0 && size<1024*1024);
    rewind(file); char *text=malloc((size_t)size+1); assert(text);
    assert(fread(text,1,(size_t)size,file)==(size_t)size); assert(!fclose(file)); text[size]=0;
    const char *cursor=text, *fence; int snippets=0;
    while ((fence=strstr(cursor,"```"))) {
        assert(!strncmp(fence,"```js\n",6));
        const char *start=fence+6, *end=strstr(start,"\n```"); assert(end);
        size_t length=(size_t)(end-start); char *code=malloc(length+1); assert(code);
        memcpy(code,start,length); code[length]=0;
        char error[1024]; cJSON *metadata=NULL;
        slider_begin(); JsProgram *program=javascript_compile(code,120,&metadata,error,sizeof(error));
        if (!program) fprintf(stderr,"Gallery example %d: %s\n%s\n",snippets+1,error,code);
        assert(program); free(code);
        const cJSON *tempo=get(metadata,"bpm");
        assert(cJSON_IsNumber(tempo) && tempo->valuedouble>0);
        double seconds=240/tempo->valuedouble; int events=0;
        for (int cycle=0;cycle<4;cycle++) {
            cJSON *haps=javascript_query_cps(program,cycle,cycle+1,1/seconds);
            assert(cJSON_IsArray(haps));
            const cJSON *hap;
            cJSON_ArrayForEach(hap,haps) {
                AudioEvent event; assert(js_audio_event(hap,0,seconds,&event));
                assert(isfinite(event.time) && isfinite(event.duration) && event.duration>0);
                assert(isfinite(event.freq) && isfinite(event.gain)); events++;
            }
            cJSON_Delete(haps);
        }
        assert(events>0);
        for (int i=0;i<console_count_get();i++) {
            const char *message=console_line(i);
            assert(!strstr(message,"[warn]") && !strstr(message,"[error]"));
        }
        cJSON_Delete(metadata); javascript_free(program);
        snippets++; cursor=end+4;
    }
    assert(snippets==18); free(text);
    puts("Gallery: all 18 Markdown examples query four cycles without warnings");
}

int main(int argc, char **argv) {
    if (argc>1 && !strcmp(argv[1],"--roll-preview")) return preview_bass_roll();
    check_gallery();
    cJSON *corpus = read_json("tests/fixtures/strudel-examples.json");
    cJSON *reference = read_json("tests/fixtures/strudel-example-events.json");
    const cJSON *examples = get(corpus,"examples"), *references = get(reference,"examples");
    assert(cJSON_GetArraySize(examples) == 31);
    assert(cJSON_GetArraySize(examples) == cJSON_GetArraySize(references));
    int supported = 0, events = 0;
    for (int i = 0; i < cJSON_GetArraySize(examples); i++) {
        const cJSON *example = cJSON_GetArrayItem(examples,i);
        const cJSON *expected = cJSON_GetArrayItem(references,i);
        const char *title = get(example,"title")->valuestring;
        const char *code = get(example,"code")->valuestring;
        assert(equivalent(get(example,"title"),get(expected,"title"),0));
        assert(equivalent(get(example,"sha256"),get(expected,"sha256"),0));
        char error[1024]; cJSON *metadata = NULL;
        slider_begin();
        JsProgram *program = javascript_compile(code,120,&metadata,error,sizeof(error));
        if (!program) { fprintf(stderr,"%s: %s\n",title,error); abort(); }
        const cJSON *expected_meta = get(expected,"metadata");
        const char *fields[] = {"bpm","samples","visuals","csound"};
        for (unsigned f = 0; f < sizeof(fields)/sizeof(fields[0]); f++)
            assert(equivalent(get(metadata,fields[f]),get(expected_meta,fields[f]),0));
        const cJSON *tempo = get(metadata,"bpm");
        double seconds = 240/(cJSON_IsNumber(tempo) ? tempo->valuedouble : 120);
        const cJSON *cycle;
        cJSON_ArrayForEach(cycle,get(expected,"cycles")) {
            double begin = get(cycle,"begin")->valuedouble;
            cJSON *actual = javascript_query_cps(program,begin,begin+1,1/seconds);
            const cJSON *expected_events = get(cycle,"events");
            if (!cJSON_IsArray(actual) || cJSON_GetArraySize(actual) != cJSON_GetArraySize(expected_events)) {
                fprintf(stderr,"%s cycle %.0f: event count mismatch\n",title,begin); abort();
            }
            for (int e = 0; e < cJSON_GetArraySize(actual); e++) {
                const cJSON *hap = cJSON_GetArrayItem(actual,e);
                const cJSON *want = cJSON_GetArrayItem(expected_events,e);
                const char *keys[] = {"begin","end","value"};
                for (int k = 0; k < 3; k++) {
                    if (!equivalent(get(hap,keys[k]),get(want,keys[k]),0)) {
                        fprintf(stderr,"%s cycle %.0f event %d: %s mismatch\n",title,begin,e,keys[k]); abort();
                    }
                }
                AudioEvent event;
                if (js_audio_event(hap,0,seconds,&event)) {
                    assert(isfinite(event.time) && isfinite(event.duration) && event.duration > 0);
                    assert(isfinite(event.freq) && isfinite(event.gain));
                    DspState state;
                    dsp_init(&state,&event.dsp);
                    for (int sample = 0; sample < 256; sample++)
                        assert(isfinite(dsp_process(&state,.1*sin(sample*.1),sample/SR,event.duration)));
                }
                events++;
            }
            cJSON_Delete(actual);
        }
        printf("PASS %s\n",title);
        if (!strcmp(title,"Arpoon")) check_piano_roll_events(code);
        supported++;
        cJSON_Delete(metadata); javascript_free(program);
    }
    printf("Published examples: %d evaluated; %d reference events checked\n",supported,events);
    cJSON_Delete(corpus); cJSON_Delete(reference);
    return 0;
}
