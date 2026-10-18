#include "csound_host.h"
#include "javascript.h"
#include "slider.h"
#include "track.h"
#include <sys/stat.h>
#define CSOUND_TEST_MARGIN 256
#include <assert.h>

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
    if (midi || !strcmp(instrument,"Test")) assert(crossings >= 8 && crossings <= 12);
    cJSON_Delete(value);
}
#endif

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1],"--csound-worker")) return csound_worker_main();
#ifndef HAVE_CSOUND
    puts("SKIP native Csound: install csound development headers and rebuild");
#else
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
    assert(track_load_from_code("note('c3').csound('triangle')"));
    JsProgram *playing = tracks[0].javascript;
    assert(playing && javascript_csound(playing));
    log_console_stderr(0);
    int accepted_invalid = track_load_from_code("await loadCsound('invalid orchestra'); note('c3').csound('Tone')");
    log_console_stderr(1);
    assert(!accepted_invalid);
    puts("PASS expected rejection: invalid orchestra retains previous playback");
    assert(tracks[0].javascript == playing);
    assert(track_load_from_code("bpm 120\nnotes c4"));
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
#endif
    return 0;
}
