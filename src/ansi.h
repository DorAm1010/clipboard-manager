#pragma once

#include <cstdio>

#ifdef _WIN32
#include <io.h>
#define ISATTY _isatty
#define STDOUT_FD 1
#else
#include <unistd.h>
#define ISATTY isatty
#define STDOUT_FD STDOUT_FILENO
#endif

namespace ansi
{
    inline bool supportsColor() { return ISATTY(STDOUT_FD); }

    inline const char *reset() { return supportsColor() ? "\033[0m" : ""; }

    constexpr const char *entryColors[] = {
        "\033[36m", // cyan
        "\033[33m", // yellow
        "\033[32m", // green
        "\033[35m", // magenta
    };
    constexpr size_t colorCount = sizeof(entryColors) / sizeof(entryColors[0]);

    inline const char *entryColor(size_t index)
    {
        if (!supportsColor())
            return "";
        return entryColors[index % colorCount];
    }
}