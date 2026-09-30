#pragma once

// Key names for dlssnr.ini and the menu (MenuKey, ToggleKey, FreezeKey): a virtual-key code as a word
// ("End", "F9", "Numpad5", "A", "None"), or "0x" and its hex code for a key without a name. Matching ignores case.

#include <cstddef>

// "End", "F9", ..., or "0x2E" for a code without a name; "None" for 0. Returns `out`.
const char* KeyName(unsigned vk, char* out, size_t size);

// The reverse. False (and *vk untouched) when the text is neither a known name nor "0x" and hex.
bool KeyFromName(const char* text, unsigned* vk);
