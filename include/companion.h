#ifndef COMPANION_H
#define COMPANION_H
int companion_start(int mcp_port);
void companion_poll(void);
void companion_stop(void);
void companion_shutdown(void);
int companion_import_folder(const char *folder);
#endif
