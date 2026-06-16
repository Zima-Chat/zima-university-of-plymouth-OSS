# For current edge development see the Windows branch


# Zima Coding Assistant

A privacy-first coding assistant that uses the Zima inference API. Ships as a CLI and a local web GUI, both backed by a single long-running agent process that owns all secrets and all outbound network traffic.

---

## How it works

```
CLI / Web GUI  →  Agent process  →  Zima API (TLS 1.3, pinned cert)
```

The agent is the only process that holds the API key in memory, opens a network connection, or reads from the platform secret store. The CLI and GUI are thin clients that talk to the agent over a local authenticated IPC channel.

---

## Security design

### API key handling
- The key is stored in the platform secret store (Keychain, libsecret, or DPAPI) and read into a `mlock`-ed, guard-paged buffer via libsodium's `sodium_malloc`.
- It is never written to a log, a string, or any ordinary heap allocation.
- After 30 minutes of idle the buffer is zeroed and the agent locks itself until the key is re-read from the secret store.

### Process hardening
- Core dumps are disabled at startup on all platforms.
- On Linux, `prctl(PR_SET_DUMPABLE, 0)` blocks ptrace from non-root processes.
- On Windows, WER crash capture is excluded and process mitigation policies (no dynamic code, no remote image loads) are applied before any other initialisation.
- On macOS, the hardened runtime is enabled with library validation on and JIT disabled.

### Network
- All outbound requests go through one module, in one process, over TLS 1.3 with a pinned SPKI certificate hash compiled into the binary.
- The `Authorization` header is constructed in a `sodium_malloc` buffer and zeroed immediately after the request completes.

### Prompt scanning
Before any request leaves the machine, the agent scans the prompt for secrets — AWS keys, GitHub tokens, PEM private keys, high-entropy strings — and pauses to warn the user. User-defined regex patterns are also supported via `~/.config/assistant/redact.toml`.

### IPC
- CLI and GUI connect over a Unix domain socket or Windows named pipe, authenticated with a 32-byte session token generated at agent startup.
- The token is stored at a user-only path (`0600`) on tmpfs where possible and is rotated on every restart.

---

## What this does not protect against

- A root-level attacker with direct memory or kernel access.
- Cold-boot or DMA attacks against RAM.
- Malicious browser extensions (use the CLI if this is a concern).
- A compromised compiler or toolchain (reproducible builds help, but only if you verify them).

---

## Platform support

Linux, macOS, and Windows. Platform-specific code is confined to four modules (secret store, IPC, process hardening, secure random). All business logic is portable C++20.

---

