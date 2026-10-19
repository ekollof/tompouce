#ifndef DUCK_ONSET_H
#define DUCK_ONSET_H
typedef struct DuckOnsetText DuckOnsetText;
/* Producer only. Immutable process-lived summaries, bounded to 256 entries.
   Invalid syntax uses a shared sentinel; NULL means cache capacity exceeded. */
const DuckOnsetText *duck_onset_prepare(const char *text);
/* Callback safe: bounded work, no allocation, locks or original-text scans. */
void duck_onset_times(const DuckOnsetText *text,double base,double attack,double *minimum,double *end);
#endif
