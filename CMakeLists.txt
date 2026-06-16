cmake_minimum_required(VERSION 3.16)
project(ClipboardManager VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── Source files shared by all platforms ────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/ClipboardManager.cpp
)

# ─── Platform-specific sources ───────────────────────────────────────────────
if(APPLE)
    list(APPEND SOURCES src/platform/clipboard_mac.mm)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -x objective-c++")
elseif(WIN32)
    list(APPEND SOURCES src/platform/clipboard_win.cpp)
elseif(UNIX)
    list(APPEND SOURCES src/platform/clipboard_linux.cpp)
endif()

add_executable(clipboard-manager ${SOURCES})

target_include_directories(clipboard-manager PRIVATE src)

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
