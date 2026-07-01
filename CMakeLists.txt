cmake_minimum_required(VERSION 3.16)
project(ClipboardManager VERSION 0.1.0 LANGUAGES CXX)

# Objective-C++ is only needed on macOS (for clipboard_mac.mm). Enabling it
# unconditionally breaks the Linux build, where GCC ships no Objective-C++
# frontend (cc1objplus). Enable it only where it is actually used.
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ─── Core library (everything except main()) ─────────────────────────────────
# Factored into a static library so both the application and the test
# executables link the same compiled objects (compile once, reuse).
set(CORE_SOURCES
    src/ClipboardManager.cpp
    src/platform/paths.cpp
)

if(APPLE)
    list(APPEND CORE_SOURCES src/platform/clipboard_mac.mm src/platform/service_unix.cpp src/platform/hotkey_mac.mm src/platform/window_mac.mm)
elseif(WIN32)
    list(APPEND CORE_SOURCES src/platform/clipboard_win.cpp src/platform/service_win.cpp)
elseif(UNIX)
    list(APPEND CORE_SOURCES src/platform/clipboard_linux.cpp src/platform/service_unix.cpp src/platform/hotkey_linux.cpp src/platform/window_linux.cpp)
endif()

add_library(clipboard_core STATIC ${CORE_SOURCES})

# PUBLIC so anything linking the core (app + tests) inherits the include paths.
target_include_directories(clipboard_core PUBLIC src src/platform)

# Platform link dependencies — PUBLIC so consumers inherit them too.
if(APPLE)
    # Carbon: RegisterEventHotKey for the global popup hotkey (hotkey_mac.mm).
    # See that file's header comment for why Carbon over the NSEvent global
    # monitor (no Accessibility permission prompt needed).
    # ApplicationServices: AXIsProcessTrustedWithOptions (Accessibility
    # permission check/prompt) and CGEventPost (synthesized paste keystroke),
    # both used by window_mac.mm's auto-paste-on-select.
    target_link_libraries(clipboard_core PUBLIC
        "-framework Cocoa"
        "-framework AppKit"
        "-framework Carbon"
        "-framework ApplicationServices"
    )
elseif(WIN32)
    # user32: clipboard APIs.  ws2_32: Winsock, used by service_win.cpp for the
    # local IPC socket. ws2_32 must be declared here rather than relying on the
    # MSVC-only `#pragma comment(lib, "Ws2_32.lib")` in service_win.cpp, which
    # other toolchains (MinGW/GCC, Clang) silently ignore.
    target_link_libraries(clipboard_core PUBLIC user32 ws2_32)
elseif(UNIX)
    # Requires libx11-dev, libxfixes-dev, libxtst-dev, libgtk-3-dev, and
    # pkg-config on Debian/Ubuntu.
    #
    # X11/Xfixes: clipboard polling (clipboard_linux.cpp) and the global
    # hotkey grab (hotkey_linux.cpp).
    # Xtst: XTestFakeKeyEvent, the synthesized paste keystroke used by
    # window_linux.cpp's auto-paste-on-select (the X11 analogue of macOS's
    # CGEventPost — see that file's header comment for why no permission
    # gate is needed here, unlike macOS's Accessibility prompt).
    # GTK3: the popup window itself (window_linux.cpp) — see that file's
    # header comment for why a toolkit is used here rather than raw Xlib
    # widgets, unlike the "native per-OS" approach on macOS/Windows.
    find_package(X11 REQUIRED)
    find_library(XTST_LIBRARY Xtst REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GTK3 REQUIRED gtk+-3.0)

    target_link_libraries(clipboard_core PUBLIC X11::X11 X11::Xfixes ${XTST_LIBRARY} ${GTK3_LIBRARIES})
    target_include_directories(clipboard_core PUBLIC ${GTK3_INCLUDE_DIRS})
    target_compile_options(clipboard_core PUBLIC ${GTK3_CFLAGS_OTHER})
endif()

# ─── Compiler warnings (shared by all targets) ───────────────────────────────
if(MSVC)
    set(PROJECT_WARNINGS /W4)
else()
    set(PROJECT_WARNINGS -Wall -Wextra -Wpedantic)
endif()
target_compile_options(clipboard_core PRIVATE ${PROJECT_WARNINGS})

# ─── CLI11 (command-line parsing) ────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
    cli11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG        v2.4.2
)
FetchContent_MakeAvailable(cli11)

# ─── Application ─────────────────────────────────────────────────────────────
add_executable(clipboard-manager src/main.cpp)
target_link_libraries(clipboard-manager PRIVATE clipboard_core CLI11::CLI11)
target_compile_options(clipboard-manager PRIVATE ${PROJECT_WARNINGS})

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

# ─── Tests ───────────────────────────────────────────────────────────────────
# enable_testing() registers the suite with CTest; `ctest` (or the CI step)
# then runs every add_test() target. The race regression test relies on its
# per-test TIMEOUT: if the start/stop race ever returns, a join() hangs and
# CTest fails the test instead of letting it block forever.
enable_testing()

add_executable(race_regression_test tests/race_regression_test.cpp)
target_link_libraries(race_regression_test PRIVATE clipboard_core)
target_compile_options(race_regression_test PRIVATE ${PROJECT_WARNINGS})

add_test(NAME race_regression COMMAND race_regression_test)
# Generous cap: the test completes in seconds when healthy; if it hangs (the
# bug regressed) CTest kills it here and reports failure.
set_tests_properties(race_regression PROPERTIES TIMEOUT 120)

add_executable(paste_entry_by_content_test tests/paste_entry_by_content_test.cpp)
target_link_libraries(paste_entry_by_content_test PRIVATE clipboard_core)
target_compile_options(paste_entry_by_content_test PRIVATE ${PROJECT_WARNINGS})

add_test(NAME paste_entry_by_content COMMAND paste_entry_by_content_test)
set_tests_properties(paste_entry_by_content PROPERTIES TIMEOUT 30)
