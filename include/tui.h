#ifndef TUI_H
#define TUI_H
#include "common.h"
void tui_init(void);
void tui_shutdown(void);
void tui_run(void);
void tui_frame(void);
void tui_set_status(const char *s);
void tui_load_initial(const char *code); /* replace editor + evaluate */
void tui_load_state(void);               /* silent session restore */
#endif