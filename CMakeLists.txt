cmake_minimum_required(VERSION 3.16)
project(ClipboardManager VERSION 0.1.0 LANGUAGES CXX OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── Source files shared by all platforms ────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/ClipboardManager.cpp
    src/platform/paths.cpp
)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─── Platform-specific sources ───────────────────────────────────────────────
if(APPLE)
    list(APPEND SOURCES src/platform/clipboard_mac.mm src/platform/service_unix.cpp)
elseif(WIN32)
    list(APPEND SOURCES src/platform/clipboard_win.cpp src/platform/service_win.cpp)
elseif(UNIX)
    list(APPEND SOURCES src/platform/clipboard_linux.cpp src/platform/service_unix.cpp)
endif()

# ─── CLI11 (command-line parsing) ────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
    cli11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG        v2.4.2
)
FetchContent_MakeAvailable(cli11)

add_executable(clipboard-manager ${SOURCES})
target_link_libraries(clipboard-manager CLI11::CLI11)

target_include_directories(clipboard-manager PRIVATE src src/platform)

# ─── Platform-specific linker flags ──────────────────────────────────────────
if(APPLE)
    target_link_libraries(clipboard-manager
        "-framework Cocoa"
        "-framework AppKit"
    )
elseif(WIN32)
    target_link_libraries(clipboard-manager user32)
elseif(UNIX)
    # Requires libx11-dev and libxfixes-dev on Debian/Ubuntu
    find_package(X11 REQUIRED)
    target_link_libraries(clipboard-manager X11::X11 X11::Xfixes)
endif()

# ─── Compiler warnings ───────────────────────────────────────────────────────
if(MSVC)
    target_compile_options(clipboard-manager PRIVATE /W4)
else()
    target_compile_options(clipboard-manager PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ─── Install target ──────────────────────────────────────────────────────────
# `cmake --install build` copies the compiled binary to the system PATH so the
# user can run `clipboard-manager` from any directory in their terminal.
#
# The default install prefix is:
#   /usr/local          on macOS and Linux  → binary lands in /usr/local/bin
#   C:/Program Files    on Windows          → binary lands in …/bin
#
# Override the prefix at configure time:
#   cmake -S . -B build -DCMAKE_INSTALL_PREFIX=/your/path
install(TARGETS clipboard-manager
        RUNTIME DESTINATION bin)   # RUNTIME = executables (.exe on Windows)
