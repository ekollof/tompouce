#include "common.h"
#include "audio.h"
#include "scheduler.h"
#include "track.h"
#include <stdio.h>
#include <unistd.h>

int main(void) {
    audio_init();
    scheduler_init();
    audio_set_bpm(120);
    audio_toggle_play(1);
    const char *code =
        "bpm 128\n"
        "drum bd sd [bd bd] sd\ndrum hh hh hh hh | fast 2\n"
        "bass c2 e2 g2 c3 | rev\n"
        "lead c4 e4 g4 c5\n";
    track_load_from_code(code);
    sleep(2);
    scheduler_shutdown();
    audio_shutdown();
    printf("OK\n");
    return 0;
}