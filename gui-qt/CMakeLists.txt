# ---------------------------------------------------------------------------
# Zima GUI (Qt 6) — build
# ---------------------------------------------------------------------------
# A Qt Widgets front end for the Zima agent. It reuses the existing RPC framing,
# session-token, and named-pipe transport from the agent/CLI, and links the
# MinGW-ABI libsodium shipped in C:\winlibs (the Qt kit on this machine is
# mingw_64, so the crypto lib must match that ABI).
#
# Configure + build (from a Developer/MinGW-aware shell):
#   cmake -S gui-qt -B build-qt -G Ninja ^
#         -DCMAKE_PREFIX_PATH=C:/Qt/6.11.1/mingw_64 ^
#         -DCMAKE_C_COMPILER=C:/Qt/Tools/mingw1310_64/bin/gcc.exe ^
#         -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe
#   cmake --build build-qt
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.21)
project(zima_gui_qt VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

# --- libsodium (MinGW static .a from C:\winlibs) ---
set(SODIUM_ROOT "C:/winlibs" CACHE PATH "Root of the MinGW libsodium (include/ + lib/)")
add_library(sodium STATIC IMPORTED)
set_target_properties(sodium PROPERTIES
    IMPORTED_LOCATION "${SODIUM_ROOT}/lib/libsodium.a"
    INTERFACE_INCLUDE_DIRECTORIES "${SODIUM_ROOT}/include")

# --- reused backend sources (shared with the agent/CLI) ---
set(REPO "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(BACKEND_SRCS
    "${REPO}/agent/rpc.cpp"
    "${REPO}/agent/session.cpp"
    "${REPO}/platform/windows/ipc_pipe.cpp")

qt_add_executable(zima-gui-qt WIN32
    main.cpp
    MainWindow.cpp
    AgentClient.cpp
    TranscriptView.cpp
    MessageWidget.cpp
    MeshBackground.cpp
    ComposerWidget.cpp
    EditorPanel.cpp
    WinChrome.cpp
    Tools.cpp
    Diff.cpp
    ConversationStore.cpp
    MemoryStore.cpp
    resources.qrc
    ${BACKEND_SRCS})

target_include_directories(zima-gui-qt PRIVATE "${REPO}" "${REPO}/agent")

target_compile_definitions(zima-gui-qt PRIVATE
    SODIUM_STATIC
    _WIN32_WINNT=0x0A00
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE _UNICODE)

target_link_libraries(zima-gui-qt PRIVATE
    Qt6::Widgets
    sodium
    ws2_32 advapi32 bcrypt crypt32 userenv dwmapi)

# Copy the .qss next to the binary is unnecessary — it's embedded via qrc.
