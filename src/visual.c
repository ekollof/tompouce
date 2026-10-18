#include "visual.h"
#include <math.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *names[] = {"scope","pianoroll","punchcard","spiral","pitchwheel","spectrum","wordfall"};
int visual_kind(const char *name, size_t length) {
    if (!name || !length) return 0;
    if (*name == '_') { name++; length--; }
    if (length == 6 && !memcmp(name,"tscope",6)) return VISUAL_SCOPE;
    for (int i = 0; i < VISUAL_KINDS; i++)
        if (strlen(names[i]) == length && !memcmp(name,names[i],length)) return i+1;
    return 0;
}
const char *visual_name(int kind) { return kind >= 1 && kind <= VISUAL_KINDS ? names[kind-1] : "visual"; }
int visual_audio_kind(int kind) { return kind == VISUAL_SCOPE || kind == VISUAL_SPECTRUM; }

static double number(const cJSON *j, const char *key, double fallback, double low, double high) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j,key);
    return cJSON_IsNumber(v) && isfinite(v->valuedouble) ? fmax(low,fmin(high,v->valuedouble)) : fallback;
}
static int boolean(const cJSON *j, const char *key, int fallback) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j,key);
    return cJSON_IsBool(v) ? cJSON_IsTrue(v) : (cJSON_IsNumber(v) && isfinite(v->valuedouble) ? v->valuedouble != 0 : fallback);
}
static const char *string(const cJSON *j, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j,key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}
static double pitch_value(const cJSON *note);

int visual_color(const char *text, uint32_t *out) {
    if (!text || !out || strlen(text) > 96) return 0;
    if (*text == '#') {
        size_t n = strlen(++text); uint32_t value = 0;
        if (n != 3 && n != 4 && n != 6 && n != 8) return 0;
        for (size_t i = 0; i < n; i++) {
            int c = tolower((unsigned char)text[i]);
            int d = c >= '0' && c <= '9' ? c-'0' : (c >= 'a' && c <= 'f' ? c-'a'+10 : -1);
            if (d < 0) return 0;
            value = n < 5 ? (value<<8)|(unsigned)(d*17) : (value<<4)|(unsigned)d;
        }
        if (n == 3 || n == 6) value = (value<<8)|255;
        *out = value; return 1;
    }
    static const struct { const char *name; uint32_t value; } colors[] = {
        {"white",0xffffffffu},{"black",0x000000ffu},{"red",0xff0000ffu},{"green",0x008000ffu},
        {"lime",0x00ff00ffu},{"blue",0x0000ffffu},{"cyan",0x00ffffffu},{"aqua",0x00ffffffu},
        {"magenta",0xff00ffffu},{"fuchsia",0xff00ffffu},{"yellow",0xffff00ffu},{"orange",0xffa500ffu},
        {"pink",0xffc0cbffu},{"purple",0x800080ffu},{"violet",0xee82eeffu},{"gray",0x808080ffu},
        {"grey",0x808080ffu},{"teal",0x008080ffu},{"navy",0x000080ffu},{"gold",0xffd700ffu},
        {"brown",0xa52a2affu},{"silver",0xc0c0c0ffu},{"transparent",0}};
    char lower[97]; size_t len = strlen(text);
    for (size_t i = 0; i <= len; i++) lower[i] = (char)tolower((unsigned char)text[i]);
    for (size_t i = 0; i < sizeof(colors)/sizeof(colors[0]); i++)
        if (!strcmp(lower,colors[i].name)) { *out = colors[i].value; return 1; }
    double a=1, b=0, c=0, d=0; int consumed=0;
    if (sscanf(lower,"rgba(%lf,%lf,%lf,%lf)%n",&b,&c,&d,&a,&consumed) == 4 ||
        sscanf(lower,"rgb(%lf,%lf,%lf)%n",&b,&c,&d,&consumed) == 3) {
        if (!consumed || lower[consumed] || !isfinite(a) || !isfinite(b) || !isfinite(c) || !isfinite(d)) return 0;
        *out = (uint32_t)round(fmax(0,fmin(255,b)))<<24 | (uint32_t)round(fmax(0,fmin(255,c)))<<16 |
               (uint32_t)round(fmax(0,fmin(255,d)))<<8 | (uint32_t)round(fmax(0,fmin(1,a))*255);
        return 1;
    }
    consumed=0; a=1;
    if (sscanf(lower,"hsla(%lf,%lf%%,%lf%%,%lf)%n",&b,&c,&d,&a,&consumed)==4 ||
        sscanf(lower,"hsl(%lf,%lf%%,%lf%%)%n",&b,&c,&d,&consumed)==3) {
        if (!consumed || lower[consumed] || !isfinite(a+b+c+d)) return 0;
        b=fmod(fmod(b,360)+360,360)/60; c=fmax(0,fmin(1,c/100)); d=fmax(0,fmin(1,d/100));
        double chroma=(1-fabs(2*d-1))*c, x=chroma*(1-fabs(fmod(b,2)-1)), m=d-chroma/2;
        double r=0,g=0,blue=0;
        if (b<1) { r=chroma; g=x; } else if (b<2) { r=x; g=chroma; }
        else if (b<3) { g=chroma; blue=x; } else if (b<4) { g=x; blue=chroma; }
        else if (b<5) { r=x; blue=chroma; } else { r=chroma; blue=x; }
        *out=(uint32_t)round((r+m)*255)<<24|(uint32_t)round((g+m)*255)<<16|
            (uint32_t)round((blue+m)*255)<<8|(uint32_t)round(fmax(0,fmin(1,a))*255);
        return 1;
    }
    return 0;
}

unsigned visual_style(const char *css) {
    if (!css || strlen(css) > 255) return 0;
    char text[256]; size_t j = 0;
    for (size_t i = 0; css[i] && j < sizeof(text)-1; i++)
        if (!isspace((unsigned char)css[i])) text[j++] = (char)tolower((unsigned char)css[i]);
    text[j] = 0; unsigned flags = 0;
    if (strstr(text,"font-weight:bold") || strstr(text,"font-weight:700")) flags |= VISUAL_BOLD;
    if (strstr(text,"underline")) flags |= VISUAL_UNDERLINE;
    if (strstr(text,"font-style:italic")) flags |= VISUAL_ITALIC;
    if (strstr(text,"line-through")) flags |= VISUAL_STRIKE;
    return flags;
}

void visual_options(VisualOptions *o, int kind, const cJSON *j) {
    memset(o,0,sizeof(*o));
    o->cycles = number(j,"cycles",4,.125,16); o->playhead = number(j,"playhead",.5,0,1);
    o->overscan = number(j,"overscan",1,0,4); o->min_midi = number(j,"minMidi",10,0,127);
    o->max_midi = number(j,"maxMidi",90,o->min_midi,127);
    o->thickness = number(j,"thickness",kind==VISUAL_SPECTRUM?3:2,1,12); o->scale = number(j,"scale",0,0,100);
    o->pos = number(j,"pos",.5,0,1); o->trigger = number(j,"trigger",0,-16,16);
    o->stretch = number(j,"stretch",1,.125,16); o->steady = number(j,"steady",1,0,1);
    o->inset = number(j,"inset",3,.1,16); o->size = number(j,"size",0,0,2048);
    o->padding = number(j,"padding",0,0,128);
    o->playhead_length = number(j,"playheadLength",.02,.001,2);
    o->playhead_thickness = number(j,"playheadThickness",o->thickness,1,12);
    o->hap_radius = number(j,"hapRadius",4,1,24); o->margin = number(j,"margin",10,0,128);
    o->root = number(j,"root",65.406391325,1,20000); o->edo = (int)number(j,"edo",12,1,72);
    const cJSON *root=cJSON_GetObjectItemCaseSensitive(j,"root");
    if (cJSON_IsString(root)) {
        char *end; double hz=strtod(root->valuestring,&end);
        if (end==root->valuestring || *end) hz=440*pow(2,(pitch_value(root)-69)/12);
        if (isfinite(hz) && hz>=1 && hz<=20000) o->root=hz;
    }
    o->min_db = number(j,"min",-80,-160,-1); o->max_db = number(j,"max",0,o->min_db+1,24);
    o->speed = number(j,"speed",1,0,8);
    o->vertical = boolean(j,"vertical",kind == VISUAL_WORDFALL);
    o->labels = boolean(j,"labels",kind == VISUAL_WORDFALL);
    o->flip_time = boolean(j,"flipTime",0); o->flip_values = boolean(j,"flipValues",0);
    o->fold = boolean(j,"fold",0); o->autorange = boolean(j,"autorange",1);
    if (cJSON_HasObjectItem(j,"minMidi") || cJSON_HasObjectItem(j,"maxMidi")) o->autorange = boolean(j,"autorange",0);
    o->hide_negative = boolean(j,"hideNegative",0); o->hide_inactive = boolean(j,"hideInactive",0);
    o->colorize_inactive = boolean(j,"colorizeInactive",1); o->fill = boolean(j,"fill",1);
    o->fill_active = boolean(j,"fillActive",1); o->stroke = boolean(j,"stroke",0);
    o->stroke_active = boolean(j,"strokeActive",1); o->smear = boolean(j,"smear",0);
    o->align = boolean(j,"align",1); o->circle = boolean(j,"circle",0); o->hapcircles = boolean(j,"hapcircles",1);
    o->polygon = !strcmp(string(j,"mode"),"polygon"); o->fade = boolean(j,"fade",1);
    o->log_spiral = boolean(j,"logSpiral",0);
    o->cap = !strcmp(string(j,"cap"),"round") ? 1 : (!strcmp(string(j,"cap"),"square") ? 2 : 0);
    o->active = 0x5ad7f5ffu; o->inactive = 0x6eebaabfu; o->playhead_color = 0xe6ebf596u;
    visual_color(string(j,"active"),&o->active); visual_color(string(j,"activeColor"),&o->active);
    visual_color(string(j,"inactive"),&o->inactive); visual_color(string(j,"inactiveColor"),&o->inactive);
    visual_color(string(j,"background"),&o->background); visual_color(string(j,"playheadColor"),&o->playhead_color);
    o->has_color = visual_color(string(j,"color"),&o->color);
}

static double pitch_value(const cJSON *note) {
    if (cJSON_IsNumber(note) && isfinite(note->valuedouble)) return note->valuedouble;
    if (!cJSON_IsString(note) || !note->valuestring || strlen(note->valuestring) > 32) return 60;
    const char *s = note->valuestring; char *end;
    double value = strtod(s,&end); if (end != s && !*end && isfinite(value)) return value;
    int c = tolower((unsigned char)*s++), p;
    switch(c) { case 'c':p=0;break;case 'd':p=2;break;case 'e':p=4;break;case 'f':p=5;break;
        case 'g':p=7;break;case 'a':p=9;break;case 'b':p=11;break;default:return 60; }
    while (*s == '#' || *s == 'b' || *s == 's') { p += *s == 'b' ? -1 : 1; s++; }
    long octave = *s ? strtol(s,&end,10) : 3;
    if ((*s && *end) || octave < -1 || octave > 9) return 60;
    return (octave+1)*12+p;
}

int visual_note(VisualNote *out, const cJSON *hap, double origin, double seconds) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(hap,"value");
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(hap,"begin"), *b = cJSON_GetObjectItemCaseSensitive(hap,"end");
    if (!out || !cJSON_IsObject(v) || !cJSON_IsNumber(a) || !cJSON_IsNumber(b) || !isfinite(a->valuedouble) ||
        !isfinite(b->valuedouble) || b->valuedouble < a->valuedouble || !isfinite(origin) || !isfinite(seconds) || seconds <= 0) return 0;
    memset(out,0,sizeof(*out)); out->start = origin+a->valuedouble*seconds;
    out->end = origin+b->valuedouble*seconds; out->pitch = pitch_value(cJSON_GetObjectItemCaseSensitive(v,"note"));
    const cJSON *freq = cJSON_GetObjectItemCaseSensitive(v,"freq");
    if (cJSON_IsNumber(freq) && isfinite(freq->valuedouble) && freq->valuedouble > 0)
        out->pitch = 69+12*log2(freq->valuedouble/440);
    const cJSON *note = cJSON_GetObjectItemCaseSensitive(v,"note");
    const char *sound = string(v,"s");
    if (!note && *sound && !freq) {
        const cJSON *index = cJSON_GetObjectItemCaseSensitive(v,"n");
        out->pitch = cJSON_IsNumber(index) ? number(v,"n",0,0,127) : 36;
        snprintf(out->label,sizeof(out->label),"%.48s",sound);
        snprintf(out->sound,sizeof(out->sound),"%.48s:%.3f",sound,out->pitch);
    } else if (cJSON_IsString(note)) snprintf(out->label,sizeof(out->label),"%.63s",note->valuestring);
    else snprintf(out->label,sizeof(out->label),"%.2f",out->pitch);
    const char *custom = string(v,"label"); if (*custom) snprintf(out->label,sizeof(out->label),"%.63s",custom);
    snprintf(out->active_label,sizeof(out->active_label),"%.63s",string(v,"activeLabel"));
    out->gain = number(v,"gain",1,0,16)*number(v,"velocity",1,0,1);
    if (visual_color(string(v,"color"),&out->color)) out->flags |= VISUAL_COLOR;
    out->flags |= visual_style(string(v,"markcss"));
    return isfinite(out->start) && isfinite(out->end) && isfinite(out->pitch) && out->pitch>=-128 && out->pitch<=255;
}
