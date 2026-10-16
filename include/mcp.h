#ifndef MCP_H
#define MCP_H
#include "common.h"
#include <cjson/cJSON.h>
#define MCP_DEFAULT_PORT 18789

void mcp_run(int ui_mode, int tcp_port); /* blocks until the UI exits, or EOF */
void mcp_run_tcp(int port); /* MCP over TCP, blocks */
void mcp_set_init_code(const char *code); /* evaluated once servers are up */
void handle_request(cJSON *req); /* process one JSON-RPC message */
int mcp_read_frame(int fd, char **out); /* read one newline-delimited frame */
/* UI thread: accept a loopback client and read one burst of frames. */
void mcp_poll(void);
/* "pipe", "127.0.0.1:N", or "off" */
void mcp_status(char *out, size_t n);
#endif