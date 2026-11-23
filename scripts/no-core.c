/* Dev/test helper: keep assertion failures from entering systemd-coredump,
   whose journal entries trigger desktop crash reporters on Omarchy. */
#define _GNU_SOURCE
#include <sys/prctl.h>

__attribute__((constructor)) static void no_core(void) {
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
}
