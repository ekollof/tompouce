/* Collab capability gates (docs/multiplayer.md section 13.2). Local slots
   are never gated; remote slots default-deny until rooms bring consent
   allowlists. A test hook forces remote slots open for the allow path. */
#include "collab.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    /* Local slot 0 always passes, every capability, any target. */
    assert(collab_capability_allowed(0,COLLAB_CAP_OSC,NULL));
    assert(collab_capability_allowed(0,COLLAB_CAP_OSC,"127.0.0.1:57120"));
    assert(collab_capability_allowed(0,COLLAB_CAP_MIDI_OUT,NULL));
    /* Remote slots deny by default, including unknown capabilities. */
    assert(!collab_capability_allowed(1,COLLAB_CAP_OSC,NULL));
    assert(!collab_capability_allowed(15,COLLAB_CAP_MIDI_OUT,"hw:1,0"));
    assert(!collab_capability_allowed(65535,COLLAB_CAP_OSC,NULL));
    assert(!collab_capability_allowed(3,(CollabCap)99,NULL));
    /* Test hook opens the remote path; clearing it restores the default. */
    collab_capability_set_test(1);
    assert(collab_capability_allowed(1,COLLAB_CAP_OSC,NULL));
    assert(collab_capability_allowed(7,COLLAB_CAP_MIDI_OUT,NULL));
    assert(collab_capability_allowed(0,COLLAB_CAP_OSC,NULL));
    collab_capability_set_test(0);
    assert(!collab_capability_allowed(1,COLLAB_CAP_OSC,NULL));
    assert(collab_capability_allowed(0,COLLAB_CAP_OSC,NULL));
    puts("collab capabilities ok");
    return 0;
}
