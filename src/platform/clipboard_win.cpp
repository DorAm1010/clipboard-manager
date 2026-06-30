/**
 * @file clipboard_win.cpp
 * @brief Windows clipboard reader using the Win32 API.
 *
 * Compiled only on Windows (guarded by `#ifdef _WIN32`). The CMake build
 * system also only adds this file to the source list on Windows, but the
 * preprocessor guard provides a second layer of safety.
 *
 * Win32 clipboard access is a three-step lock protocol:
 *   1. OpenClipboard()    — acquire exclusive access to the clipboard.
 *   2. GetClipboardData() — get a handle to the requested data format.
 *   3. GlobalLock()       — get a raw pointer to the clipboard memory block.
 *   ... read the data ...
 *   4. GlobalUnlock()     — release the memory pin.
 *   5. CloseClipboard()   — release the clipboard so other apps can use it.
 *
 * We must call CloseClipboard() on every code path — including error returns —
 * otherwise other processes cannot access the clipboard until our process exits.
 */

#ifdef _WIN32

#include "ClipboardManager.h"

// WIN32_LEAN_AND_MEAN strips rarely-used headers from windows.h, reducing
// compile time and avoiding macro collisions with standard C++ names.
#define WIN32_LEAN_AND_MEAN
#define ISATTY _isatty
#define STDOUT_FILENO 1

#include <windows.h>
#include <io.h>

/**
 * @brief Read the current plain-text content of the Windows clipboard.
 *
 * Windows stores clipboard text in UTF-16 (wide characters, `wchar_t`).
 * We convert to UTF-8 (`std::string`) using WideCharToMultiByte so the
 * rest of the program can work with a single string type everywhere.
 *
 * WideCharToMultiByte is called twice:
 *   - First call (with nullptr output buffer): measures how many UTF-8
 *     bytes the conversion will produce.
 *   - Second call (with a sized output buffer): performs the actual conversion.
 * This two-pass pattern is idiomatic Win32 for size-unknown conversions.
 *
 * @return The clipboard text as a UTF-8 std::string, or an empty string if
 *         the clipboard holds no text or cannot be opened.
 */
std::string ClipboardManager::readClipboard()
{
    // Acquire exclusive access to the clipboard.
    // We pass nullptr for the window handle because we are a console app
    // with no HWND. Only one process can have the clipboard open at a time;
    // if another app currently holds it, OpenClipboard() returns FALSE.
    if (!OpenClipboard(nullptr))
    {
        return {};
    }

    // Request the Unicode text format. CF_UNICODETEXT is the standard Win32
    // clipboard format for UTF-16 encoded text. Returns nullptr if no text
    // is currently on the clipboard (e.g., an image was copied).
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (hData == nullptr)
    {
        CloseClipboard(); // always release before returning
        return {};
    }

    // GlobalLock() pins the global memory block in place and returns a typed
    // pointer to its contents. The block is owned by the clipboard — we must
    // not free it. GlobalUnlock() releases the pin when we are done reading.
    wchar_t *pText = static_cast<wchar_t *>(GlobalLock(hData));
    if (pText == nullptr)
    {
        CloseClipboard();
        return {};
    }

    // ── UTF-16 → UTF-8 conversion ─────────────────────────────────────────
    // Pass 1: compute the required buffer size (in bytes, including the null
    // terminator). Passing -1 as the source length tells the function to
    // measure up to (and including) the null terminator automatically.
    int sizeNeeded = WideCharToMultiByte(
        CP_UTF8,  // target code page: UTF-8
        0,        // no special flags
        pText,    // source wide string
        -1,       // source length: auto-detect via null terminator
        nullptr,  // output buffer: nullptr → just measure
        0,        // output buffer size: 0 → just measure
        nullptr,  // default char (not used for UTF-8)
        nullptr); // used default char flag (not used for UTF-8)

    // Guard against a failed/zero-length conversion. WideCharToMultiByte
    // returns 0 on failure; computing `sizeNeeded - 1` in that case would
    // underflow to SIZE_MAX and request a gigantic std::string allocation
    // (crash). sizeNeeded includes the null terminator, so a valid result is
    // always >= 1; treat anything <= 0 as "no readable text".
    if (sizeNeeded <= 0)
    {
        GlobalUnlock(hData);
        CloseClipboard();
        return {};
    }

    // Allocate the result string and fill it.
    // sizeNeeded includes the null terminator; std::string manages its own
    // null terminator internally, so we subtract 1 to avoid a trailing '\0'
    // inside the string's character sequence.
    std::string result(sizeNeeded - 1, '\0');

    // Pass 2: perform the actual conversion into our pre-sized buffer.
    WideCharToMultiByte(
        CP_UTF8, 0, pText, -1,
        result.data(), // write directly into std::string's buffer (C++17)
        sizeNeeded,
        nullptr, nullptr);

    // Release resources in reverse acquisition order.
    GlobalUnlock(hData); // unpin the global memory block
    CloseClipboard();    // release the clipboard for other processes

    return result;
}

void ClipboardManager::writeClipboard(const std::string &text)
{
    if (!OpenClipboard(nullptr))
        return;
    EmptyClipboard();

    // Convert UTF-8 to UTF-16. MultiByteToWideChar returns 0 on failure; with
    // -1 as the source length the count includes the null terminator, so a
    // valid result is always >= 1. Bail on <= 0 rather than GlobalAlloc(0).
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (wideLen <= 0)
    {
        CloseClipboard();
        return;
    }

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, static_cast<size_t>(wideLen) * sizeof(wchar_t));
    if (!hMem)
    {
        CloseClipboard();
        return;
    }

    wchar_t *pMem = static_cast<wchar_t *>(GlobalLock(hMem));
    if (!pMem)
    {
        // Lock failed: we still own hMem (it was never handed to the clipboard),
        // so we must free it to avoid leaking the global allocation.
        GlobalFree(hMem);
        CloseClipboard();
        return;
    }
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, pMem, wideLen);
    GlobalUnlock(hMem);

    // On success, SetClipboardData transfers ownership of hMem to the system —
    // we must NOT free it. On failure ownership stays with us, so free it to
    // avoid a leak.
    if (!SetClipboardData(CF_UNICODETEXT, hMem))
    {
        GlobalFree(hMem);
    }
    CloseClipboard();
}

#endif // _WIN32
