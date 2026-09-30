// Key names: see keys.h.

#include "keys.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
struct Named
{
    unsigned vk;
    const char* name;
};

// The first name of a code is the one written; the others are accepted when read.
const Named kNamed[] = {
    { 0, "None" },
    { VK_END, "End" },
    { VK_HOME, "Home" },
    { VK_INSERT, "Insert" },
    { VK_DELETE, "Delete" },
    { VK_DELETE, "Del" },
    { VK_PRIOR, "PageUp" },
    { VK_NEXT, "PageDown" },
    { VK_PAUSE, "Pause" },
    { VK_SCROLL, "ScrollLock" },
    { VK_SNAPSHOT, "PrintScreen" },
    { VK_BACK, "Backspace" },
    { VK_TAB, "Tab" },
    { VK_SPACE, "Space" },
    { VK_RETURN, "Enter" },
    { VK_CAPITAL, "CapsLock" },
    { VK_NUMLOCK, "NumLock" },
    { VK_APPS, "Menu" },
    { VK_LEFT, "Left" },
    { VK_RIGHT, "Right" },
    { VK_UP, "Up" },
    { VK_DOWN, "Down" },
    { VK_MULTIPLY, "NumpadMultiply" },
    { VK_ADD, "NumpadPlus" },
    { VK_SUBTRACT, "NumpadMinus" },
    { VK_DECIMAL, "NumpadDecimal" },
    { VK_DIVIDE, "NumpadDivide" },
    { VK_OEM_3, "Grave" },
    { VK_OEM_MINUS, "Minus" },
    { VK_OEM_PLUS, "Equals" },
    { VK_OEM_4, "LBracket" },
    { VK_OEM_6, "RBracket" },
    { VK_OEM_5, "Backslash" },
    { VK_OEM_1, "Semicolon" },
    { VK_OEM_7, "Quote" },
    { VK_OEM_COMMA, "Comma" },
    { VK_OEM_PERIOD, "Period" },
    { VK_OEM_2, "Slash" },
};
} // namespace

const char* KeyName(unsigned vk, char* out, size_t size)
{
    for (const Named& n : kNamed)
    {
        if (n.vk == vk)
        {
            snprintf(out, size, "%s", n.name);
            return out;
        }
    }
    if (vk >= VK_F1 && vk <= VK_F24)
        snprintf(out, size, "F%u", vk - VK_F1 + 1);
    else if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        snprintf(out, size, "Numpad%u", vk - VK_NUMPAD0);
    else if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
        snprintf(out, size, "%c", char(vk));
    else
        snprintf(out, size, "0x%02X", vk & 0xFF);
    return out;
}

bool KeyFromName(const char* text, unsigned* vk)
{
    for (const Named& n : kNamed)
    {
        if (_stricmp(text, n.name) == 0)
        {
            *vk = n.vk;
            return true;
        }
    }
    const size_t length = strlen(text);
    if (length == 1)
    {
        const char c = char(toupper(static_cast<unsigned char>(text[0])));
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))
        {
            *vk = unsigned(c);
            return true;
        }
        return false;
    }
    char* end = nullptr;
    if ((text[0] == 'F' || text[0] == 'f') && length <= 3)
    {
        const unsigned long number = strtoul(text + 1, &end, 10);
        if (end != text + 1 && *end == '\0' && number >= 1 && number <= 24)
        {
            *vk = VK_F1 + unsigned(number) - 1;
            return true;
        }
        return false;
    }
    if (_strnicmp(text, "Numpad", 6) == 0 && length == 7 && text[6] >= '0' && text[6] <= '9')
    {
        *vk = VK_NUMPAD0 + unsigned(text[6] - '0');
        return true;
    }
    if (length > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
    {
        const unsigned long number = strtoul(text + 2, &end, 16);
        if (end != text + 2 && *end == '\0' && number <= 0xFE)
        {
            *vk = unsigned(number);
            return true;
        }
    }
    return false;
}
