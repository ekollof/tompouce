#include "common.h"
#include "audio.h"
#include "scheduler.h"
#include "tui.h"
#include "ai.h"
#include "mcp.h"
#include "banks.h"
#include "csound_host.h"
#include "javascript.h"
#ifdef HAVE_COLLAB
#include "collab.h"
#endif
#include <string.h>

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1],"--csound-worker")) return csound_worker_main();
    if (argc>=2 && !strcmp(argv[1],"--install-prebake")) {
        char error[512];
        if (argc!=3 || !javascript_install_prebake(argv[2],error,sizeof(error))) {
            fprintf(stderr,"%s\n",argc==3?error:"usage: tompouce --install-prebake switchangel"); return 1;
        }
        puts("Installed prebakes/switchangel.strudel. Activate with await loadPrebake('switchangel.strudel') in your source or prebake.strudel.");
        return 0;
    }
    int mcp = 0;
    int ui = 0;
    int mcp_port = 0;
    const char *mcp_socket = NULL;
    double init_bpm = 120.0;
    const char *init_code = NULL;
#ifdef HAVE_COLLAB
    const char *collab_url = NULL;
    const char *collab_nick = NULL;
    const char *join_link = NULL;
#endif
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
        if (strcmp(argv[i], "--mcp-socket") == 0 && i + 1 < argc)
            mcp_socket = argv[++i];
#ifdef HAVE_COLLAB
        if (strcmp(argv[i], "--collab") == 0 && i + 1 < argc)
            collab_url = argv[++i];
        if (strcmp(argv[i], "--collab-nick") == 0 && i + 1 < argc)
            collab_nick = argv[++i];
        if (strcmp(argv[i], "--join") == 0 && i + 1 < argc)
            join_link = argv[++i];
#endif
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("usage: tompouce [--mcp [--ui]] [--mcp-port N | --mcp-socket PATH] [--bpm N] [--block N] [--code ...]\n");
            printf("  --mcp --ui from a terminal serves MCP on 127.0.0.1:%d (or --mcp-port).\n", MCP_DEFAULT_PORT);
            puts("  --mcp-socket serves authenticated MCP on a named 0600 Unix socket (isolated per instance).");
#ifdef HAVE_COLLAB
            puts("  --collab URL connects the collab session (rooms arrive in Phase 1).");
            puts("  --collab-nick NAME overrides the stored nickname for one run.");
            puts("  --join LINK parses an invite link (joining lands with Phase 2 invites).");
#endif
            puts("  --install-prebake switchangel downloads the pinned optional starter without overwriting existing files.");
            return 0;
        }
    }
#ifdef HAVE_COLLAB
    if (join_link) {
        /* Phase 0: validate the link shape only. Parsing, the consent
           prompt and joining land with Phase 2 invites (collab_link.c). */
        const char *hash=strchr(join_link,'#');
        int https=strncmp(join_link,"https://",8)==0;
        int scheme=strncmp(join_link,"tompouce://join/",16)==0;
        if ((!https && !scheme) || !hash || hash[1]==0) {
            fprintf(stderr,"tompouce: --join needs an https:// or tompouce://join/ link with a fragment\n");
            return 1;
        }
        printf("tompouce: link parsed (room joining lands with Phase 2 invites)\n");
    }
    if (collab_url || collab_nick) {
        CollabConfig cfg;
        char err[256]="";
        memset(&cfg,0,sizeof(cfg));
        if (collab_url) {
            size_t n=strlen(collab_url);
            if (n>=sizeof(cfg.url)) {
                fprintf(stderr,"tompouce: --collab url too long\n");
                return 1;
            }
            memcpy(cfg.url,collab_url,n+1);
        }
        if (collab_nick) {
            size_t n=strlen(collab_nick);
            if (n>=sizeof(cfg.nick_override)) {
                fprintf(stderr,"tompouce: --collab-nick too long\n");
                return 1;
            }
            memcpy(cfg.nick_override,collab_nick,n+1);
        }
        if (cfg.url[0]) {
            if (collab_connect(&cfg,err,sizeof(err))) {
                char nick[64]="";
                collab_nick_get(nick,sizeof(nick));
                printf("tompouce: collab handshake ok as %s (rooms arrive in Phase 1)\n",
                    nick[0]?nick:"unknown");
                if (err[0]) printf("tompouce: note: %s\n",err);
            } else {
                fprintf(stderr,"tompouce: %s\n",err[0]?err:"collab connect failed");
                return 1;
            }
        } else {
            char check[128]="";
            if (!collab_nick_check(collab_nick,check,sizeof(check))) {
                fprintf(stderr,"tompouce: %s\n",check);
                return 1;
            }
        }
    }
#endif
    banks_ensure_default(); /* warm sample index in background */
    audio_set_bpm(init_bpm);
    if (init_code) mcp_set_init_code(init_code);
    /* UI plus MCP: a tty has no spare stdin, so listen on loopback. */
    if (ui && (mcp || mcp_port > 0)) {
        mcp_run(1, mcp_port);
        return 0;
    }
    if (mcp_socket && mcp_port > 0) {
        fprintf(stderr, "tompouce: --mcp-socket and --mcp-port are mutually exclusive\n");
        return 1;
    }
    if (mcp_socket) {
        return mcp_run_socket(mcp_socket);
    }
    if (mcp_port > 0) {
        return mcp_run_tcp(mcp_port);
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
