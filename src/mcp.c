#include "mcp.h"
#include "common.h"
#include "track.h"
#include "scheduler.h"
#include "audio.h"
#include "ai.h"
#include "tui.h"
#include "midi.h"
#include "banks.h"
#include "songfile.h"
#include "sounds.h"
#include "sound_store.h"
#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <sys/random.h>

FILE *mcp_out_stream = NULL;
int mcp_input_fd = -1;
static int mcp_listen_fd = -1;
static int mcp_port_bound = 0;
static char mcp_token[65];
static int mcp_output_socket = -1, mcp_input_tcp, mcp_send_failed;
static double mcp_client_deadline;
static char *mcp_init_code;

static int mcp_auth_init(int generate) {
    if (mcp_token[0]) return 1;
    const char *configured = getenv("TOMPOUCE_MCP_TOKEN");
    if (configured) {
        if (strlen(configured) != 64) return 0;
        for (int i=0;i<64;i++)
            if (!((configured[i]>='0' && configured[i]<='9') ||
                  (configured[i]>='a' && configured[i]<='f'))) return 0;
        memcpy(mcp_token,configured,65); return 1;
    }
    if (!generate) return 0;
    unsigned char bytes[32]; size_t used=0;
    while (used<sizeof(bytes)) {
        ssize_t got=getrandom(bytes+used,sizeof(bytes)-used,0);
        if (got<0 && errno==EINTR) continue;
        if (got<=0) return 0;
        used+=(size_t)got;
    }
    const char *hex="0123456789abcdef";
    for (int i=0;i<32;i++) { mcp_token[i*2]=hex[bytes[i]>>4]; mcp_token[i*2+1]=hex[bytes[i]&15]; }
    return 1;
}

const char *mcp_tcp_token(void) { return mcp_token[0]?mcp_token:NULL; }

static int mcp_dispatch_tcp(const char *frame) {
    cJSON *request=cJSON_ParseWithOpts(frame,NULL,1);
    const cJSON *auth=cJSON_GetObjectItemCaseSensitive(request,"auth");
    int valid=cJSON_IsObject(request) && cJSON_IsString(auth) && strlen(auth->valuestring)==64;
    unsigned difference=0;
    if (valid) for (int i=0;i<64;i++) difference|=(unsigned char)auth->valuestring[i]^(unsigned char)mcp_token[i];
    valid=valid && !difference && mcp_token[0];
    if (valid) { handle_request(request); mcp_client_deadline=now_seconds()+10; }
    cJSON_Delete(request);
    return valid && !mcp_send_failed;
}

void mcp_set_init_code(const char *code) {
    free(mcp_init_code);
    mcp_init_code = NULL;
    if (!code || !code[0]) return;
    size_t n = strlen(code);
    if (n > (size_t)INT_MAX) return;
    mcp_init_code = malloc(n + 1);
    if (!mcp_init_code) return;
    memcpy(mcp_init_code, code, n + 1);
}

#define MCP_MAX_FRAME (1024 * 1024)
static char mcp_pending[MCP_MAX_FRAME + 1];
static size_t mcp_pending_len = 0;
static int mcp_frame_drop = 0;

static void mcp_take_line(char **out, size_t nl) {
    mcp_pending[nl] = 0;
    *out = strdup_safe(mcp_pending);
    size_t rest = mcp_pending_len - (nl + 1);
    if (rest > 0) memmove(mcp_pending, mcp_pending + nl + 1, rest);
    mcp_pending_len = rest;
}

int mcp_read_frame(int fd, char **out) {
    char tmp[8192];
    size_t received=0;
    *out = NULL;
    for (;;) {
        /* try to extract a complete line from the pending buffer */
        if (mcp_frame_drop) {
            char *nl = memchr(mcp_pending, '\n', mcp_pending_len);
            if (nl) {
                size_t pos = (size_t)(nl - mcp_pending);
                size_t rest = mcp_pending_len - (pos + 1);
                if (rest > 0) memmove(mcp_pending, mcp_pending + pos + 1, rest);
                mcp_pending_len = rest;
                mcp_frame_drop = 0;
                continue;
            }
            mcp_pending_len = 0;
        } else {
            char *nl = memchr(mcp_pending, '\n', mcp_pending_len);
            if (nl) {
                mcp_take_line(out, (size_t)(nl - mcp_pending));
                return 1;
            }
        }
        /* A continuous unterminated sender must yield to the foreground. */
        if (received>=64*1024) return 0;
        ssize_t r = read(fd, tmp, sizeof(tmp));
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        if (r == 0) {
            if (mcp_pending_len > 0 && !mcp_frame_drop) {
                mcp_pending[mcp_pending_len] = 0;
                *out = strdup_safe(mcp_pending);
                mcp_pending_len = 0;
                return 1;
            }
            return -1;
        }
        received+=(size_t)r;
        for (ssize_t i = 0; i < r; i++) {
            if (mcp_frame_drop) {
                if (tmp[i] == '\n') { mcp_frame_drop = 0; mcp_pending_len = 0; }
                continue;
            }
            if (mcp_pending_len >= MCP_MAX_FRAME) {
                /* TCP framing is fail-closed; never skip a hostile preamble
                   and resume dispatching later lines on that connection. */
                if (mcp_output_socket>=0 && fd==mcp_output_socket) return -1;
                mcp_frame_drop = 1;
                mcp_pending_len = 0;
                continue;
            }
            mcp_pending[mcp_pending_len++] = tmp[i];
        }
    }
}

static void send_json(cJSON *obj) {
    FILE *out = mcp_out_stream ? mcp_out_stream : stdout;
    char *s = cJSON_PrintUnformatted(obj);
    if (s) {
        if (mcp_output_socket>=0) {
            /* Never block the foreground or raise SIGPIPE for a peer. A slow
               or disconnected client loses its connection, not the engine. */
            const char *parts[]={s,"\n"};
            for (int i=0;i<2 && !mcp_send_failed;i++) {
                const char *p=parts[i]; size_t left=strlen(p);
                while (left) {
                    ssize_t sent=send(mcp_output_socket,p,left,MSG_NOSIGNAL|MSG_DONTWAIT);
                    if (sent<0 && errno==EINTR) continue;
                    if (sent<=0) { mcp_send_failed=1; break; }
                    p+=sent; left-=(size_t)sent;
                }
            }
        } else {
            fputs(s, out); fputc('\n', out); fflush(out);
        }
        free(s);
    }
    cJSON_Delete(obj);
}

static int session_name_ok(const char *n) {
    if (!n) return 0;
    size_t L = strlen(n);
    if (L == 0 || L > 64) return 0;
    for (size_t i = 0; i < L; i++) {
        char c = n[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return 1;
}

static void session_base(char *base, size_t n) {
    user_dir(base, n, "XDG_CONFIG_HOME", "tompouce/sessions",
             ".config/tompouce/sessions");
}

static void session_path(const char *name, char *out, size_t outlen) {
    char base[1024];
    if (!out || outlen == 0) return;
    out[0] = 0;
    session_base(base, sizeof(base));
    if (!base[0]) return;
    mkdir_p(base);
    int n = snprintf(out, outlen, "%s/%s.json", base, name);
    if (n < 0 || (size_t)n >= outlen) out[0] = 0;
}

static void handle_session(cJSON *args, char *out, size_t outlen, int *iserr) {
    cJSON *a = args ? cJSON_GetObjectItem(args, "action") : NULL;
    const char *act = (a && cJSON_IsString(a)) ? a->valuestring : "";
    if (strcmp(act, "list") == 0) {
        char base[1024];
        session_base(base, sizeof(base));
        DIR *dp = base[0] ? opendir(base) : NULL;
        size_t pos = 0;
        out[0] = 0;
        if (dp) {
            struct dirent *de;
            while ((de = readdir(dp)) != NULL && pos < outlen - 70) {
                size_t L = strlen(de->d_name);
                if (L < 6 || L - 5 >= 72) continue;
                if (strcmp(de->d_name + L - 5, ".json") != 0) continue;
                char nm[72];
                memcpy(nm, de->d_name, L - 5);
                nm[L - 5] = 0;
                if (!session_name_ok(nm)) continue;
                int w = snprintf(out + pos, outlen - pos, "%s\n", nm);
                if (w < 0) break;
                pos += (size_t)w;
            }
            closedir(dp);
        }
        if (pos == 0) snprintf(out, outlen, "%s", "(no sessions)");
        return;
    }
    cJSON *nm = args ? cJSON_GetObjectItem(args, "name") : NULL;
    const char *name = (nm && cJSON_IsString(nm)) ? nm->valuestring : "";
    if (!session_name_ok(name)) {
        snprintf(out, outlen, "%s", "bad name (a-z 0-9 _ -)");
        *iserr = 1;
        return;
    }
    char path[2048];
    session_path(name, path, sizeof(path));
    if (!path[0]) {
        snprintf(out, outlen, "%s", "no session dir");
        *iserr = 1;
        return;
    }
    if (strcmp(act, "save") == 0) {
        int n = songfile_save(path, current_code, audio_get_bpm(), 1);
        if (n > 0) snprintf(out, outlen, "saved %s (%d track%s)", name, n, n == 1 ? "" : "s");
        else { snprintf(out, outlen, "%s", "save failed"); *iserr = 1; }
    } else if (strcmp(act, "load") == 0) {
        char *tmp = NULL;
        double b = 0;
        int n = songfile_load(path, &tmp, &b, 1);
        if (n < 0) { snprintf(out, outlen, "%s", "load failed"); *iserr = 1; return; }
        int apply = (b > 0);
        if (apply && tmp) {
            if (strncmp(tmp, "bpm ", 4) == 0 || strncmp(tmp, "bpm\t", 4) == 0 ||
                strstr(tmp, "\nbpm ") || strstr(tmp, "\nbpm\t")) apply = 0;
        }
        free(tmp);
        /* songfile_load already replaced the list + engine; follow in UI */
        scheduler_song_follow();
        if (apply) audio_set_bpm(b);
        snprintf(out, outlen, "loaded %s (%d track%s)", name, n, n == 1 ? "" : "s");
    } else if (strcmp(act, "delete") == 0) {
        if (unlink(path) == 0) snprintf(out, outlen, "deleted %s", name);
        else { snprintf(out, outlen, "%s", "not found"); *iserr = 1; }
    } else {
        snprintf(out, outlen, "%s", "Unknown action (save/load/list/delete)");
        *iserr = 1;
    }
}

static void handle_health(char *out, size_t outlen) {
    int av = audio_active_voices();
    snprintf(out, outlen,
        "playing=%s stream=%s backend=%s bpm=%.0f t=%.1fs voices=%d/128 queue=%d compiled=%d song=%d/%d banks=%s(%d) midi=%s",
        audio_is_playing() ? "yes" : "no",
        audio_is_stream_active() ? "up" : "down",
        audio_backend_name(),
        audio_get_bpm(), audio_time_now(), av, event_queue_depth(), track_count,
        scheduler_song_pos() + 1, scheduler_song_count(),
        banks_ready() ? "ready" : "indexing", banks_count(),
        midi_ready() ? "up" : "down");
}

static cJSON *text_result(const char *text, int is_error) {
    cJSON *res = cJSON_CreateObject();
    cJSON *content = cJSON_AddArrayToObject(res, "content");
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "type", "text");
    cJSON_AddStringToObject(item, "text", text ? text : "");
    cJSON_AddItemToArray(content, item);
    if (is_error) cJSON_AddBoolToObject(res, "isError", 1);
    return res;
}

static char *sound_catalog_json(const cJSON *args) {
    if (args && !cJSON_IsObject(args)) return NULL;
    const cJSON *category = cJSON_GetObjectItemCaseSensitive(args,"category");
    const cJSON *query = cJSON_GetObjectItemCaseSensitive(args,"query");
    const cJSON *offset = cJSON_GetObjectItemCaseSensitive(args,"offset");
    const cJSON *limit = cJSON_GetObjectItemCaseSensitive(args,"limit");
    if ((category && (!cJSON_IsString(category) || strlen(category->valuestring) > 24)) ||
        (query && (!cJSON_IsString(query) || strlen(query->valuestring) > 240)) ||
        (offset && (!cJSON_IsNumber(offset) || !isfinite(offset->valuedouble) || offset->valuedouble < 0 || offset->valuedouble > 4096 || floor(offset->valuedouble) != offset->valuedouble)) ||
        (limit && (!cJSON_IsNumber(limit) || !isfinite(limit->valuedouble) || limit->valuedouble < 1 || limit->valuedouble > 256 || floor(limit->valuedouble) != limit->valuedouble))) return NULL;
    int cat = -1, start = offset ? offset->valueint : 0, maximum = limit ? limit->valueint : 100;
    if (category && strcmp(category->valuestring,"all")) {
        for (int i = 0; i < SOUND_CATEGORIES; i++) if (!strcmp(category->valuestring,sounds_category(i))) cat = i;
        if (cat < 0) return NULL;
    }
    SoundInfo *rows = calloc(4096,sizeof(*rows));
    cJSON *result = cJSON_CreateObject(), *items = cJSON_CreateArray(), *categories = cJSON_CreateArray();
    if (!rows || !result || !items || !categories) { free(rows); cJSON_Delete(result); cJSON_Delete(items); cJSON_Delete(categories); return NULL; }
    if (!cJSON_AddItemToObject(result,"items",items)) { cJSON_Delete(items); cJSON_Delete(categories); cJSON_Delete(result); free(rows); return NULL; }
    if (!cJSON_AddItemToObject(result,"categories",categories)) { cJSON_Delete(categories); cJSON_Delete(result); free(rows); return NULL; }
    for (int i = 0; i < SOUND_CATEGORIES; i++) {
        cJSON *name = cJSON_CreateString(sounds_category(i));
        if (!name || !cJSON_AddItemToArray(categories,name)) { cJSON_Delete(name); cJSON_Delete(result); free(rows); return NULL; }
    }
    int total = sounds_search(cat,query ? query->valuestring : "",rows,4096);
    if (total < 0 || !cJSON_AddNumberToObject(result,"total",total) || !cJSON_AddNumberToObject(result,"offset",start)) {
        cJSON_Delete(result); free(rows); return NULL;
    }
    for (int i = start; i < total && i < 4096 && i < start+maximum; i++) {
        char code[512];
        if (!sounds_snippet(&rows[i],code,sizeof(code))) { cJSON_Delete(result); free(rows); return NULL; }
        cJSON *item = cJSON_CreateObject();
        if (!item || !cJSON_AddStringToObject(item,"name",rows[i].name) ||
            !cJSON_AddStringToObject(item,"category",sounds_category(rows[i].category)) ||
            !cJSON_AddNumberToObject(item,"slots",rows[i].slots) || !cJSON_AddStringToObject(item,"code",code) ||
            !cJSON_AddItemToArray(items,item)) { cJSON_Delete(item); cJSON_Delete(result); free(rows); return NULL; }
    }
    char *text = total >= 0 ? cJSON_PrintUnformatted(result) : NULL;
    free(rows); cJSON_Delete(result); return text;
}

void handle_request(cJSON *req) {
    cJSON *id = cJSON_GetObjectItem(req, "id");
    cJSON *method = cJSON_GetObjectItem(req, "method");
    if (!method || !cJSON_IsString(method)) return;
    const char *m = method->valuestring;
    cJSON *params = cJSON_GetObjectItem(req, "params");

    /* notifications: no response */
    if (strncmp(m, "notifications/", 14) == 0) return;

    if (strcmp(m, "initialize") == 0) {
        cJSON *res = cJSON_CreateObject();
        cJSON_AddStringToObject(res, "protocolVersion", "2024-11-05");
        cJSON *caps = cJSON_AddObjectToObject(res, "capabilities");
        cJSON_AddObjectToObject(caps, "tools");
        cJSON_AddObjectToObject(caps, "resources");
        cJSON *info = cJSON_AddObjectToObject(res, "serverInfo");
        cJSON_AddStringToObject(info, "name", "tompouce");
        cJSON_AddStringToObject(info, "version", "0.1.0");
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
        if (id) cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id,1));
        cJSON_AddItemToObject(resp, "result", res);
        send_json(resp);
        return;
    }
    if (strcmp(m, "ping") == 0) {
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
        if (id) cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id,1));
        cJSON_AddObjectToObject(resp, "result");
        send_json(resp);
        return;
    }
    if (strcmp(m, "tools/list") == 0) {
        cJSON *tools = cJSON_CreateArray();
#define ADDTOOL(name, desc, schema_json) do { \
        cJSON *t = cJSON_CreateObject(); \
        cJSON_AddStringToObject(t, "name", name); \
        cJSON_AddStringToObject(t, "description", desc); \
        cJSON *sc = cJSON_Parse(schema_json); \
        cJSON_AddItemToObject(t, "inputSchema", sc); \
        cJSON_AddItemToArray(tools, t); \
    } while(0)
        ADDTOOL("music_set_code", "Replace and evaluate Tompouce code.",
            "{\"type\":\"object\",\"properties\":{\"code\":{\"type\":\"string\"},\"autostart\":{\"type\":\"boolean\",\"default\":false}},\"required\":[\"code\"]}");
        ADDTOOL("music_evaluate", "Evaluate current code buffer; optional autostart starts playback only on success.", "{\"type\":\"object\",\"properties\":{\"autostart\":{\"type\":\"boolean\",\"default\":false}}}");
        ADDTOOL("music_play", "Start playback.", "{\"type\":\"object\",\"properties\":{}}");
        ADDTOOL("music_stop", "Stop playback.", "{\"type\":\"object\",\"properties\":{}}");
        ADDTOOL("music_set_bpm", "Set tempo BPM.", "{\"type\":\"object\",\"properties\":{\"bpm\":{\"type\":\"number\"}},\"required\":[\"bpm\"]}");
        ADDTOOL("music_get_status", "Get playback status, bpm, track count, console tail.", "{\"type\":\"object\",\"properties\":{}}");
        ADDTOOL("music_get_code", "Get current code.", "{\"type\":\"object\",\"properties\":{}}");
        ADDTOOL("music_get_console", "Get recent console log lines.", "{\"type\":\"object\",\"properties\":{\"lines\":{\"type\":\"number\"}}}");
        ADDTOOL("music_ai", "Ask AI to generate code from a prompt; result is loaded and evaluated.",
            "{\"type\":\"object\",\"properties\":{\"prompt\":{\"type\":\"string\"}},\"required\":[\"prompt\"]}");
        ADDTOOL("music_song", "Song mode: play track buffers in sequence, one per cycle.",
            "{\"type\":\"object\",\"properties\":{\"enabled\":{\"type\":\"boolean\"}}}");
        ADDTOOL("music_track", "Song tracks: copy/next/prev/hold/list/goto/move/remove.",
            "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\"},\"index\":{\"type\":\"number\"},\"to\":{\"type\":\"number\"}},\"required\":[\"action\"]}");
        ADDTOOL("music_session", "Composition sessions (save/load/list/delete).",
            "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\"},\"name\":{\"type\":\"string\"}},\"required\":[\"action\"]}");
        ADDTOOL("music_health", "Engine diagnostics (audio, voices, queue, index).",
            "{\"type\":\"object\",\"properties\":{}}");
        ADDTOOL("music_debug", "Read-only engine snapshot: voices, filter Hz, sample window, recent triggers, queued events.",
            "{\"type\":\"object\",\"properties\":{\"limit\":{\"type\":\"number\"}}}");
        ADDTOOL("music_sounds", "Search playable samples, drum-machines, synths, wavetables and user sounds. Returns JSON catalog text.",
            "{\"type\":\"object\",\"properties\":{\"category\":{\"type\":\"string\"},\"query\":{\"type\":\"string\"},\"offset\":{\"type\":\"integer\"},\"limit\":{\"type\":\"integer\"}}}");
        ADDTOOL("music_reload_sounds", "Reload imported sounds from confined application storage; source/playback unchanged.",
            "{\"type\":\"object\",\"properties\":{}}");
#undef ADDTOOL
        cJSON *res = cJSON_CreateObject();
        cJSON_AddItemToObject(res, "tools", tools);
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
        if (id) cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id,1));
        cJSON_AddItemToObject(resp, "result", res);
        send_json(resp);
        return;
    }
    if (strcmp(m, "tools/call") == 0) {
        cJSON *name = cJSON_GetObjectItem(params, "name");
        cJSON *args = cJSON_GetObjectItem(params, "arguments");
        if (!name || !cJSON_IsString(name)) {
            cJSON *err = cJSON_CreateObject();
            cJSON_AddNumberToObject(err, "code", -32602);
            cJSON_AddStringToObject(err, "message", "Missing tool name");
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
            if (id) cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id,1));
            cJSON_AddItemToObject(resp, "error", err);
            send_json(resp);
            return;
        }
        const char *tname = name->valuestring;
        size_t outcap = 65536;
        if (current_code) {
            size_t ncode = strlen(current_code);
            if (ncode < (size_t)-1 - 4096 && ncode + 4096 > outcap)
                outcap = ncode + 4096;
        }
        char *out = calloc(outcap, 1);
        if (!out) return;
        int iserr = 0;
        if (strcmp(tname,"music_sounds") == 0) {
            char *catalog = sound_catalog_json(args);
            if (!catalog) { snprintf(out,outcap,"Invalid sound query or catalog allocation failed."); iserr = 1; }
            else { free(out); out = catalog; outcap = strlen(out)+1; }
        } else if (strcmp(tname,"music_reload_sounds") == 0) {
            int names = user_sounds_reload();
            snprintf(out,outcap,"Imported sound catalog reloaded: %d names.",names);
            iserr = names < 0;
        } else if (strcmp(tname, "music_set_code") == 0) {
            cJSON *code = args ? cJSON_GetObjectItem(args, "code") : NULL;
            const cJSON *autostart=args?cJSON_GetObjectItemCaseSensitive(args,"autostart"):NULL;
            if (autostart && !cJSON_IsBool(autostart)) { snprintf(out,outcap,"autostart must be boolean."); iserr=1; }
            else if (code && cJSON_IsString(code)) {
                if (track_load_from_code(code->valuestring)) {
                    if (cJSON_IsTrue(autostart)) { scheduler_transport_set(1); scheduler_start(); }
                    snprintf(out, outcap, "Loaded %d tracks.", track_count);
                }
                else { snprintf(out,outcap,"Evaluation failed; last playable pattern retained. See music_get_console."); iserr = 1; }
            } else { snprintf(out, outcap, "Missing code"); iserr = 1; }
        } else if (strcmp(tname, "music_evaluate") == 0) {
            const cJSON *autostart=args?cJSON_GetObjectItemCaseSensitive(args,"autostart"):NULL;
            if (autostart && !cJSON_IsBool(autostart)) { snprintf(out,outcap,"autostart must be boolean."); iserr=1; }
            else if (track_load_from_code(current_code)) {
                if (cJSON_IsTrue(autostart)) { scheduler_transport_set(1); scheduler_start(); }
                snprintf(out, outcap, "Evaluated %d tracks.", track_count);
            }
            else { snprintf(out,outcap,"Evaluation failed; last playable pattern retained. See music_get_console."); iserr = 1; }
        } else if (strcmp(tname, "music_play") == 0) {
            scheduler_transport_set(1); scheduler_start();
            snprintf(out, outcap, "Playing.");
        } else if (strcmp(tname, "music_stop") == 0) {
            scheduler_transport_set(0);
            snprintf(out, outcap, "Stopped.");
        } else if (strcmp(tname, "music_set_bpm") == 0) {
            cJSON *b = args ? cJSON_GetObjectItem(args, "bpm") : NULL;
            if (b && cJSON_IsNumber(b) && isfinite(b->valuedouble) && b->valuedouble>=0 &&
                (b->valuedouble==0 || isfinite(240/b->valuedouble))) {
                audio_set_bpm(b->valuedouble);
                snprintf(out, outcap, "BPM set to %.0f.", audio_get_bpm());
            } else { snprintf(out, outcap, "Missing or invalid bpm"); iserr=1; }
        } else if (strcmp(tname, "music_song") == 0) {
            cJSON *e = args ? cJSON_GetObjectItem(args, "enabled") : NULL;
            if (e && (cJSON_IsTrue(e) || cJSON_IsFalse(e))) {
                scheduler_song_set(cJSON_IsTrue(e) ? 1 : 0);
                scheduler_song_follow();
            }
            else if (!e) { snprintf(out, outcap, "Missing enabled"); iserr=1; }
            if (!iserr) {
                int si = scheduler_song_pos();
                int n = scheduler_song_count();
                if (scheduler_song_on() && si >= 0)
                    snprintf(out, outcap, "song on: track %d/%d", si + 1, n);
                else snprintf(out, outcap, "song off (%d tracks)", n);
            }
        } else if (strcmp(tname, "music_track") == 0) {
            cJSON *a = args ? cJSON_GetObjectItem(args, "action") : NULL;
            const char *act = (a && cJSON_IsString(a)) ? a->valuestring : "";
            if (strcmp(act, "copy") == 0) {
                int idx = scheduler_track_copy(current_code);
                scheduler_song_follow();
                if (idx >= 0) snprintf(out, outcap, "copied to track %d/%d", idx + 1, scheduler_song_count());
                else { snprintf(out, outcap, "copy failed"); iserr=1; }
            } else if (strcmp(act, "next") == 0) {
                scheduler_track_next();
                scheduler_song_follow();
                snprintf(out, outcap, "track %d/%d", scheduler_song_pos() + 1, scheduler_song_count());
            } else if (strcmp(act, "prev") == 0) {
                scheduler_track_prev();
                scheduler_song_follow();
                snprintf(out, outcap, "track %d/%d", scheduler_song_pos() + 1, scheduler_song_count());
            } else if (strcmp(act, "hold") == 0) {
                scheduler_song_hold_toggle();
                snprintf(out, outcap, scheduler_song_hold() ? "hold on" : "hold off");
            } else if (strcmp(act, "goto") == 0) {
                cJSON *ix = args ? cJSON_GetObjectItem(args, "index") : NULL;
                int n = scheduler_song_count();
                if (ix && cJSON_IsNumber(ix) && isfinite(ix->valuedouble) &&
                    (int)ix->valuedouble >= 0 && (int)ix->valuedouble < n) {
                    scheduler_track_goto((int)ix->valuedouble);
                    scheduler_song_follow();
                    snprintf(out, outcap, "track %d/%d", scheduler_song_pos() + 1, n);
                } else { snprintf(out, outcap, "bad index"); iserr=1; }
            } else if (strcmp(act, "list") == 0) {
                int n = scheduler_song_count();
                size_t pos = 0;
                out[0] = 0;
                for (int i = 0; i < n; i++) {
                    char prev[128];
                    scheduler_track_preview(i, prev, sizeof(prev));
                    int w = snprintf(out + pos, outcap - pos, "%c%d: %.100s\n",
                        i == scheduler_song_pos() ? '*' : ' ', i, prev);
                    if (w < 0 || pos + (size_t)w >= outcap) break;
                    pos += (size_t)w;
                }
                if (n == 0) snprintf(out, outcap, "(no tracks)");
            } else if (strcmp(act, "move") == 0) {
                cJSON *ix = args ? cJSON_GetObjectItem(args, "index") : NULL;
                cJSON *to = args ? cJSON_GetObjectItem(args, "to") : NULL;
                if (ix && cJSON_IsNumber(ix) && to && cJSON_IsNumber(to) &&
                    isfinite(ix->valuedouble) && isfinite(to->valuedouble) &&
                    scheduler_track_move((int)ix->valuedouble, (int)to->valuedouble)) {
                    scheduler_song_follow();
                    snprintf(out, outcap, "moved, now track %d/%d",
                        scheduler_song_pos() + 1, scheduler_song_count());
                } else { snprintf(out, outcap, "bad index"); iserr=1; }
            } else if (strcmp(act, "remove") == 0) {
                cJSON *ix = args ? cJSON_GetObjectItem(args, "index") : NULL;
                if (ix && cJSON_IsNumber(ix) && isfinite(ix->valuedouble) &&
                    scheduler_track_remove((int)ix->valuedouble)) {
                    scheduler_song_follow();
                    snprintf(out, outcap, "removed, %d left", scheduler_song_count());
                } else { snprintf(out, outcap, "bad index"); iserr=1; }
            } else { snprintf(out, outcap, "Unknown action (copy/next/prev/hold/list/goto/move/remove)"); iserr=1; }
        } else if (strcmp(tname, "music_session") == 0) {
            handle_session(args, out, outcap, &iserr);
        } else if (strcmp(tname, "music_health") == 0) {
            handle_health(out, outcap);
        } else if (strcmp(tname, "music_debug") == 0) {
            cJSON *lim = args ? cJSON_GetObjectItem(args, "limit") : NULL;
            int n = 16;
            size_t pos = 0;
            if (lim && cJSON_IsNumber(lim) && isfinite(lim->valuedouble))
                n = (int)lim->valuedouble;
            if (n < 1) n = 1;
            if (n > 48) n = 48;
            {
                int w = audio_debug_snapshot(out, outcap, n);
                if (w < 0) {
                    snprintf(out, outcap, "debug failed");
                    iserr = 1;
                } else {
                    pos = (size_t)w;
                }
            }
            if (!iserr && pos + 1 < outcap) {
                int w = scheduler_debug_queue(out + pos, outcap - pos, n);
                if (w < 0) {
                    snprintf(out, outcap, "debug failed");
                    iserr = 1;
                }
            }
        } else if (strcmp(tname, "music_get_status") == 0) {
            int si = scheduler_song_pos();
            int n = scheduler_song_count();
            char song[64] = {0};
            if (scheduler_song_on() && si >= 0)
                snprintf(song, sizeof(song), " song=%d/%d%s", si + 1, n,
                    scheduler_song_hold() ? ":HOLD" : "");
            else if (n > 0)
                snprintf(song, sizeof(song), " tracks=%d", n);
            snprintf(out, outcap, "playing=%s bpm=%.0f voices=%d%s\ncurrent_code_bytes=%zu",
                audio_is_playing()?"yes":"no", audio_get_bpm(), track_count, song,
                current_code ? strlen(current_code) : 0);
        } else if (strcmp(tname, "music_get_code") == 0) {
            size_t ncode = current_code ? strlen(current_code) : 0;
            if (ncode + 1 > outcap) ncode = outcap ? outcap - 1 : 0;
            if (outcap > 0) {
                if (ncode) memcpy(out, current_code, ncode);
                out[ncode] = 0;
            }
        } else if (strcmp(tname, "music_get_console") == 0) {
            cJSON *ln = args ? cJSON_GetObjectItem(args, "lines") : NULL;
            int n = (ln && cJSON_IsNumber(ln)) ? (int)ln->valuedouble : 20;
            if (n < 1) n = 1;
            if (n > 100) n = 100;
            int cnt = console_count_get();
            int start = (cnt <= n) ? 0 : (cnt - n);
            size_t pos = 0;
            for (int i = start; i < cnt; i++) {
                const char *l = console_line(i);
                if (l) {
                    size_t ll = strlen(l);
                    if (pos + ll + 2 < outcap) {
                        memcpy(out+pos, l, ll); pos += ll; out[pos++] = '\n'; out[pos]=0;
                    }
                }
            }
            if (pos == 0) snprintf(out, outcap, "%s", "(no console output)");
        } else if (strcmp(tname, "music_ai") == 0) {
            cJSON *pr = args ? cJSON_GetObjectItem(args, "prompt") : NULL;
            if (pr && cJSON_IsString(pr)) {
                ai_request_async(pr->valuestring, current_code);
                char *r = NULL;
                int waited = 0;
                while (waited < 120000) {
                    r = ai_poll_result();
                    if (r) break;
                    usleep(200000);
                    waited += 200;
                }
                if (r) {
                    track_load_from_code(r);
                    size_t need = strlen(r) + 64;
                    if (need > outcap) {
                        char *bigger = realloc(out, need);
                        if (bigger) { out = bigger; outcap = need; }
                    }
                    snprintf(out, outcap, "AI generated and loaded code:\n%s", r);
                    free(r);
                } else {
                    snprintf(out, outcap, "AI timed out.");
                    iserr = 1;
                }
            } else { snprintf(out, outcap, "Missing prompt"); iserr=1; }
        } else {
            snprintf(out, outcap, "Unknown tool: %s", tname);
            iserr = 1;
        }
        cJSON *result = text_result(out, iserr);
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
        if (id) cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id,1));
        cJSON_AddItemToObject(resp, "result", result);
        send_json(resp);
        free(out);
        return;
    }
    if (strcmp(m, "resources/list") == 0) {
        cJSON *resources = cJSON_CreateArray();
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "uri", "strudel://code");
        cJSON_AddStringToObject(r, "name", "current_code");
        cJSON_AddItemToArray(resources, r);
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "uri", "strudel://tracks");
        cJSON_AddStringToObject(t, "name", "song_tracks");
        cJSON_AddItemToArray(resources, t);
        cJSON *res = cJSON_CreateObject();
        cJSON_AddItemToObject(res, "resources", resources);
        cJSON *resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
        if (id) cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id,1));
        cJSON_AddItemToObject(resp, "result", res);
        send_json(resp);
        return;
    }
    if (strcmp(m, "resources/read") == 0) {
        cJSON *uri = params ? cJSON_GetObjectItem(params, "uri") : NULL;
        const char *u = (uri && cJSON_IsString(uri)) ? uri->valuestring : "";
        if (strcmp(u, "strudel://code") == 0) {
            cJSON *contents = cJSON_CreateArray();
            cJSON *c = cJSON_CreateObject();
            cJSON_AddStringToObject(c, "uri", u);
            cJSON_AddStringToObject(c, "mimeType", "text/plain");
            cJSON_AddStringToObject(c, "text", current_code);
            cJSON_AddItemToArray(contents, c);
            cJSON *res = cJSON_CreateObject();
            cJSON_AddItemToObject(res, "contents", contents);
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
            if (id) cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id,1));
            cJSON_AddItemToObject(resp, "result", res);
            send_json(resp);
            return;
        }
        if (strcmp(u, "strudel://tracks") == 0) {
            cJSON *arr = cJSON_CreateArray();
            int n = scheduler_song_count();
            int pos = scheduler_song_pos();
            for (int i = 0; i < n; i++) {
                char prev[128];
                scheduler_track_preview(i, prev, sizeof(prev));
                cJSON *t = cJSON_CreateObject();
                cJSON_AddNumberToObject(t, "index", i);
                cJSON_AddBoolToObject(t, "active", i == pos ? 1 : 0);
                cJSON_AddStringToObject(t, "preview", prev);
                cJSON_AddItemToArray(arr, t);
            }
            cJSON *c = cJSON_CreateObject();
            cJSON_AddStringToObject(c, "uri", u);
            cJSON_AddStringToObject(c, "mimeType", "application/json");
            char *js = cJSON_PrintUnformatted(arr);
            cJSON_Delete(arr);
            cJSON_AddStringToObject(c, "text", js ? js : "[]");
            if (js) free(js);
            cJSON *contents = cJSON_CreateArray();
            cJSON_AddItemToArray(contents, c);
            cJSON *res = cJSON_CreateObject();
            cJSON_AddItemToObject(res, "contents", contents);
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
            if (id) cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id,1));
            cJSON_AddItemToObject(resp, "result", res);
            send_json(resp);
            return;
        }
    }

    /* method not found */
    cJSON *err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", -32601);
    cJSON_AddStringToObject(err, "message", "Method not found");
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    if (id) cJSON_AddItemToObject(resp, "id", cJSON_Duplicate(id,1));
    cJSON_AddItemToObject(resp, "error", err);
    send_json(resp);
}

static void mcp_reset_pending(void) {
    mcp_pending_len = 0;
    mcp_frame_drop = 0;
}

static void mcp_drop_client(void) {
    mcp_output_socket=-1; mcp_input_tcp=0; mcp_send_failed=0; mcp_client_deadline=0;
    if (mcp_out_stream && mcp_out_stream != stdout) {
        fclose(mcp_out_stream);
        mcp_out_stream = NULL;
    }
    if (mcp_input_fd >= 0) {
        close(mcp_input_fd);
        mcp_input_fd = -1;
    }
    mcp_reset_pending();
}

/* Loopback only. One accepted client at a time; the UI thread polls it. */
static int mcp_open_listen(int port) {
    if (!mcp_auth_init(1)) { log_console("mcp: TCP requires a valid 64-character lowercase hex TOMPOUCE_MCP_TOKEN or secure randomness"); return -1; }
    int ls = socket(AF_INET, SOCK_STREAM|SOCK_CLOEXEC, 0);
    if (ls < 0) return -1;
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(ls, 1) != 0) {
        close(ls);
        return -1;
    }
    socklen_t address_size = sizeof(addr);
    if (getsockname(ls,(struct sockaddr *)&addr,&address_size) != 0) { close(ls); return -1; }
    port = ntohs(addr.sin_port);
    int flags = fcntl(ls, F_GETFL, 0);
    if (flags >= 0) fcntl(ls, F_SETFL, flags | O_NONBLOCK);
    mcp_listen_fd = ls;
    mcp_port_bound = port;
    log_console("mcp on 127.0.0.1:%d", port);
    return 0;
}

int mcp_companion_port(void) {
    if (mcp_listen_fd >= 0) return mcp_port_bound;
    if (mcp_input_fd >= 0) {
        log_console("companion needs a terminal TCP session; launch ./tompouce --mcp --ui in a terminal");
        return 0;
    }
    if (mcp_open_listen(0) != 0) { log_console("companion: could not enable loopback MCP"); return 0; }
    return mcp_port_bound;
}

void mcp_status(char *out, size_t n) {
    if (!out || n == 0) return;
    if (mcp_port_bound > 0)
        snprintf(out, n, "127.0.0.1:%d%s", mcp_port_bound, mcp_input_fd >= 0 ? "*" : "");
    else if (mcp_input_fd >= 0)
        snprintf(out, n, "pipe");
    else
        snprintf(out, n, "off");
}

void mcp_wait_input(int terminal_fd, int timeout_ms) {
    fd_set ready;
    FD_ZERO(&ready);
    int highest = -1;
    int descriptors[] = {terminal_fd,mcp_input_fd,mcp_input_fd < 0 ? mcp_listen_fd : -1};
    for (size_t i = 0; i < sizeof(descriptors)/sizeof(descriptors[0]); i++) {
        int fd = descriptors[i];
        if (fd < 0 || fd >= FD_SETSIZE) continue;
        FD_SET(fd,&ready);
        if (fd > highest) highest = fd;
    }
    if (timeout_ms < 0) timeout_ms = 0;
    if (timeout_ms > 50) timeout_ms = 50;
    struct timeval timeout = {0,timeout_ms*1000};
    (void)select(highest+1,&ready,NULL,NULL,&timeout);
}

void mcp_poll(void) {
    if (mcp_listen_fd >= 0 && mcp_input_fd < 0) {
        fd_set lfds;
        FD_ZERO(&lfds);
        FD_SET(mcp_listen_fd, &lfds);
        struct timeval tv0 = {0, 0};
        if (select(mcp_listen_fd + 1, &lfds, NULL, NULL, &tv0) > 0) {
            int cs = accept(mcp_listen_fd, NULL, NULL);
            if (cs >= 0) {
                int flags = fcntl(cs, F_GETFL, 0);
                if (flags<0 || fcntl(cs,F_SETFL,flags|O_NONBLOCK)<0 || fcntl(cs,F_SETFD,FD_CLOEXEC)<0) {
                    close(cs);
                } else {
                    if (mcp_out_stream && mcp_out_stream != stdout) fclose(mcp_out_stream);
                    mcp_out_stream = NULL;
                    mcp_input_fd = cs;
                    mcp_input_tcp=1; mcp_output_socket=cs; mcp_send_failed=0;
                    mcp_client_deadline=now_seconds()+2;
                    mcp_reset_pending();
                    /* Short-lived polling clients are normal (companion UI).
                       Connection state is visible through mcp_status(). */
                }
            }
        }
    }
    if (mcp_input_fd < 0) return;
    if (mcp_input_tcp && now_seconds()>mcp_client_deadline) { mcp_drop_client(); return; }
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(mcp_input_fd, &rfds);
    struct timeval tv = {0, 0};
    int sel = select(mcp_input_fd + 1, &rfds, NULL, NULL, &tv);
    if (sel < 0) {
        if (errno == EINTR) return;
        mcp_drop_client();
        return;
    }
    if (sel == 0) return;
    for (;;) {
        char *frame = NULL;
        int rr = mcp_read_frame(mcp_input_fd, &frame);
        if (rr < 0) {
            free(frame);
            mcp_drop_client();
            /* Frequent short-lived telemetry clients are normal. */
            return;
        }
        if (rr == 0 || !frame) {
            free(frame);
            return;
        }
        if (mcp_input_tcp) {
            if (!mcp_dispatch_tcp(frame)) { free(frame); mcp_drop_client(); return; }
        } else {
            cJSON *req = cJSON_Parse(frame);
            if (req) { handle_request(req); cJSON_Delete(req); }
        }
        free(frame);
    }
}

void mcp_run(int ui_mode, int tcp_port) {
    int ui = ui_mode;
    /* start audio */
    audio_init();
    ai_init();
    scheduler_init();
    audio_toggle_play(1);
    if (mcp_init_code && mcp_init_code[0] && !ui) track_load_from_code(mcp_init_code);

    if (ui) {
        if (!isatty(0)) {
            mcp_input_fd = dup(0);
            int f = fcntl(mcp_input_fd, F_GETFL, 0);
            if (f >= 0) fcntl(mcp_input_fd, F_SETFL, f | O_NONBLOCK);
        } else {
            mcp_input_fd = -1;
            int p = tcp_port > 0 ? tcp_port : MCP_DEFAULT_PORT;
            if (mcp_open_listen(p) != 0)
                log_console("mcp: listen 127.0.0.1:%d failed", p);
        }
        int saved_out = dup(1);
        mcp_out_stream = fdopen(saved_out, "w");
        /* reopen stdin/stdout to terminal for UI */
        freopen("/dev/tty", "r", stdin);
        freopen("/dev/tty", "w", stdout);
        tui_init();
        if (mcp_init_code && mcp_init_code[0]) tui_load_initial(mcp_init_code);
        tui_run();
        tui_shutdown();
        mcp_drop_client();
        if (mcp_listen_fd >= 0) {
            close(mcp_listen_fd);
            mcp_listen_fd = -1;
        }
        return;
    }

    /* headless */
    mcp_out_stream = stdout;
    mcp_input_fd = -1;
    int stdin_fd = fileno(stdin);
    int flags = fcntl(stdin_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(stdin_fd, F_SETFL, flags | O_NONBLOCK);
    int running = 1;
    while (running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(stdin_fd, &rfds);
        struct timeval tv = { 0, 100000 };
        int sel = select(stdin_fd + 1, &rfds, NULL, NULL, &tv);
        if (sel > 0) {
            for (;;) {
                char *frame = NULL;
                int rr = mcp_read_frame(stdin_fd, &frame);
                if (rr < 0) { running = 0; break; }
                if (rr == 0) break;
                cJSON *req = cJSON_Parse(frame);
                if (req) {
                    handle_request(req);
                    cJSON_Delete(req);
                }
                free(frame);
            }
            if (!running) break;
        } else if (sel < 0) {
            if (errno == EINTR) continue;
            running = 0;
            break;
        }
    }
    scheduler_shutdown();
    audio_shutdown();
    ai_shutdown();
}

int mcp_run_tcp(int port) {
    if (!mcp_auth_init(0)) {
        log_console("mcp: --mcp-port requires TOMPOUCE_MCP_TOKEN (64 lowercase hex characters)");
        return 1;
    }
    audio_init();
    ai_init();
    scheduler_init();
    audio_toggle_play(1);
    if (mcp_init_code && mcp_init_code[0]) track_load_from_code(mcp_init_code);

    int ls = socket(AF_INET, SOCK_STREAM|SOCK_CLOEXEC, 0);
    if (ls < 0) { log_console("tcp: socket failed"); scheduler_shutdown(); audio_shutdown(); ai_shutdown(); return 1; }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* local only */
    addr.sin_port = htons((uint16_t)port);
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(ls, 4) != 0) {
        log_console("tcp: bind/listen %d failed", port);
        close(ls);
        scheduler_shutdown(); audio_shutdown(); ai_shutdown(); return 1;
    }
    log_console("mcp tcp on 127.0.0.1:%d", port);
    for (;;) {
        int cs = accept(ls, NULL, NULL);
        if (cs < 0) {
            if (errno == EINTR) continue;
            break;
        }
        int flags = fcntl(cs, F_GETFL, 0);
        if (flags<0 || fcntl(cs,F_SETFL,flags|O_NONBLOCK)<0 || fcntl(cs,F_SETFD,FD_CLOEXEC)<0) { close(cs); continue; }
        mcp_output_socket=cs; mcp_send_failed=0; mcp_reset_pending();
        mcp_client_deadline=now_seconds()+2;
        int alive = 1;
        while (alive) {
            if (now_seconds()>mcp_client_deadline) break;
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(cs, &rfds);
            struct timeval tv = { 0, 100000 };
            int sel = select(cs + 1, &rfds, NULL, NULL, &tv);
            if (sel > 0) {
                for (;;) {
                    char *frame = NULL;
                    int rr = mcp_read_frame(cs, &frame);
                    if (rr < 0) { alive = 0; break; }
                    if (rr == 0) break;
                    int accepted=mcp_dispatch_tcp(frame);
                    free(frame);
                    if (!accepted) { alive=0; break; }
                }
            } else if (sel < 0) {
                if (errno == EINTR) continue;
                alive = 0;
            }
        }
        mcp_output_socket=-1; mcp_send_failed=0; mcp_reset_pending();
        mcp_out_stream = stdout;
        close(cs);
    }
    close(ls);
    scheduler_shutdown();
    audio_shutdown();
    ai_shutdown();
    return 0;
}
