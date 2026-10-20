#define websamp_fetch_text csound_test_fetch
#include "../src/csound_host.c"
#undef websamp_fetch_text
#include "javascript.h"
#include "slider.h"
#include "track.h"
#include <sys/stat.h>
#include <stdatomic.h>
#define CSOUND_TEST_MARGIN 256
#include <assert.h>

char *websamp_fetch_text(const char *url);
static int orchestra_fetches, missing_orchestra_fetches, gone_orchestra_fetches, lost_orchestra_fetches,
    kept_orchestra_fetches;
char *csound_test_fetch(const char *url) {
    if (!strcmp(url,"https://raw.githubusercontent.com/fixture/orchestra/main/tone.orc")) {
        orchestra_fetches++;
        return strdup_safe("instr Cached\na1 oscili p5,p4\nout a1,a1\nendin\n");
    }
    if (!strcmp(url,"https://raw.githubusercontent.com/fixture/orchestra/main/missing.orc")) {
        missing_orchestra_fetches++; return NULL;
    }
    if (!strcmp(url,"https://raw.githubusercontent.com/fixture/orchestra/main/gone.orc")) {
        gone_orchestra_fetches++; return NULL;
    }
    if (!strcmp(url,"https://raw.githubusercontent.com/fixture/orchestra/main/lost.orc")) {
        lost_orchestra_fetches++; return NULL;
    }
    if (!strcmp(url,"https://raw.githubusercontent.com/fixture/orchestra/main/kept.orc")) {
        kept_orchestra_fetches++;
        return strdup_safe("instr Kept\na1 oscili p5,p4\nout a1,a1\nendin\n");
    }
    return websamp_fetch_text(url);
}

#ifdef HAVE_CSOUND
static CsoundProgram *compile(const char *code, int rejection_expected) {
    cJSON *loads = cJSON_CreateArray(), *item = cJSON_CreateObject();
    assert(loads && item && cJSON_AddStringToObject(item,"code",code));
    assert(cJSON_AddItemToArray(loads,item));
    char error[1024];
    CsoundProgram *program = csound_program_create(loads,error,sizeof(error));
    if (!program && !rejection_expected) fprintf(stderr,"Unexpected Csound failure: %s\n",error);
    cJSON_Delete(loads); return program;
}
static int update(CsoundProgram *program, const char *code, int rejection_expected) {
    cJSON *loads=cJSON_CreateArray(), *item=cJSON_CreateObject();
    assert(loads && item && cJSON_AddStringToObject(item,"code",code) && cJSON_AddItemToArray(loads,item));
    char error[1024];
    int ok=csound_program_update(program,loads,error,sizeof(error))==CSOUND_UPDATE_OK;
    if (!ok && !rejection_expected) fprintf(stderr,"Unexpected update failure: %s\n",error);
    cJSON_Delete(loads); return ok;
}
#define CONTINUITY_FRAMES 32768
typedef struct {
    CsoundProgram *program, *control;
    atomic_int frame;
    double pcm[CONTINUITY_FRAMES][3];
} Capture;
static void *capture(void *opaque) {
    Capture *state=opaque;
    struct timespec warmup={0,10000000}; nanosleep(&warmup,NULL);
    for (int at=0;at<CONTINUITY_FRAMES;at+=64) {
        for (int i=at;i<at+64;i++) {
            double left=0,right=0,baseline=0,unused=0;
            csound_program_sample(state->program,&left,&right);
            csound_program_sample(state->control,&baseline,&unused);
            state->pcm[i][0]=left; state->pcm[i][1]=baseline; state->pcm[i][2]=right;
        }
        atomic_store_explicit(&state->frame,at+64,memory_order_release);
        struct timespec pace={0,1500000}; nanosleep(&pace,NULL);
    }
    return NULL;
}
static void await_frame(Capture *state, int frame) {
    for (int i=0;i<4000 && atomic_load_explicit(&state->frame,memory_order_acquire)<frame;i++) {
        struct timespec pace={0,1000000}; nanosleep(&pace,NULL);
    }
    assert(atomic_load_explicit(&state->frame,memory_order_acquire)>=frame);
}
static void test_live_orchestra(void) {
    const char *source="giRetained init 2\ninstr Held\na1 oscili p5,p4\na0 init 0\nout a1,a0\nendin\n";
    Capture *state=calloc(1,sizeof(*state)); assert(state);
    state->program=compile(source,0); state->control=compile(source,0);
    assert(state->program && state->control); atomic_init(&state->frame,0);
    cJSON *value=cJSON_Parse("{\"csound\":\"Held\",\"gain\":0.5}"); assert(value);
    assert(csound_program_schedule(state->program,value,.02,.8,441));
    assert(csound_program_schedule(state->control,value,.02,.8,441));
    csound_program_activate(state->program,0); csound_program_activate(state->control,0);
    pthread_t thread; assert(!pthread_create(&thread,NULL,capture,state));
    await_frame(state,4096);
    assert(update(state->program,"instr Held\na1 oscili p5,2*p4\na0 init 0\nout a0,a1\nendin\n",0));
    assert(csound_program_schedule(state->program,value,.4,.1,441));
    await_frame(state,8192);
    assert(!update(state->program,"invalid orchestra",1));
    char include_path[]="/tmp/tompouce-csound-include-XXXXXX", include_code[256];
    int fd=mkstemp(include_path); assert(fd>=0);
    const char forbidden[]="giEscaped init 99\n";
    assert(write(fd,forbidden,sizeof(forbidden)-1)==(ssize_t)sizeof(forbidden)-1 && !close(fd));
    snprintf(include_code,sizeof(include_code),"#include \"%s\"\n",include_path);
    assert(!update(state->program,include_code,1)); /* Compiler thread is confined too. */
    assert(!unlink(include_path));
    /* Refill the audio-rate constant each block, including sample-accurate
       start offsets. An init-only audio buffer retains the first block's gap. */
    const char *other="giRetained += 1\ninstr Other\na1 = p5*giRetained\na0 init 0\nout a0,a1\nendin\n";
    assert(update(state->program,other,0));
    assert(cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(value,"csound"),"Other"));
    assert(csound_program_schedule(state->program,value,.6,.05,441));
    await_frame(state,28000);
    assert(update(state->program,other,0)); /* Identical inline loads still run globals. */
    assert(csound_program_schedule(state->program,value,.69,.03,441));
    assert(!pthread_join(thread,NULL));
    double left_energy=0,right_energy=0; int crossings=0;
    for (int i=0;i<CONTINUITY_FRAMES;i++) {
        assert(isfinite(state->pcm[i][0]) && isfinite(state->pcm[i][2]));
        assert(fabs(state->pcm[i][0]-state->pcm[i][1])<1e-6);
        left_energy+=state->pcm[i][0]*state->pcm[i][0];
        if (i>=(int)(.43*SR) && i<(int)(.48*SR)) {
            right_energy+=state->pcm[i][2]*state->pcm[i][2];
            if (state->pcm[i-1][2]<0 && state->pcm[i][2]>=0) crossings++;
        }
        if (i>=(int)(.62*SR) && i<(int)(.63*SR)) {
            if (fabs(state->pcm[i][2]-.3)>=1e-6) fprintf(stderr,"global continuity frame=%d right=%.9g expected=.3\n",i,state->pcm[i][2]);
            assert(fabs(state->pcm[i][2]-.3)<1e-6);
        }
        if (i>=(int)(.71*SR) && i<(int)(.715*SR)) assert(fabs(state->pcm[i][2]-.4)<1e-6);
    }
    assert(left_energy>1 && right_energy>.1 && crossings>=43 && crossings<=45);
    cJSON_Delete(value); csound_program_free(state->program); csound_program_free(state->control); free(state);
    puts("live orchestra: held PCM, new definitions, globals and failed update continuity ok");
}
static void test_orchestra_url_cache(void) {
    CsoundProgram *program=compile("",0); assert(program);
    cJSON *loads=cJSON_Parse("[{\"url\":\"github:fixture/orchestra/main/tone.orc\"}]");
    char error[1024]; assert(loads);
    assert(csound_program_update(program,loads,error,sizeof(error)) && orchestra_fetches==1);
    assert(cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(loads,0),"url"),
        "https://raw.githubusercontent.com/fixture/orchestra/main/tone.orc"));
    assert(csound_program_update(program,loads,error,sizeof(error)) && orchestra_fetches==1);
    struct timespec idle={3,200000000}; nanosleep(&idle,NULL);
    assert(update(program,"instr AfterPause\na1 oscili p5,p4\nout a1,a1\nendin\n",0));
    cJSON_Delete(loads); csound_program_free(program);
}
/* Failed orchestra fetches stay rejected without refetching, matching the
   deployed loader's session promise cache (one observable fetch for two
   loadOrc calls). A distinct dead URL still fetches once, and the program
   keeps working afterwards. */
static void test_orchestra_failed_url_cache(void) {
    CsoundProgram *program=compile("instr Alive\na1 oscili p5,p4\nout a1,a1\nendin\n",0); assert(program);
    cJSON *loads=cJSON_Parse("[{\"url\":\"github:fixture/orchestra/main/missing.orc\"}]"); assert(loads);
    char error[1024],first_error[1024];
    assert(!csound_program_update(program,loads,error,sizeof(error)) && missing_orchestra_fetches==1);
    snprintf(first_error,sizeof(first_error),"%s",error);
    assert(!csound_program_update(program,loads,error,sizeof(error)) && missing_orchestra_fetches==1);
    assert(!strcmp(error,first_error));
    assert(cJSON_SetValuestring(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(loads,0),"url"),
        "github:fixture/orchestra/main/gone.orc"));
    assert(!csound_program_update(program,loads,error,sizeof(error)) && gone_orchestra_fetches==1);
    assert(missing_orchestra_fetches==1);
    assert(update(program,"instr AfterFailure\na1 oscili p5,p4\nout a1,a1\nendin\n",0));
    cJSON_Delete(loads); csound_program_free(program);
    puts("failed orchestra URLs reject from cache without refetching; the program survives");
}
/* Loads that succeed before a later failure stay available, matching the
   deployed page engine. The first prepare fails on the dead URL and parks
   its engine; later prepares adopt it without refetching. Uses direct
   prepares with unique fixture URLs, so it runs anywhere in the suite. */
static void test_csound_stash_adopts(void) {
    char error[1024]; cJSON *metadata=NULL;
    JsProgram *bad=javascript_compile(
        "await loadOrc('github:fixture/orchestra/main/kept.orc');"
        "await loadOrc('github:fixture/orchestra/main/lost.orc'); note(60)",
        120,&metadata,error,sizeof(error));
    assert(bad && metadata);
    assert(!javascript_prepare_csound_from(bad,metadata,NULL,error,sizeof(error)));
    assert(kept_orchestra_fetches==1 && lost_orchestra_fetches==1);
    cJSON_Delete(metadata); javascript_free(bad);
    metadata=NULL;
    JsProgram *plain=javascript_compile("note(60)",120,&metadata,error,sizeof(error));
    assert(plain && metadata);
    assert(javascript_prepare_csound_from(plain,metadata,NULL,error,sizeof(error)));
    CsoundProgram *engine=javascript_csound(plain); assert(engine);
    assert(kept_orchestra_fetches==1 && lost_orchestra_fetches==1);
    cJSON_Delete(metadata); metadata=NULL;
    JsProgram *reuse=javascript_compile(
        "await loadOrc('github:fixture/orchestra/main/kept.orc'); note(60)",
        120,&metadata,error,sizeof(error));
    assert(reuse && metadata);
    assert(javascript_prepare_csound_from(reuse,metadata,NULL,error,sizeof(error)));
    assert(javascript_csound(reuse)==engine);
    assert(kept_orchestra_fetches==1 && lost_orchestra_fetches==1);
    cJSON_Delete(metadata); javascript_free(reuse); javascript_free(plain);
    puts("failed prepare parks its engine; later prepares adopt it without refetching");
}
static void render(CsoundProgram *program, const char *instrument, int midi) {
    cJSON *value = cJSON_CreateObject();
    assert(value);
    if (instrument[0] >= '0' && instrument[0] <= '9') assert(cJSON_AddNumberToObject(value,"csound",strtod(instrument,NULL)));
    else assert(cJSON_AddStringToObject(value,"csound",instrument));
    if (midi) assert(cJSON_AddBoolToObject(value,"_csoundMidi",1));
    assert(csound_program_schedule(program,value,.03,.1,220));
    csound_program_activate(program,0);
    double sum = 0, previous = 0; int first = -1, crossings = 0;
    for (int frame = 0; frame < 8820; frame++) {
        if (frame%64 == 0) { struct timespec pause = {0,1500000}; nanosleep(&pause,NULL); }
        double left = 0, right = 0;
        csound_program_sample(program,&left,&right);
        assert(isfinite(left) && isfinite(right));
        if (first < 0 && fabs(left)+fabs(right) > 1e-7) first = frame;
        sum += left*left+right*right;
        if (frame > 2000 && frame < 4000 && previous < 0 && left >= 0) crossings++;
        previous = left;
    }
    assert(sum > .001);
    assert(first >= (int)(.03*SR)-CSOUND_TEST_MARGIN && first <= (int)(.03*SR)+CSOUND_TEST_MARGIN);
    if (!strcmp(instrument,"Test") && !midi) {
        if (first!=(int)llround(.03*SR)+1) fprintf(stderr,"sample-accurate Csound onset: first=%d expected=%d\n",first,(int)llround(.03*SR)+1);
        assert(first==(int)llround(.03*SR)+1); /* oscili's first sample is zero. */
    }
    if (midi || !strcmp(instrument,"Test")) assert(crossings >= 8 && crossings <= 12);
    cJSON_Delete(value);
}
#endif

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1],"--csound-worker")) {
#ifdef TOMPOUCE_TEST_CSOUND_WORKER
        execl(TOMPOUCE_TEST_CSOUND_WORKER,"test_csound","--csound-worker",(char *)NULL); return 1;
#else
        return csound_worker_main();
#endif
    }
#ifndef HAVE_CSOUND
    puts("SKIP native Csound: install csound development headers and rebuild");
#else
    test_live_orchestra();
    test_orchestra_url_cache();
    test_orchestra_failed_url_cache();
    CsoundProgram *program = compile("instr Test\na1 oscili p5,p4\nout a1,a1\nendin\n",0);
    assert(program); render(program,"Test",0); csound_program_free(program);
    program = compile("instr 1\na1 oscili (p5/127)*.2,cpsmidinn(p4)\nout a1,a1\nendin\n",0);
    assert(program); render(program,"1.25",1); csound_program_free(program);
    assert(!compile("this is not an orchestra",1));
    puts("PASS expected rejection: invalid orchestra syntax");
    assert(!compile("while 1 == 1 do\nod\n",1));
    puts("PASS expected rejection: orchestra execution timeout");
    char confined_path[] = "/tmp/strudel-csound-confined-XXXXXX";
    int fd = mkstemp(confined_path);
    assert(fd >= 0 && !close(fd));
    char orchestra[512];
    snprintf(orchestra,sizeof(orchestra),"fprints \"%s\", \"forbidden\"\n",confined_path);
    program = compile(orchestra,1);
    csound_program_free(program);
    struct stat file_status;
    assert(!stat(confined_path,&file_status) && file_status.st_size == 0);
    assert(!unlink(confined_path));
    puts("PASS expected rejection: orchestra file access");
    assert(track_load_from_code("await loadCsound`instr Tone\na1 oscili p5,p4\nout a1,a1\nendin`; note('c3').csound('Tone')"));
    CsoundProgram *registry=javascript_csound(tracks[0].javascript);
    assert(track_load_from_code("note('c3').csound('triangle')"));
    assert(javascript_csound(tracks[0].javascript)==registry);
    assert(track_load_from_code("note(60).gain(0)"));
    assert(javascript_csound(tracks[0].javascript)==registry);
    assert(track_load_from_code("note('c3').csound('Tone')"));
    assert(javascript_csound(tracks[0].javascript)==registry);
    JsProgram *playing = tracks[0].javascript;
    assert(playing && javascript_csound(playing));
    log_console_stderr(0);
    int accepted_invalid = track_load_from_code("await loadCsound('invalid orchestra'); note('c3').csound('Tone')");
    log_console_stderr(1);
    assert(accepted_invalid);
    puts("PASS web loader behavior: orchestra syntax reports an error while existing definitions remain");
    assert(tracks[0].javascript != playing && javascript_csound(tracks[0].javascript)==registry);
    log_console_stderr(0);
    assert(track_load_from_code(
        "await loadCsound('giOrder init 1\\n');"
        "await loadCsound('invalid orchestra');"
        "await loadCsound('giOrder += 1\\ninstr Ordered\\na1 = p5*giOrder\\nout a1,a1\\nendin\\n');"
        "note(60).csound('Ordered')"));
    log_console_stderr(1);
    assert(javascript_csound(tracks[0].javascript)==registry);
    render(registry,"Ordered",0);
    assert(track_load_from_code("note(60).gain(0)"));
    FILE *file = fopen("tests/fixtures/strudel-examples.json","rb");
    assert(file);
    char text[65536]; size_t size = fread(text,1,sizeof(text)-1,file);
    assert(size > 0 && !ferror(file) && feof(file));
    text[size] = 0; fclose(file);
    cJSON *corpus = cJSON_Parse(text);
    assert(corpus);
    const cJSON *example;
    cJSON_ArrayForEach(example,cJSON_GetObjectItemCaseSensitive(corpus,"examples")) {
        const char *title = cJSON_GetObjectItemCaseSensitive(example,"title")->valuestring;
        if (strcmp(title,"CSound demo") && strcmp(title,"Lounge sponge")) continue;
#ifndef HAVE_CSOUND7
        if (!strcmp(title,"Lounge sponge")) { puts("SKIP Lounge sponge native render: its current orchestra requires Csound 7"); continue; }
#endif
        const char *code = cJSON_GetObjectItemCaseSensitive(example,"code")->valuestring;
        char error[1024]; cJSON *metadata = NULL;
        slider_begin();
        JsProgram *js = javascript_compile(code,120,&metadata,error,sizeof(error));
        assert(js);
        if (!javascript_prepare_csound(js,metadata,error,sizeof(error))) {
            fprintf(stderr,"%s: %s\n",title,error); abort();
        }
        program = javascript_csound(js);
        assert(program);
        render(program,!strcmp(title,"CSound demo") ? "CoolSynth" : "FM1",0);
        cJSON_Delete(metadata); javascript_free(js);
    }
    cJSON_Delete(corpus);
    puts("native Csound loaders, rendering, invalid-source and execution-budget checks ok");
    test_csound_stash_adopts();
#endif
    return 0;
}
