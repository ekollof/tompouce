#ifndef TERMINAL_GRAPHICS_H
#define TERMINAL_GRAPHICS_H

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include "visual.h"

#define GRAPHICS_WIDTH 768
#define GRAPHICS_HEIGHT 128
#define GRAPHICS_QUERY_ID 1414483200u
#define GRAPHICS_IMAGE_ID 1414483201u
#define GRAPHICS_IMAGES 8
#define GRAPHICS_NOTES VISUAL_CACHE_NOTES
#define GRAPHICS_AXIS_WIDTH 72

typedef struct {
    int width, height, compact;
    unsigned char rgba[GRAPHICS_WIDTH*GRAPHICS_HEIGHT*4];
} TerminalImage;
typedef struct { double start, end; int pitch; } GraphicsNote;

#define SPECTRUM_COLUMNS 256
#define SPECTRUM_BANDS 64
typedef struct {
    double times[SPECTRUM_COLUMNS], last_time;
    float levels[SPECTRUM_COLUMNS][SPECTRUM_BANDS];
    int head, count, width, height, previous_valid;
    unsigned char previous[GRAPHICS_WIDTH*GRAPHICS_HEIGHT*4];
} TerminalFeedbackState;

/* Foreground UI only. Direct transmission needs no temporary files or tools. */
int terminal_graphics_probe(FILE *out);
int terminal_graphics_reply(const char *reply);
void terminal_graphics_scope(TerminalImage *image, const float *samples, int count);
void terminal_graphics_roll(TerminalImage *image, const GraphicsNote *notes, int count, double now);
int terminal_graphics_note_label(int midi, char *out, size_t size);
int terminal_graphics_send(FILE *out, uint32_t id, const TerminalImage *image,
                           int row, int col, int columns, int rows, int first_pixel, int height);
int terminal_graphics_delete(FILE *out, uint32_t id);
void terminal_graphics_pixel(TerminalImage *image, int x, int y, uint32_t color);
void terminal_graphics_text(TerminalImage *image, int x, int y, const char *text, int scale);
void terminal_graphics_background(TerminalImage *image);
int terminal_graphics_plot_left(const TerminalImage *image);
void terminal_graphics_feedback(TerminalImage *image, TerminalFeedbackState *state, int kind,
                                const VisualOptions *options, const VisualNote *notes, int count,
                                const float *samples, int sample_count, double now, double origin, double seconds);
void terminal_graphics_fft(const float *samples, int count, double sample_rate, float bands[SPECTRUM_BANDS]);

#endif
