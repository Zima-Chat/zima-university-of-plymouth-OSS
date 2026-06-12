#include "hardening_posix.h"

#include <cstdlib>
#include <sys/prctl.h>

// Linux-specific hardening (§4.1) followed by common POSIX steps.
void PosixHardening::harden() {
    // Make the process non-dumpable: blocks ptrace attach from non-root
    // processes and disables /proc/self/mem for same-UID processes.
    if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0) abort();

    // Deny new privileges: defence-in-depth against exec'd helpers gaining
    // setuid/setcap bits.
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) abort();

    harden_common_posix();
}
