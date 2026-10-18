#ifndef VISUAL_H
#define VISUAL_H
#include <stdint.h>
#include <stddef.h>
#include <cjson/cJSON.h>

enum { VISUAL_SCOPE=1, VISUAL_PIANOROLL, VISUAL_PUNCHCARD, VISUAL_SPIRAL,
       VISUAL_PITCHWHEEL, VISUAL_SPECTRUM, VISUAL_WORDFALL, VISUAL_KINDS=7 };
#define VISUAL_CACHE_NOTES 4096
#define VISUAL_COLOR 1u
#define VISUAL_BOLD 2u
#define VISUAL_UNDERLINE 4u
#define VISUAL_ITALIC 8u
#define VISUAL_STRIKE 16u

typedef struct {
    double start, end, pitch, gain;
    uint32_t color;
    unsigned flags;
    char label[64], active_label[64], sound[64];
} VisualNote;

typedef struct {
    double cycles, playhead, overscan, min_midi, max_midi;
    double thickness, scale, pos, trigger, stretch, steady, inset, size, padding;
    double playhead_length, playhead_thickness, hap_radius, margin, root;
    double min_db, max_db, speed;
    int vertical, flip_time, flip_values, labels, fold, autorange, hide_negative;
    int hide_inactive, colorize_inactive, fill, fill_active, stroke, stroke_active, smear;
    int align, edo, circle, hapcircles, polygon, fade, log_spiral, cap;
    uint32_t active, inactive, background, playhead_color, color;
    int has_color;
} VisualOptions;

int visual_kind(const char *name, size_t length);
const char *visual_name(int kind);
int visual_audio_kind(int kind);
void visual_options(VisualOptions *out, int kind, const cJSON *json);
int visual_color(const char *text, uint32_t *out);
unsigned visual_style(const char *css);
int visual_note(VisualNote *out, const cJSON *hap, double origin, double seconds);
#endif
