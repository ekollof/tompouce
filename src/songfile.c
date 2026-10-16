#include "songfile.h"
#include "scheduler.h"
#include "common.h"
#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

int songfile_save(const char *path, const char *editor_code, double bpm, int quiet) {
    if (!path || !path[0] || !editor_code) {
        if (!quiet) log_console("save failed: bad args");
        return -1;
    }
    int n = scheduler_song_count();
    int pos = scheduler_song_pos();
    /* no list yet: the editor buffer is the song */
    int total = (n > 0) ? n : 1;
    if (pos < 0 || pos >= total) pos = 0;
    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;
    cJSON_AddStringToObject(root, "app", "strudelterm");
    cJSON_AddNumberToObject(root, "version", 2);
    cJSON_AddNumberToObject(root, "bpm", bpm);
    cJSON_AddStringToObject(root, "code", editor_code);
    cJSON *arr = cJSON_CreateArray();
    if (!arr) { cJSON_Delete(root); return -1; }
    cJSON_AddItemToObject(root, "tracks", arr);
    char tmp[SONG_CODE_LEN];
    for (int i = 0; i < total; i++) {
        if (n > 0 && i != pos && scheduler_track_code(i, tmp, sizeof(tmp)))
            cJSON_AddItemToArray(arr, cJSON_CreateString(tmp));
        else
            cJSON_AddItemToArray(arr, cJSON_CreateString(editor_code));
    }
    cJSON_AddNumberToObject(root, "active", pos);
    char *txt = cJSON_Print(root);
    cJSON_Delete(root);
    if (!txt) { if (!quiet) log_console("save failed"); return -1; }
    FILE *f = fopen(path, "w");
    if (!f) { if (!quiet) log_console("save failed: %.200s", path); free(txt); return -1; }
    size_t len = strlen(txt);
    int ok = (fwrite(txt, 1, len, f) == len);
    free(txt);
    if (fclose(f) != 0) ok = 0;
    if (!ok) { if (!quiet) log_console("save failed: %.200s", path); return -1; }
    return total;
}

int songfile_load(const char *path, char *editor_out, size_t editor_len,
    double *bpm_out, int quiet) {
    if (editor_out && editor_len > 0) editor_out[0] = 0;
    if (bpm_out) *bpm_out = 0;
    if (!path || !path[0]) {
        if (!quiet) log_console("load failed: bad path");
        return -1;
    }
    FILE *f = fopen(path, "r");
    if (!f) { if (!quiet) log_console("load failed: %.200s", path); return -1; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0 || sz > 262144) {
        fclose(f);
        if (!quiet) log_console("load failed: bad size");
        return -1;
    }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = 0;
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root || !cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        if (!quiet) log_console("load failed: bad json");
        return -1;
    }
    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (!code || !cJSON_IsString(code) || !code->valuestring) {
        cJSON_Delete(root);
        if (!quiet) log_console("load failed: no code");
        return -1;
    }
    cJSON *bpm = cJSON_GetObjectItem(root, "bpm");
    double b = 0;
    if (bpm && cJSON_IsNumber(bpm) && isfinite(bpm->valuedouble)) b = bpm->valuedouble;
    if (bpm_out) *bpm_out = b;
    /* gather tracks (v2) or fall back to the single code buffer (v1) */
    static char tracks[MAX_SONG_TRACKS][SONG_CODE_LEN];
    const char *ptrs[MAX_SONG_TRACKS];
    int n = 0, active = 0;
    cJSON *arr = cJSON_GetObjectItem(root, "tracks");
    if (arr && cJSON_IsArray(arr)) {
        int m = cJSON_GetArraySize(arr);
        for (int i = 0; i < m && n < MAX_SONG_TRACKS; i++) {
            cJSON *e = cJSON_GetArrayItem(arr, i);
            if (!e || !cJSON_IsString(e) || !e->valuestring) continue;
            snprintf(tracks[n], sizeof(tracks[n]), "%s", e->valuestring);
            ptrs[n] = tracks[n];
            n++;
        }
        cJSON *ac = cJSON_GetObjectItem(root, "active");
        if (ac && cJSON_IsNumber(ac) && isfinite(ac->valuedouble)) active = (int)ac->valuedouble;
    }
    if (n == 0) {
        snprintf(tracks[0], sizeof(tracks[0]), "%s", code->valuestring);
        ptrs[0] = tracks[0];
        n = 1;
        active = 0;
    }
    if (active < 0) active = 0;
    if (active >= n) active = n - 1;
    cJSON_Delete(root);
    scheduler_song_replace(ptrs, n, active);
    if (editor_out && editor_len > 0)
        snprintf(editor_out, editor_len, "%s", tracks[active]);
    return n;
}
