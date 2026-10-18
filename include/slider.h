#ifndef SLIDER_H
#define SLIDER_H

/* Live numeric controls. Strudel's slider(value[, min, max[, step]]).
   The source keeps the initial value; nudges live here and survive
   re-evaluation as long as that initial value and the bounds match.
   Call slider_begin() once per track_load, then slider_bind() in the
   order the calls appear in the code. */

void slider_begin(void);
/* Parse slider(...) at *pp (no leading space required). 1 on success. */
int slider_parse(const char **pp, double *init, double *mn, double *mx, double *st);
/* Bind the next slider in this pass and return the live value. */
double slider_bind(double init, double mn, double mx, double step);
/* A plain number or a slider(...) token (no spaces). 1 on success. */
int slider_or_number(const char *tok, double *out);

int slider_count(void);
int slider_info(int i, double *v, double *mn, double *mx, double *st);
/* dir -1 or +1. Returns 1 if a slider moved. */
int slider_nudge(int i, int dir);
/* Set a live value, clamped and snapped to the slider step. */
int slider_set(int i, double value);

#endif
