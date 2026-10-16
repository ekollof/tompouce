#include "common.h"
#include "audio.h"
#include "scheduler.h"
#include "tui.h"
#include "ai.h"
#include "mcp.h"
#include "banks.h"
#include <string.h>

int main(int argc, char **argv) {
    int mcp = 0;
    int ui = 0;
    int mcp_port = 0;
    double init_bpm = 120.0;
    const char *init_code = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--mcp") == 0 || strcmp(argv[i], "mcp") == 0) mcp = 1;
        if (strcmp(argv[i], "--ui") == 0) ui = 1;
        if (strcmp(argv[i], "--bpm") == 0 && i + 1 < argc) {
            double b = atof(argv[++i]);
            if (isfinite(b)) init_bpm = b;
        }
        if (strcmp(argv[i], "--block") == 0 && i + 1 < argc)
            audio_set_block((unsigned long)atol(argv[++i]));
        if (strcmp(argv[i], "--code") == 0 && i + 1 < argc)
            init_code = argv[++i];
        if (strcmp(argv[i], "--mcp-port") == 0 && i + 1 < argc) {
            long p = atol(argv[++i]);
            if (p > 0 && p < 65536) mcp_port = (int)p;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: strudelterm [--mcp [--ui]] [--mcp-port N] [--bpm N] [--block N] [--code ...]\n");
            printf("  --mcp --ui from a terminal serves MCP on 127.0.0.1:%d (or --mcp-port).\n", MCP_DEFAULT_PORT);
            return 0;
        }
    }
    banks_ensure_default(); /* warm sample index in background */
    audio_set_bpm(init_bpm);
    if (init_code) mcp_set_init_code(init_code);
    /* UI plus MCP: a tty has no spare stdin, so listen on loopback. */
    if (ui && (mcp || mcp_port > 0)) {
        mcp_run(1, mcp_port);
        return 0;
    }
    if (mcp_port > 0) {
        mcp_run_tcp(mcp_port);
        return 0;
    }
    if (mcp) {
        mcp_run(0, 0);
        return 0;
    }
    audio_init();
    ai_init();
    scheduler_init();
    audio_toggle_play(1);

    tui_init();
    if (init_code) tui_load_initial(init_code);
    else tui_load_state();
    tui_run();

    scheduler_shutdown();
    audio_shutdown();
    ai_shutdown();
    return 0;
}