/**
 * @file clipboard_mac.mm
 * @brief macOS clipboard reader using NSPasteboard (Objective-C++).
 *
 * The .mm extension tells the compiler to treat this file as Objective-C++,
 * which allows C++ and Objective-C syntax to be freely mixed in the same
 * translation unit. The rest of the project is plain C++ (.cpp); only this
 * file and its Windows/Linux counterparts are platform-specific.
 *
 * Linked against the Cocoa and AppKit frameworks via CMakeLists.txt.
 */

#include "ClipboardManager.h"

// Cocoa is Apple's macOS application framework. Importing it gives us access
// to NSPasteboard (the clipboard API) and NSString (Apple's string class).
#import <Cocoa/Cocoa.h>

/**
 * @brief Read the current plain-text content of the macOS clipboard.
 *
 * NSPasteboard is the macOS system clipboard abstraction. It can hold
 * multiple data types simultaneously (text, images, files, etc.). We ask
 * specifically for NSPasteboardTypeString (plain UTF-8/UTF-16 text).
 *
 * Memory management note: Objective-C objects on modern macOS are managed
 * by ARC (Automatic Reference Counting). We do not need to call retain/release
 * manually — the compiler inserts those calls for us.
 *
 * @return The clipboard text as a std::string, or an empty string if the
 *         clipboard holds no plain text or is otherwise unavailable.
 */
std::string ClipboardManager::readClipboard()
{
    // [NSPasteboard generalPasteboard] returns the shared system clipboard.
    // This is a class method call ("+method" in Obj-C notation) that returns
    // a singleton — there is only one system clipboard, so no need to alloc/init.
    NSPasteboard* pb = [NSPasteboard generalPasteboard];

    // [pb stringForType:] asks the clipboard for its text content.
    // Returns nil if the clipboard holds no text (e.g., an image was copied).
    // NSPasteboardTypeString is the standard type identifier for plain text.
    NSString* text = [pb stringForType:NSPasteboardTypeString];

    if (text == nil) {
        // Return an empty std::string — the caller treats empty as "nothing new".
        return {};
    }

    // [text UTF8String] returns a const char* pointing to a null-terminated
    // UTF-8 byte sequence owned by the NSString. Passing it to the
    // std::string constructor copies the bytes into a new heap allocation,
    // so we no longer depend on the NSString's lifetime after this line.
    return std::string([text UTF8String]);
}

void ClipboardManager::writeClipboard(const std::string &text)
{
    NSPasteboard *pb = [NSPasteboard generalPasteboard];
    [pb clearContents];  // must clear before writing
    [pb setString:[NSString stringWithUTF8String:text.c_str()]
          forType:NSPasteboardTypeString];
}
