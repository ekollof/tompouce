#include "ai.h"
#include "common.h"
#include <curl/curl.h>
#include <cjson/cJSON.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t ai_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t ai_tid;
static int ai_busy = 0;
static char *ai_result = NULL;
static char ai_prompt_copy[2048];
static char *ai_context_copy;
static int curl_ok = 0;

#define AI_MAX_BODY (256u * 1024u)
static struct Memory { char *data; size_t size; } mem;

static size_t write_cb(void *contents, size_t size, size_t nmemb, void *userp) {
    if (nmemb != 0 && size > (size_t)-1 / nmemb) return 0;
    size_t realsize = size * nmemb;
    struct Memory *m = (struct Memory *)userp;
    if (m->size > AI_MAX_BODY || realsize > AI_MAX_BODY - m->size) return 0;
    char *ptr = realloc(m->data, m->size + realsize + 1);
    if (!ptr) return 0;
    m->data = ptr;
    memcpy(m->data + m->size, contents, realsize);
    m->size += realsize;
    m->data[m->size] = 0;
    return realsize;
}

/* http(s) only, no spaces or control bytes (blocks file:// and header injection) */
static int http_base_ok(const char *s) {
    if (!s || !s[0] || strlen(s) > 400) return 0;
    if (strncmp(s, "https://", 8) != 0 && strncmp(s, "http://", 7) != 0) return 0;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c <= 0x20 || c == 0x7f) return 0;
    }
    return 1;
}

static int header_token_ok(const char *s) {
    if (!s || !s[0] || strlen(s) > 480) return 0;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c <= 0x20 || c == 0x7f) return 0;
    }
    return 1;
}

static int model_ok(const char *s) {
    if (!s || !s[0] || strlen(s) > 128) return 0;
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f) return 0;
    }
    return 1;
}

static char *extract_code_block(const char *text) {
    if (!text) return strdup_safe("");
    const char *start = strstr(text, "```");
    if (start) {
        start += 3;
        // skip language tag
        const char *nl = strchr(start, '\n');
        if (nl) start = nl + 1;
        const char *end = strstr(start, "```");
        size_t len = end ? (size_t)(end - start) : strlen(start);
        char *out = malloc(len + 1);
        if (!out) return strdup_safe("");
        memcpy(out, start, len);
        out[len] = 0;
        return out;
    }
    // no fence: return whole text
    return strdup_safe(text);
}

static void offline_generate(const char *prompt, char **out) {
    // Simple templates
    char *res = malloc(1024);
    if (!res) { *out = NULL; return; }
    res[0] = 0;
    const char *p = prompt ? prompt : "";
    if (strstr(p,"techno") || strstr(p,"dance")) {
        snprintf(res, 1024, "setcpm(138/4)\n$: stack(s(\"bd ~ bd ~\"),s(\"~ sd ~ sd\"),s(\"hh*8\"),note(\"c2 ~ e2 g2\").s(\"sawtooth\").fast(2),note(\"c4 e4 g4 c5\").s(\"square\").rev())");
    } else if (strstr(p,"ambient") || strstr(p,"chill")) {
        snprintf(res, 1024, "setcpm(72/4)\n$: stack(note(\"c4 e4 g4 b4\").s(\"triangle\"),note(\"e4 g4 b4 d5\").s(\"triangle\").slow(2),note(\"c2 ~ ~ ~\").s(\"sawtooth\").slow(2))");
    } else if (strstr(p,"happy") || strstr(p,"major")) {
        snprintf(res, 1024, "setcpm(120/4)\n$: stack(note(\"c4 d4 e4 f4 g4 a4 b4 c5\").s(\"triangle\"),note(\"c3 g2 a2 e3\").s(\"sawtooth\"))");
    } else if (strstr(p,"sad") || strstr(p,"minor")) {
        snprintf(res, 1024, "setcpm(80/4)\n$: stack(note(\"c4 eb4 g4 bb4\").s(\"triangle\"),note(\"c2 eb2 g2 bb2\").s(\"sawtooth\").slow(2))");
    } else if (strstr(p,"funk") || strstr(p,"groove")) {
        snprintf(res, 1024, "setcpm(105/4)\n$: stack(note(\"c4 ~ e4 g4 ~ bb4 c5 ~\").s(\"triangle\"),note(\"c2 ~ ~ eb2 ~ g2 ~ ~\").s(\"sawtooth\").gain(.9),s(\"bd ~ sd ~\"))");
    } else {
        snprintf(res, 1024, "setcpm(120/4)\n$: stack(s(\"bd sd bd sd\"),s(\"hh*4\"),note(\"c2 e2 g2 c3\").s(\"sawtooth\"))");
    }
    *out = res;
}

static void *ai_worker(void *arg) {
    (void)arg;
    char *result_text = NULL;
    const char *apikey = getenv("OPENAI_API_KEY");
    if (!apikey) apikey = getenv("AI_API_KEY");
    const char *base = getenv("OPENAI_BASE_URL");
    if (!base) base = getenv("AI_BASE_URL");
    if (!base || !base[0]) base = "https://api.openai.com/v1";
    const char *model = getenv("OPENAI_MODEL");
    if (!model) model = getenv("AI_MODEL");
    if (!model || !model[0]) model = "gpt-4o-mini";

    if (!header_token_ok(apikey) || !http_base_ok(base) || !model_ok(model) || !curl_ok) {
        offline_generate(ai_prompt_copy, &result_text);
    } else {
        CURL *curl = curl_easy_init();
        if (curl) {
            char url[512];
            snprintf(url, 512, "%s/chat/completions", base);
            // build JSON body
            cJSON *root = cJSON_CreateObject();
            cJSON_AddStringToObject(root, "model", model);
            cJSON *msgs = cJSON_AddArrayToObject(root, "messages");
            cJSON *sys = cJSON_CreateObject();
            cJSON_AddStringToObject(sys, "role", "system");
            const char *sysprompt =
                "You generate Tompouce code. Output ONLY a fenced code block. "
                "Use JavaScript Strudel with note(), s(), stack(), and method chains. "
                "Set tempo using setcpm(BPM/4). Put mini-notation inside quoted strings. "
                "Use ~ for rests, .s('sawtooth') for synths, .gain(slider(.5)) for live controls, "
                ".fast(N), .slow(N), .rev(), .lpf(Hz), .scale('C:minor') and .add(N). "
                "For multiple patterns return stack(...), or use $: statements. "
                "No extra explanation.";
            cJSON_AddStringToObject(sys, "content", sysprompt);
            cJSON_AddItemToArray(msgs, sys);
            if (ai_context_copy && ai_context_copy[0]) {
                cJSON *ctx = cJSON_CreateObject();
                cJSON_AddStringToObject(ctx, "role", "user");
                // shorten context maybe
                char ctxbuf[4096]; snprintf(ctxbuf, sizeof(ctxbuf), "Current code:\n%.4000s", ai_context_copy);
                cJSON_AddStringToObject(ctx, "content", ctxbuf);
                cJSON_AddItemToArray(msgs, ctx);
            }
            cJSON *usr = cJSON_CreateObject();
            cJSON_AddStringToObject(usr, "role", "user");
            cJSON_AddStringToObject(usr, "content", ai_prompt_copy);
            cJSON_AddItemToArray(msgs, usr);

            char *body = cJSON_PrintUnformatted(root);
            cJSON_Delete(root);

            mem.data = NULL; mem.size = 0;
            struct curl_slist *headers = NULL;
            char auth[512]; snprintf(auth,512,"Authorization: Bearer %s", apikey);
            headers = curl_slist_append(headers, auth);
            headers = curl_slist_append(headers, "Content-Type: application/json");
            curl_easy_setopt(curl, CURLOPT_URL, url);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mem);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
            curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
            curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
            CURLcode rc = curl_easy_perform(curl);
            curl_easy_cleanup(curl);
            curl_slist_free_all(headers);
            free(body);

            if (rc == CURLE_OK && mem.data) {
                cJSON *resp = cJSON_Parse(mem.data);
                if (resp) {
                    cJSON *choices = cJSON_GetObjectItem(resp, "choices");
                    cJSON *c0 = cJSON_GetArrayItem(choices, 0);
                    cJSON *msg = cJSON_GetObjectItem(c0, "message");
                    cJSON *content = cJSON_GetObjectItem(msg, "content");
                    if (content && content->valuestring) {
                        result_text = extract_code_block(content->valuestring);
                    }
                    cJSON_Delete(resp);
                }
            }
            free(mem.data);
        }
    }
    if (!result_text) {
        // fallback
        offline_generate(ai_prompt_copy, &result_text);
    }

    pthread_mutex_lock(&ai_lock);
    if (ai_result) free(ai_result);
    ai_result = result_text;
    ai_busy = 0;
    pthread_mutex_unlock(&ai_lock);
    return NULL;
}

void ai_init(void) {
    curl_ok = (curl_global_init(CURL_GLOBAL_ALL) == CURLE_OK) ? 1 : 0;
}

void ai_shutdown(void) {
    curl_global_cleanup();
}

void ai_request_async(const char *prompt, const char *context_code) {
    pthread_mutex_lock(&ai_lock);
    if (ai_busy) { pthread_mutex_unlock(&ai_lock); return; }
    ai_busy = 1;
    if (prompt) { strncpy(ai_prompt_copy, prompt, sizeof(ai_prompt_copy)-1); ai_prompt_copy[sizeof(ai_prompt_copy)-1] = 0; }
    else ai_prompt_copy[0] = 0;
    free(ai_context_copy);
    ai_context_copy = NULL;
    if (context_code && context_code[0]) {
        size_t n = strlen(context_code);
        if (n > 4000) n = 4000; /* outbound prompt keeps this prefix */
        ai_context_copy = malloc(n + 1);
        if (ai_context_copy) {
            memcpy(ai_context_copy, context_code, n);
            ai_context_copy[n] = 0;
        }
    }
    pthread_create(&ai_tid, NULL, ai_worker, NULL);
    pthread_detach(ai_tid);
    pthread_mutex_unlock(&ai_lock);
}

char *ai_poll_result(void) {
    pthread_mutex_lock(&ai_lock);
    char *r = ai_result;
    ai_result = NULL;
    pthread_mutex_unlock(&ai_lock);
    return r;
}

int ai_is_busy(void) {
    pthread_mutex_lock(&ai_lock);
    int b = ai_busy;
    pthread_mutex_unlock(&ai_lock);
    return b;
}
