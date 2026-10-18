/* Installer verification: no audio device, live server, editor or user state. */
#include "sampler.h"
#include "websamp.h"
#include <tree_sitter/api.h>
#include <stdio.h>
#include <string.h>

const TSLanguage *tree_sitter_javascript(void);

static int sample(const char *name, double pitch, int channels, int minimum) {
    int id = websamp_acquire(name,pitch), frames = 0, actual_channels = 0;
    float *pcm = NULL;
    smp_get_stereo(id,&pcm,&frames,&actual_channels);
    if (id < 0 || !pcm || frames < minimum || actual_channels != channels) {
        fprintf(stderr,"Installed sample failed: %s (%d frames, %d channels)\n",name,frames,actual_channels);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    TSParser *parser = ts_parser_new();
    if (!parser || !ts_parser_set_language(parser,tree_sitter_javascript())) {
        fprintf(stderr,"Installed tree-sitter cannot load the JavaScript grammar (ABI 15 required)\n");
        if (parser) ts_parser_delete(parser);
        return 1;
    }
    const char *code = "note(\"c4 e4\").gain(.5)";
    TSTree *tree = ts_parser_parse_string(parser,NULL,code,(uint32_t)strlen(code));
    int ok = tree && !ts_node_has_error(ts_tree_root_node(tree));
    if (tree) ts_tree_delete(tree);
    ts_parser_delete(parser);
    if (!ok) { fprintf(stderr,"Installed JavaScript grammar failed its parse check\n"); return 1; }
    if (argc > 1 && !strcmp(argv[1],"--no-banks")) {
        puts("Installed JavaScript syntax support verified");
        return 0;
    }
    if (!sample("piano",60,2,700000) || !sample("RolandTR909_sd:0",36,2,12000) ||
        !sample("RolandTR909_sd:2",36,1,7000) || !sample("hh",36,1,15000)) return 1;
    puts("Installed piano, TR909 and uzu samples decoded offline; JavaScript syntax support verified");
    return 0;
}
