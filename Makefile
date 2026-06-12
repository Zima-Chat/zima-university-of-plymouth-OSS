CXX      ?= c++
STD      := -std=c++20
WARN     := -Wall -Wextra -Wpedantic
INCLUDES := -I.

# ---------- Detect platform ----------
ifeq ($(OS),Windows_NT)
  PLATFORM := windows
else
  UNAME_S := $(shell uname -s)
  ifeq ($(UNAME_S),Darwin)
    PLATFORM := macos
  else
    PLATFORM := linux
  endif
endif

# ---------- Security compiler flags (§10.1) ----------
ifeq ($(PLATFORM),macos)
  SEC_FLAGS := -O2 -fstack-protector-strong -fPIE
  LDFLAGS_SEC := -Wl,-pie
  # libsodium via pkg-config / Homebrew.
  SODIUM_FLAGS  := $(shell pkg-config --cflags libsodium 2>/dev/null || echo "-I/opt/homebrew/include")
  SODIUM_LIBS   := $(shell pkg-config --libs   libsodium 2>/dev/null || echo "-L/opt/homebrew/lib -lsodium")
  #
  # libcurl MUST be the Homebrew build (OpenSSL backend), NOT the system curl
  # (SecureTransport). SecureTransport does not support CURLOPT_PINNEDPUBLICKEY
  # or CURL_SSLVERSION_TLSv1_3. Homebrew's curl is keg-only; try both arches.
  # Install: brew install curl
  BREW_CURL_X86 := /usr/local/opt/curl/lib/pkgconfig
  BREW_CURL_ARM := /opt/homebrew/opt/curl/lib/pkgconfig
  BREW_CURL_PC  := $(shell [ -d $(BREW_CURL_X86) ] && echo $(BREW_CURL_X86) || echo $(BREW_CURL_ARM))
  CURL_FLAGS    := $(shell PKG_CONFIG_PATH="$(BREW_CURL_PC)" pkg-config --cflags libcurl 2>/dev/null)
  CURL_LIBS     := $(shell PKG_CONFIG_PATH="$(BREW_CURL_PC)" pkg-config --libs   libcurl 2>/dev/null)
  ifeq ($(CURL_LIBS),)
    $(error Homebrew curl not found. Run: brew install curl)
  endif
  PLATFORM_AGENT_SRCS := platform/posix/hardening_posix.cpp \
                          platform/posix/hardening_macos.cpp \
                          platform/posix/secret_store_keychain.cpp \
                          platform/posix/ipc_unix.cpp
  PLATFORM_CLI_SRCS   := platform/posix/ipc_unix.cpp
  PLATFORM_LIBS       := -framework Security -framework CoreFoundation
  PLATFORM_DEFINES    :=

else ifeq ($(PLATFORM),linux)
  SEC_FLAGS := -O2 -fstack-protector-strong -fstack-clash-protection \
               -fcf-protection=full -D_FORTIFY_SOURCE=2 -fPIE
  LDFLAGS_SEC := -pie -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack
  SODIUM_FLAGS  := $(shell pkg-config --cflags libsodium 2>/dev/null)
  SODIUM_LIBS   := $(shell pkg-config --libs   libsodium 2>/dev/null || echo "-lsodium")
  CURL_FLAGS    := $(shell pkg-config --cflags libcurl   2>/dev/null)
  CURL_LIBS     := $(shell pkg-config --libs   libcurl   2>/dev/null || echo "-lcurl")
  PLATFORM_AGENT_SRCS := platform/posix/hardening_posix.cpp \
                          platform/posix/hardening_linux.cpp \
                          platform/posix/secret_store_libsecret.cpp \
                          platform/posix/secret_store_keyring.cpp \
                          platform/posix/ipc_unix.cpp
  PLATFORM_CLI_SRCS   := platform/posix/ipc_unix.cpp
  PLATFORM_FLAGS      := $(shell pkg-config --cflags libsecret-1 2>/dev/null)
  PLATFORM_LIBS       := $(shell pkg-config --libs libsecret-1 2>/dev/null || echo "-lsecret-1 -lgio-2.0 -lglib-2.0") \
                          -lkeyutils
  PLATFORM_DEFINES    :=

else  # windows
  SEC_FLAGS   := /O2 /GS /guard:cf /guard:ehcont /Qspectre /sdl
  LDFLAGS_SEC := /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /CETCOMPAT
  SODIUM_FLAGS  :=
  SODIUM_LIBS   := sodium.lib
  CURL_FLAGS    :=
  CURL_LIBS     := libcurl.lib
  PLATFORM_AGENT_SRCS := platform/windows/hardening_win.cpp \
                          platform/windows/secret_store_credman.cpp \
                          platform/windows/ipc_pipe.cpp
  PLATFORM_CLI_SRCS   := platform/windows/ipc_pipe.cpp
  PLATFORM_LIBS       :=
  PLATFORM_DEFINES    :=
endif

CXXFLAGS := $(STD) $(WARN) $(INCLUDES) $(SEC_FLAGS) \
             $(SODIUM_FLAGS) $(CURL_FLAGS) $(PLATFORM_FLAGS) $(PLATFORM_DEFINES)
# ---------- Agent sources ----------
AGENT_SRCS := agent/main.cpp \
              agent/session.cpp \
              agent/rpc.cpp \
              agent/zima_client.cpp \
              agent/prompt_scanner.cpp \
              $(PLATFORM_AGENT_SRCS)

AGENT_OBJS := $(AGENT_SRCS:.cpp=.o)

# ---------- CLI sources ----------
CLI_SRCS := cli/main.cpp \
            cli/terminal_input.cpp \
            agent/rpc.cpp \
            agent/session.cpp \
            $(PLATFORM_CLI_SRCS)

CLI_OBJS := $(CLI_SRCS:.cpp=.o)

# ---------- Targets ----------
AGENT_BIN := zima-agent
CLI_BIN   := zima

.PHONY: all clean

all: $(AGENT_BIN) $(CLI_BIN)

$(AGENT_BIN): $(AGENT_OBJS)
	$(CXX) $(LDFLAGS_SEC) $^ -o $@ $(SODIUM_LIBS) $(CURL_LIBS) $(PLATFORM_LIBS)

$(CLI_BIN): $(CLI_OBJS)
	$(CXX) $(LDFLAGS_SEC) $^ -o $@ $(SODIUM_LIBS) $(PLATFORM_LIBS)

# Compile rule
%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(AGENT_BIN) $(CLI_BIN) \
	      $(AGENT_OBJS) $(CLI_OBJS) \
	      core.o posix.o windows.o
