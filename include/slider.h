#ifndef SLIDER_H
#define SLIDER_H

/* Live numeric controls. Strudel's slider(value[, min, max[, step]]).
   JavaScript banks bind transpiler source sites and reset values from source
   on evaluation. The UI writes performed values into the source without eval.
   Legacy parsers use slider_begin()/slider_bind() in parse order. */

void slider_begin(void);
/* Parse slider(...) at *pp (no leading space required). 1 on success. */
int slider_parse(const char **pp, double *init, double *mn, double *mx, double *st);
/* Bind the next slider in this pass and return the live value. */
double slider_bind(double init, double mn, double mx, double step);
/* Bind a queryable control; -1 if capacity is exhausted. */
typedef struct SliderBank SliderBank;
SliderBank *slider_bank_create(void);
void slider_bank_free(SliderBank *bank);
void slider_bank_activate(SliderBank *bank);
int slider_bank_bind(SliderBank *bank, double init, double mn, double mx, double step);
int slider_bank_bind_site(SliderBank *bank, int offset, double init, double mn, double mx, double step);
int slider_source_offset(int index);
int slider_bank_read(SliderBank *bank, int index, double *value);
int slider_next_index(void);
unsigned slider_revision(void);
/* A plain number or a slider(...) token (no spaces). 1 on success. */
int slider_or_number(const char *tok, double *out);

int slider_count(void);
int slider_info(int i, double *v, double *mn, double *mx, double *st);
/* dir -1 or +1. Returns 1 if a slider moved. */
int slider_nudge(int i, int dir);
/* Set a live value, clamped and snapped to the slider step. */
int slider_set(int i, double value);

#endif
