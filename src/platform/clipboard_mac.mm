// clipboard_mac.mm  –  macOS implementation using NSPasteboard
//
// This file uses Objective-C++ (.mm) so we can call Apple's Cocoa APIs
// directly from C++ code.  The rest of the project is plain C++.

#include "ClipboardManager.h"

#import <Cocoa/Cocoa.h>   // gives us NSPasteboard and NSString

std::string ClipboardManager::readClipboard()
{
    // NSPasteboard is the macOS system clipboard object.
    // +generalPasteboard returns the shared instance (no need to release it).
    NSPasteboard* pb = [NSPasteboard generalPasteboard];

    // stringForType: returns the clipboard's plain-text content,
    // or nil if there is no text on the clipboard right now.
    NSString* text = [pb stringForType:NSPasteboardTypeString];

    if (text == nil) {
        return {};          // empty string — no text on clipboard
    }

    // Convert NSString (Objective-C) to std::string (C++).
    return std::string([text UTF8String]);
}
