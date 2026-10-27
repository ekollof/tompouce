/* Offline diagnostic executable: production callback, no audio device/UI. */
#include "../src/audio.c"
#include "../src/js_audio.c"
#include <stdint.h>

static int compare_events(const void *a, const void *b) {
    const AudioEvent *left = a, *right = b;
    return (left->time > right->time) - (left->time < right->time);
}

int main(int argc, char **argv) {
    log_console_stderr(1);
    if (argc != 3) { fprintf(stderr,"usage: %s request.json output.f32\n",argv[0]); return 1; }
    FILE *input = fopen(argv[1],"rb");
    if (!input) return 1;
    char *source = malloc(2*1024*1024+1);
    if (!source) { fclose(input); return 1; }
    size_t size = fread(source,1,2*1024*1024,input);
    int extra = fgetc(input), failed = ferror(input);
    fclose(input);
    source[size] = 0;
    cJSON *request = !failed && extra == EOF ? cJSON_Parse(source) : NULL;
    free(source);
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(request,"code");
    const cJSON *sound = cJSON_GetObjectItemCaseSensitive(request,"sound");
    const cJSON *cycles = cJSON_GetObjectItemCaseSensitive(request,"cycles");
    const cJSON *cps = cJSON_GetObjectItemCaseSensitive(request,"cps");
    const cJSON *events_flag = cJSON_GetObjectItemCaseSensitive(request,"eventsOnly");
    int events_only=cJSON_IsTrue(events_flag);
    if (!cJSON_IsString(code) || !cJSON_IsString(sound) || !cJSON_IsNumber(cycles) ||
        !cJSON_IsNumber(cps) || !isfinite(cycles->valuedouble) || cycles->valuedouble < 1 ||
        cycles->valuedouble > 32 || !isfinite(cps->valuedouble) || cps->valuedouble < .1 || cps->valuedouble > 2 ||
        (events_flag && !cJSON_IsBool(events_flag))) {
        cJSON_Delete(request); return 1;
    }
    char error[512]; cJSON *metadata = NULL;
    JsProgram *program = javascript_compile(code->valuestring,cps->valuedouble*240,&metadata,error,sizeof(error));
    if (!program) { fprintf(stderr,"%s\n",error); cJSON_Delete(request); return 1; }
    char *samples = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(metadata,"samples"));
    if (!samples) { cJSON_Delete(metadata); javascript_free(program); cJSON_Delete(request); return 1; }
    if (!events_only) websamp_load_arguments(samples);
    free(samples);
    double query_cps=cps->valuedouble;
    const cJSON *bpm=cJSON_GetObjectItemCaseSensitive(metadata,"bpm");
    if (events_only && cJSON_IsNumber(bpm) && isfinite(bpm->valuedouble) && bpm->valuedouble>0)
        query_cps=bpm->valuedouble/240;
    cJSON *haps = cJSON_CreateArray();
    for (double begin = 0; haps && begin < cycles->valuedouble; begin += 1) {
        cJSON *part = javascript_query_cps(program,begin,fmin(begin+1,cycles->valuedouble),query_cps);
        if (!cJSON_IsArray(part)) { cJSON_Delete(part); cJSON_Delete(haps); haps = NULL; break; }
        cJSON *item;
        while ((item = cJSON_DetachItemFromArray(part,0))) {
            if (!cJSON_AddItemToArray(haps,item)) { cJSON_Delete(item); cJSON_Delete(haps); haps = NULL; break; }
        }
        cJSON_Delete(part);
    }
    int count = cJSON_GetArraySize(haps);
    if (events_only) {
        cJSON *result=cJSON_CreateObject();
        if (!result || !cJSON_IsArray(haps) || !cJSON_AddNumberToObject(result,"cps",query_cps) ||
            !cJSON_AddItemToObject(result,"events",haps)) {
            cJSON_Delete(result); cJSON_Delete(haps); cJSON_Delete(metadata);
            javascript_free(program); cJSON_Delete(request); return 1;
        }
        char *json=cJSON_PrintUnformatted(result);
        FILE *output=json?fopen(argv[2],"wb"):NULL;
        int ok=output && fwrite(json,1,strlen(json),output)==strlen(json);
        if (output && fclose(output)) ok=0;
        free(json); cJSON_Delete(result); cJSON_Delete(metadata);
        javascript_free(program); cJSON_Delete(request); return ok?0:1;
    }
    AudioEvent *events = calloc((size_t)count+1,sizeof(*events));
    if (!events || !cJSON_IsArray(haps)) { fprintf(stderr,"pattern query failed\n"); free(events); cJSON_Delete(haps); cJSON_Delete(metadata); javascript_free(program); cJSON_Delete(request); return 1; }
    int used = 0; const cJSON *hap;
    cJSON_ArrayForEach(hap,haps) {
        const cJSON *value = cJSON_GetObjectItemCaseSensitive(hap,"value");
        const cJSON *begin = cJSON_GetObjectItemCaseSensitive(hap,"begin");
        if (*sound->valuestring && strcmp(text_control(value,"s"),sound->valuestring)) continue;
        AudioEvent event;
        /* Fixed-cycle mapping: the browser's offline render ignores
           event-level cps pivots for onset mapping, so tempo changes are
           covered by the live engine harness instead of static gates. */
        if (!js_audio_event(hap,0,1/cps->valuedouble,&event)) continue;
        if (getenv("TOMPOUCE_RENDER_DEBUG") && cJSON_IsNumber(begin))
            fprintf(stderr,"render hap onset=%.6f target=%.6f duration=%.6f token=%s\n",
                begin->valuedouble,begin->valuedouble/cps->valuedouble,event.duration,event.token);
        if (!js_audio_resolve_sample(&event,value) || (oscillator(text_control(value,"s")) < 0 && event.smp < 0 && !event.native_synth && !event.stretch_buffer)) {
            fprintf(stderr,"sample resolution failed\n"); free(events); cJSON_Delete(haps); cJSON_Delete(metadata); javascript_free(program); cJSON_Delete(request); return 1;
        }
        events[used++] = event;
        /* Decide shared delay automation at preparation time, matching the
           live commit path and the web's preparation-time parameter guards.
           A future return that already matches the current setting is skipped
           here; deciding at trigger time would see advanced state instead. */
        audio_prepare_delay(&events[used-1]);
        /* Static waveform gates intentionally use completed IRs. Live pending
           and failure behavior is exercised separately by the room harness. */
        if (event.dsp.p[DSP_ROOM]>0 && event.room_sample<0)
            events[used-1].ir_kernel=convolution_prepare_reverb(event.dsp.p[DSP_SIZE],
                event.dsp.p[DSP_ROOMFADE],event.dsp.p[DSP_ROOMLP],event.dsp.p[DSP_ROOMDIM]);
        /* The eager web renderer connects room sends while scheduling all
           haps, not at their audible onsets. Retain the same graph width and
           custom-IR reuse snapshots during offline preparation. */
        audio_prepare_room(&events[used-1]);
    }
    qsort(events,(size_t)used,sizeof(*events),compare_events);
    int seed_supersaw=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(request,"seedSupersaw"));
    if (seed_supersaw && (used!=1 || events[0].inst!=INST_SUPERSAW)) {
        fprintf(stderr,"seeded supersaw comparison requires one supersaw event\n");
        free(events); cJSON_Delete(haps); cJSON_Delete(metadata); javascript_free(program); cJSON_Delete(request); return 1;
    }
    FILE *output = fopen(argv[2],"wb");
    if (!output) { free(events); cJSON_Delete(haps); cJSON_Delete(metadata); javascript_free(program); cJSON_Delete(request); return 1; }
    playing = 1; audio_set_reference_mode(1);
    long frames = lround(cycles->valuedouble/cps->valuedouble*SR);
    int next = 0, ok = 1;
    for (long frame = 0; frame < frames; frame++) {
        while (next < used && lround(events[next].time*SR) <= frame) {
            audio_trigger_event(&events[next++]);
            if (seed_supersaw) for (int i=0;i<MAX_VOICES;i++)
                if (voices[i].active && voices[i].onset_serial==voice_onset_serial)
                    dsp_supersaw_init(&voices[i].dsp,0xc0ffee);
        }
        float stereo[2];
        pa_callback(NULL,stereo,1,NULL,0,NULL);
        if (fwrite(stereo,sizeof(float),2,output) != 2) { ok = 0; break; }
    }
    if (fclose(output)) ok = 0;
    fprintf(stderr,"rendered %ld stereo frames, %d events at %.0f Hz\n",frames,used,SR);
    free(events); cJSON_Delete(haps); cJSON_Delete(metadata); javascript_free(program); cJSON_Delete(request);
    return ok ? 0 : 1;
}
