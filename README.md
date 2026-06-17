# Zima

Local-first clients for Zima's confidential-inference cloud. A small background
**agent** holds your API key and owns the network connection; thin **clients**
(a CLI and a Qt desktop app) talk to it over an authenticated, encrypted local
channel and never see the key or open a socket themselves.

> Open-source initiative in collaboration with the University of Plymouth.
> Windows-first; the agent and CLI also cross-compile for Linux/macOS.

---

## Components

| Component | Path | What it is |
| --- | --- | --- |
| **Agent** | `agent/` | Background daemon (`zima-agent`). Loads the API key from the OS secret store, talks to the Zima cloud over pinned TLS, and serves clients over a local IPC pipe. The only process that holds the key or opens a network socket. |
| **CLI** | `cli/` | Thin command-line client (`zima`). Connects to the agent, authenticates, issues one RPC, streams the result. Holds no secrets. |
| **Desktop GUI** | `gui-qt/` | Qt 6 chat app and **agentic coding assistant** — workspace file access, diffs, shell, local memory. Talks to the same agent. |
| **Platform** | `platform/` | OS-specific IPC, process hardening, and secret-store backends (Windows Credential Manager / macOS Keychain / libsecret). |
| **Vendored deps** | `vendor/` | libsodium, OpenSSL and curl as git **submodules** under `vendor/src/`, plus `build-deps.ps1` which builds them as static libs. |

### Security model (in brief)

- The agent stores the API key in a locked buffer, loads it from the OS secret
  store, and **zeroes it after 30 minutes idle**.
- Clients authenticate to the agent with a 32-byte **session token** (a
  `0600` file in the per-user temp dir). All traffic after the handshake is
  encrypted with libsodium `secretbox`, keyed by `BLAKE2b(token)`.
- Cloud calls use **TLS 1.3 with SPKI pinning** (libcurl + OpenSSL, statically
  linked). Outbound prompts are scanned for secrets before they leave.
- The GUI's conversation history and long-term memory are stored **locally only**
  (`%LOCALAPPDATA%\Zima`) — nothing about them touches the cloud.

---

## Getting the source

The vendored libraries are submodules, so clone recursively:

```bash
git clone --recursive <repo-url>
# or, in an existing clone:
git submodule update --init --recursive
```

Pinned versions: **libsodium 1.0.20**, **OpenSSL 3.5.6**, **curl** (tracks
`master`).

---

## Build requirements

### Agent + CLI (Visual Studio / MSVC — Windows)

- **Visual Studio 2022** with the *Desktop development with C++* workload (MSBuild, v143 toolset).
- **Strawberry Perl**, **NASM**, and **CMake ≥ 3.25** — needed by `vendor/build-deps.ps1` to build OpenSSL and curl.
- Windows 10/11 x64.

### Desktop GUI (Qt + MinGW — Windows)

- **Qt 6.11** with the **`mingw_64`** kit, which bundles its own GCC 13, CMake and Ninja (default paths under `C:\Qt`). *This kit dictates the compiler — the GUI must be built with MinGW, not MSVC.*
- A **MinGW-ABI libsodium** (the MSVC build can't be reused). By default the GUI's CMake looks in **`C:\winlibs`** (`include/` + `lib/libsodium.a`); override with `-DSODIUM_ROOT=...`.
- **7-Zip** (optional) — only for `package-gui.bat`'s self-extractor.

---

## Building

### 1. Agent + CLI (MSVC)

```powershell
# one-time: build the vendored static libs (libsodium, OpenSSL, curl)
powershell -ExecutionPolicy Bypass -File vendor\build-deps.ps1   # Release

# build the solution
msbuild "Zima CLI.sln" /p:Configuration=Release /p:Platform=x64
```

Outputs land in `bin\Release\` (`zima-agent.exe`, `zima-cli.exe`).

### 2. Desktop GUI (Qt)

The GUI only needs libsodium (it opens no sockets — just the agent pipe), so you
don't need the full `build-deps.ps1` for it.

```bat
:: configure + build (Debug) and stage the Qt/MinGW runtime next to the exe
build-gui.bat
```

Run it from `build-qt\zima-gui-qt.exe`.

To produce a **distributable** build (Release, self-contained, no loose DLLs to
manage):

```bat
package-gui.bat
```

This Release-builds, runs `windeployqt`, and emits:

- `dist\Zima\` — self-contained folder (exe + exactly the required Qt DLLs/plugins + MinGW runtime)
- `dist\Zima-win64.zip` — portable, unzip-and-run
- `dist\Zima-Setup.exe` — one-file self-extractor (needs 7-Zip)

> Qt here is a *shared* build, so a single static `.exe` isn't possible; the
> folder/zip/self-extractor is the supported way to ship it.

### 3. Cross-compile agent + CLI for Windows from Linux/macOS (optional)

```bash
make -f Makefile.mingw MINGW_SYSROOT=/path/to/winroot
```

Requires a mingw-w64 GCC and a sysroot with cross-built libsodium/OpenSSL/curl
(see the header of `Makefile.mingw`).

### Cleaning

```bat
clean.bat
```

Removes all build/packaging output (`build-qt`, `build-qt-release`, `dist`,
`build`, `bin`, `obj`, `.vs`) while leaving source and the vendored libraries
intact.

---

## Running

1. Start `zima-agent.exe` (the CLI auto-spawns it on POSIX; on Windows keep it
   running, or place it next to the client so the GUI can launch it).
2. Provide your Zima API key once — `zima login`, or **Settings → API key** in the GUI.
3. Use it:
   - CLI: `zima chat`, `zima ask "…"`, `zima models`, `zima status`.
   - GUI: open a folder to give the assistant file/shell access, pick a model, and chat.

> The agent serves **one client at a time** - close the GUI before using the CLI
> against the same agent, or vice-versa.

---

## License

This project is licensed under the **MIT License** - see [`LICENSE`](LICENSE).

### Third-party components

These retain their own licenses and are not relicensed by this project:

- **Qt 6** — used by the desktop GUI under the **LGPL v3**. The GUI links Qt
  dynamically; redistributed bundles (`dist/`) ship the Qt DLLs as separate
  files, satisfying LGPL's relinking requirement. Include Qt's license notice
  when redistributing.
- **libsodium** (ISC), **OpenSSL** (Apache-2.0), **curl** (curl license) —
  vendored as submodules under `vendor/src/`.


