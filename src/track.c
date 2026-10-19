#include "track.h"
#include "websamp.h"
#include "audio.h"
#include "scheduler.h"
#include "slider.h"
#include <ctype.h>
#include <limits.h>

Track tracks[MAX_TRACKS];
int track_count = 0;
pthread_mutex_t tracks_lock = PTHREAD_MUTEX_INITIALIZER;
static char current_code_empty[] = "";
char *current_code = current_code_empty;

int current_code_set(const char *s) {
    if (!s) s = "";
    if (s == current_code) return 1;
    size_t n = strlen(s);
    if (n > (size_t)INT_MAX) return 0;
    char *p = malloc(n + 1);
    if (!p) return 0;
    memcpy(p, s, n + 1);
    if (current_code != current_code_empty) free(current_code);
    current_code = p;
    return 1;
}

static void free_tracks(void) {
    for (int i=0;i<track_count;i++) javascript_free(tracks[i].javascript);
    track_count=0;
}

int track_load_from_code(const char *code_buf) {
    if (!code_buf) code_buf = "";
    if (strlen(code_buf) > (size_t)INT_MAX) {
        log_console("code exceeds the editor offset limit");
        return 0;
    }
    JsProgram *program = NULL;
    cJSON *metadata = NULL;
    {
        char error[512];
        program = javascript_compile(code_buf,audio_get_bpm(),&metadata,error,sizeof(error));
        pthread_mutex_lock(&tracks_lock);
        JsProgram *previous=track_count==1?tracks[0].javascript:NULL;
        javascript_retain(previous);
        pthread_mutex_unlock(&tracks_lock);
        if (program && !javascript_prepare_csound_from(program,metadata,previous,error,sizeof(error))) {
            javascript_free(program); program = NULL;
            cJSON_Delete(metadata); metadata = NULL;
        }
        javascript_free(previous);
        if (!program) {
            pthread_mutex_lock(&tracks_lock);
            int stored = current_code_set(code_buf);
            pthread_mutex_unlock(&tracks_lock);
            if (!stored) log_console("out of memory");
            else log_console("JavaScript: %s (keeping the last playable pattern)",error);
            return 0;
        }
        char *samples = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(metadata,"samples"));
        websamp_load_arguments(samples);
        free(samples);
    }
    pthread_mutex_lock(&tracks_lock);
    if (!current_code_set(code_buf)) {
        pthread_mutex_unlock(&tracks_lock);
        javascript_free(program);
        cJSON_Delete(metadata);
        log_console("out of memory");
        return 0;
    }
    double now=audio_time_now(), music_cursor=0, tick=now;
    double boundary=round((now+.01)*10000)/10000+.1;
    int replace_javascript=track_count==1 && tracks[0].javascript;
    int continuous=replace_javascript && audio_is_playing() && tracks[0].js_seconds>0;
    if (continuous) {
        music_cursor=tracks[0].js_cursor;
        boundary=tracks[0].js_phase+.1;
        tick=tracks[0].js_tick;
        if (boundary<now+.05) {
            music_cursor+=(now+.1-boundary)/tracks[0].js_seconds;
            boundary=now+.1; tick=now;
        }
    }
    free_tracks();
    /* Web output has already accepted these notes. Replacing a pattern, even
       after Stop, invalidates producers without withdrawing committed audio. */
    if (replace_javascript) event_queue_new_program(HUGE_VAL);
    else event_queue_clear();
    audio_set_csound(javascript_csound(program));
    audio_set_reference_mode(1);
    javascript_activate_sliders(program);
    Track *t = &tracks[0];
    memset(t,0,sizeof(*t));
    t->slider_revision=slider_revision();
    t->javascript = program;
    const cJSON *visuals = cJSON_GetObjectItemCaseSensitive(metadata,"visuals");
    t->visual_requests = cJSON_IsNumber(visuals) ? visuals->valueint & 3 : 0;
    t->compile_generation = event_queue_generation();
    t->active = 1;
    snprintf(t->keyword,sizeof(t->keyword),"javascript");
    track_count = 1;
    const cJSON *tempo = cJSON_GetObjectItemCaseSensitive(metadata,"bpm");
    if (cJSON_IsNumber(tempo) && isfinite(tempo->valuedouble)) audio_set_bpm(tempo->valuedouble);
    t->js_seconds=audio_sec_per_cycle();
    t->js_cursor=music_cursor; t->js_origin=isfinite(t->js_seconds)?boundary-music_cursor*t->js_seconds:boundary; t->js_tick=tick;
    t->js_anchor_cycle=music_cursor;
    t->js_anchor_time=boundary; t->js_phase=boundary-.1;
    t->cycle_index=music_cursor>=INT_MAX?INT_MAX:music_cursor<=0?0:(int)floor(music_cursor);
    t->next_cycle=isfinite(t->js_seconds)?t->js_origin+t->cycle_index*t->js_seconds:boundary;
    cJSON_Delete(metadata);
    pthread_mutex_unlock(&tracks_lock);
    if (!continuous) scheduler_song_sync();
    log_console("Loaded JavaScript/Strudel pattern, bpm %.0f",audio_get_bpm());
    return 1;
}
