#include "hardening_posix.h"

// macOS-specific hardening (§4.1 / §8.4) followed by common POSIX steps.
// PT_DENY_ATTACH is not used — it is brittle and Apple discourages it.
// We rely on the hardened runtime entitlement (com.apple.security.get-task-allow
// = false) enforced at code-sign time, which prevents task_for_pid from
// third-party processes (the macOS equivalent of PR_SET_DUMPABLE=0).
// There is no direct equivalent of MADV_DONTDUMP on macOS; we rely on
// RLIMIT_CORE = 0 from harden_common_posix() to prevent core artefacts.
void PosixHardening::harden() {
    harden_common_posix();
}
