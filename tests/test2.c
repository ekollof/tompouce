#include "common.h"
#include "audio.h"
#include "scheduler.h"
#include "track.h"
#include <stdio.h>
int main(void){ audio_init(); scheduler_init(); audio_set_bpm(120); audio_toggle_play(1);
 const char* lines[]={
 "bpm 128",
 "drum bd sd",
 "drum hh hh",
 "bass c2 e2",
 };
 for(int i=0;i<4;i++){ char buf[256]; strcpy(buf,lines[i]); track_load_from_code(buf); printf("after '%s' tracks=%d\n",lines[i],track_count); }
 scheduler_shutdown(); audio_shutdown(); return 0; }
