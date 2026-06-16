// clipboard_win.cpp  –  Windows implementation using the Win32 API

#ifdef _WIN32

#include "ClipboardManager.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

std::string ClipboardManager::readClipboard()
{
    // OpenClipboard(nullptr) opens the clipboard for examination.
    // Pass nullptr (no window handle) since we are a console/background app.
    if (!OpenClipboard(nullptr)) {
        return {};
    }

    // CF_UNICODETEXT is the standard format for Unicode text on Windows.
    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
    if (hData == nullptr) {
        CloseClipboard();
        return {};
    }

    // GlobalLock gives us a raw pointer into the clipboard memory block.
    // We must call GlobalUnlock when done.
    wchar_t* pText = static_cast<wchar_t*>(GlobalLock(hData));
    if (pText == nullptr) {
        CloseClipboard();
        return {};
    }

    // Convert wide string (UTF-16) to a UTF-8 std::string.
    int sizeNeeded = WideCharToMultiByte(
        CP_UTF8, 0, pText, -1,
        nullptr, 0, nullptr, nullptr);

    std::string result(sizeNeeded - 1, '\0');   // -1 to exclude null terminator
    WideCharToMultiByte(
        CP_UTF8, 0, pText, -1,
        result.data(), sizeNeeded, nullptr, nullptr);

    GlobalUnlock(hData);
    CloseClipboard();

    return result;
}

#endif // _WIN32
