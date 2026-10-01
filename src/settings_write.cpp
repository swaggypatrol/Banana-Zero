// Writing dlssnr.ini back: see settings_write.h.

#include "settings_write.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <type_traits>

#include "keys.h"
#include "settings.h"

namespace
{
const Settings g_defaults;

// One key the menu owns, and how its value is written. `line` is filled by Value(); `present` says whether the
// value differs from the default (or, for a model parameter, is set at all).
struct Key
{
    const char* name;
    const char* alternative; // a second spelling that counts as the same key when read
    bool remove;             // true: no value, take the line out
    char value[32];
};

void Float(char* out, size_t size, float value)
{
    // Short and exact enough to come back the same: 4 significant digits cover every slider's steps.
    snprintf(out, size, "%.4g", double(value));
}

template <typename T> void Tunable(Key* key, const char* name, const ::Tunable<T>& t)
{
    key->name = name;
    key->alternative = nullptr;
    key->remove = !t.set;
    if constexpr (std::is_same_v<T, float>)
        Float(key->value, sizeof key->value, t.value);
    else
        snprintf(key->value, sizeof key->value, "%u", unsigned(t.value));
}

const char* const kInputText[] = { "auto", "linear", "tonemapped" };
const char* const kWhiteText[] = { "exposure", "manual", "scene", "auto" };
const char* const kPreviewText[] = { "off", "input", "output" };
const char* const kCornerText[] = { "topleft", "topright", "bottomleft", "bottomright" };
const char* const kMenuColourText[] = { "auto", "sdr", "hdr" };
const char* const kMenuThemeText[] = { "dark", "paper" };

// The keys in the order new lines are added. `wanted` is set for a key whose line should exist: a value away from
// the default, or a model parameter that is set. Room for the 33 keys MakePlan adds and a few more: nothing checks.
constexpr unsigned kMaxKeys = 40;
struct Plan
{
    Key keys[kMaxKeys];
    bool wanted[kMaxKeys];
    unsigned count;
};

void Add(Plan* plan, const char* name, const char* alternative, bool wanted, const char* value)
{
    Key& k = plan->keys[plan->count];
    k.name = name;
    k.alternative = alternative;
    k.remove = false;
    snprintf(k.value, sizeof k.value, "%s", value);
    plan->wanted[plan->count] = wanted;
    ++plan->count;
}

void AddBool(Plan* plan, const char* name, bool value, bool fallback)
{
    Add(plan, name, nullptr, value != fallback, value ? "1" : "0");
}

void AddFloat(Plan* plan, const char* name, float value, float fallback)
{
    char text[32];
    Float(text, sizeof text, value);
    Add(plan, name, nullptr, std::fabs(value - fallback) > 1.0e-6f, text);
}

void AddChoice(Plan* plan, const char* name, const char* alternative, unsigned value, unsigned fallback,
               const char* const* names, unsigned count)
{
    Add(plan, name, alternative, value != fallback, names[value < count ? value : 0]);
}

void AddKey(Plan* plan, const char* name, unsigned vk, unsigned fallback)
{
    char text[16];
    Add(plan, name, nullptr, vk != fallback, KeyName(vk, text, sizeof text));
}

template <typename T> void AddTunable(Plan* plan, const char* name, const ::Tunable<T>& t)
{
    Key& k = plan->keys[plan->count];
    Tunable(&k, name, t);
    plan->wanted[plan->count] = t.set;
    ++plan->count;
}

Plan MakePlan(const Settings& s)
{
    const Settings& d = g_defaults;
    Plan plan = {};
    AddBool(&plan, "Enabled", s.enabled, d.enabled);
    AddFloat(&plan, "Badge", s.badgeSeconds, d.badgeSeconds);
    AddBool(&plan, "Compare", s.compare, d.compare);
    AddFloat(&plan, "CompareSplit", s.compareSplit, d.compareSplit);
    AddTunable(&plan, "Preset", s.model.preset);
    AddTunable(&plan, "Intensity", s.model.intensity);
    AddTunable(&plan, "Style", s.model.style);
    AddTunable(&plan, "LocalStructure", s.model.localStructure);
    AddTunable(&plan, "LocalTone", s.model.localTone);
    AddTunable(&plan, "SkinStructure", s.model.skinStructure);
    AddTunable(&plan, "AutoMask", s.model.autoMask);
    AddChoice(&plan, "InputType", nullptr, unsigned(s.inputType), unsigned(d.inputType), kInputText, 3);
    AddChoice(&plan, "WhiteSource", nullptr, unsigned(s.whiteSource), unsigned(d.whiteSource), kWhiteText, 4);
    AddFloat(&plan, "WhiteEV", s.whiteEV, d.whiteEV);
    AddFloat(&plan, "Shoulder", s.shoulder, d.shoulder);
    AddFloat(&plan, "DetailStrength", s.detailStrength, d.detailStrength);
    AddFloat(&plan, "ColourStrength", s.colourStrength, d.colourStrength);
    plan.keys[plan.count - 1].alternative = "ColorStrength";
    AddFloat(&plan, "MaxGainEV", s.maxGainEV, d.maxGainEV);
    AddFloat(&plan, "HighlightRestore", s.highlightRestore, d.highlightRestore);
    AddChoice(&plan, "Preview", nullptr, unsigned(s.preview), unsigned(d.preview), kPreviewText, 3);
    AddBool(&plan, "Zebra", s.zebra, d.zebra);
    AddChoice(&plan, "CardCorner", nullptr, unsigned(s.cardCorner), unsigned(d.cardCorner), kCornerText, 4);
    // The card is not stored: a line the file already has is kept true to what the menu shows.
    AddBool(&plan, "Card", s.card, d.card);
    plan.wanted[plan.count - 1] = false;
    AddKey(&plan, "MenuKey", s.menuKey, d.menuKey);
    AddKey(&plan, "ToggleKey", s.toggleKey, d.toggleKey);
    AddKey(&plan, "FreezeKey", s.freezeKey, d.freezeKey);
    AddChoice(&plan, "MenuColour", "MenuColor", unsigned(s.menuColour), unsigned(d.menuColour), kMenuColourText, 3);
    AddFloat(&plan, "MenuNits", s.menuNits, d.menuNits);
    AddChoice(&plan, "MenuTheme", nullptr, unsigned(s.menuTheme), unsigned(d.menuTheme), kMenuThemeText, 2);
    AddBool(&plan, "DilateMotion", s.dilateMotion, d.dilateMotion);
    AddFloat(&plan, "SkyTone", s.skyTone, d.skyTone);
    AddFloat(&plan, "SkyStructure", s.skyStructure, d.skyStructure);
    // The sky's stripes are not stored either, like the card.
    AddBool(&plan, "ShowSky", s.showSky, d.showSky);
    plan.wanted[plan.count - 1] = false;
    return plan;
}

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// The key of a `key = value` line, without its whitespace, or 0 length when the line is not one. `keyStart` and
// `keyLength` say where the key is; `valueStart` where the value begins (after `=` and its blanks).
size_t LineKey(const char* line, size_t length, size_t* keyStart, size_t* valueStart)
{
    size_t i = 0;
    while (i < length && IsSpace(line[i]))
        ++i;
    if (i >= length || line[i] == ';' || line[i] == '#' || line[i] == '[')
        return 0;
    *keyStart = i;
    while (i < length && line[i] != '=' && line[i] != ';' && line[i] != '#')
        ++i;
    if (i >= length || line[i] != '=')
        return 0;
    size_t keyEnd = i;
    while (keyEnd > *keyStart && IsSpace(line[keyEnd - 1]))
        --keyEnd;
    ++i;
    while (i < length && (line[i] == ' ' || line[i] == '\t'))
        ++i;
    *valueStart = i;
    return keyEnd - *keyStart;
}

bool SameKey(const Key& key, const char* text, size_t length)
{
    if (strlen(key.name) == length && _strnicmp(key.name, text, length) == 0)
        return true;
    return key.alternative != nullptr && strlen(key.alternative) == length &&
           _strnicmp(key.alternative, text, length) == 0;
}

struct Out
{
    char* text;
    size_t length;
    size_t capacity;
};

void Put(Out* out, const char* text, size_t length)
{
    if (out->length + length > out->capacity)
        length = out->capacity - out->length;
    memcpy(out->text + out->length, text, length);
    out->length += length;
}

void PutLine(Out* out, const Key& key, const char* eol, const char* comment, size_t commentLength)
{
    Put(out, key.name, strlen(key.name));
    Put(out, " = ", 3);
    Put(out, key.value, strlen(key.value));
    if (commentLength != 0)
    {
        Put(out, "  ", 2);
        Put(out, comment, commentLength);
    }
    Put(out, eol, strlen(eol));
}
} // namespace

char* SettingsMergeIni(const char* text, const Settings& settings, size_t* length)
{
    Plan plan = MakePlan(settings);
    const size_t textLength = text != nullptr ? strlen(text) : 0;
    Out out = {};
    out.capacity = textLength + 128 + size_t(plan.count) * 96;
    out.text = new (std::nothrow) char[out.capacity + 1];
    if (out.text == nullptr)
        return nullptr;

    // The file's own line ending: the first one found; CRLF when there is none (a new file, or one line).
    const char* eol = "\r\n";
    if (const char* first = text != nullptr ? strchr(text, '\n') : nullptr)
        eol = first > text && first[-1] == '\r' ? "\r\n" : "\n";

    bool seen[kMaxKeys] = {};
    if (text == nullptr)
    {
        const char* header = "; Banana-Zero dlssnr.ini: the in-game menu writes the keys it changed; the rest stay at "
                             "their defaults (every key is in the README).";
        Put(&out, header, strlen(header));
        Put(&out, eol, strlen(eol));
    }
    for (size_t at = 0; at < textLength;)
    {
        const char* line = text + at;
        const char* newline = static_cast<const char*>(memchr(line, '\n', textLength - at));
        const size_t lineLength = newline != nullptr ? size_t(newline - line) + 1 : textLength - at;
        at += lineLength;
        // The line's content without its line ending, and any comment after the value.
        size_t content = lineLength;
        while (content > 0 && (line[content - 1] == '\n' || line[content - 1] == '\r'))
            --content;
        size_t keyStart = 0, valueStart = 0;
        const size_t keyLength = LineKey(line, content, &keyStart, &valueStart);
        unsigned index = plan.count;
        for (unsigned i = 0; keyLength != 0 && i < plan.count; ++i)
        {
            if (SameKey(plan.keys[i], line + keyStart, keyLength))
            {
                index = i;
                break;
            }
        }
        if (index == plan.count)
        {
            Put(&out, line, lineLength); // not ours: as it is
            continue;
        }
        const Key& key = plan.keys[index];
        if (seen[index] || key.remove)
            continue; // a second line for the same key, or a model parameter back at its default: gone
        seen[index] = true;
        const char* comment = nullptr;
        size_t commentLength = 0;
        for (size_t i = valueStart; i < content; ++i)
        {
            if (line[i] == ';' || line[i] == '#')
            {
                comment = line + i;
                commentLength = content - i;
                break;
            }
        }
        PutLine(&out, key, eol, comment, commentLength);
    }
    // Keys that have no line yet and want one, after the file's last line.
    bool any = false;
    for (unsigned i = 0; i < plan.count; ++i)
    {
        if (seen[i] || !plan.wanted[i] || plan.keys[i].remove)
            continue;
        if (!any && out.length > 0 && out.text[out.length - 1] != '\n')
            Put(&out, eol, strlen(eol));
        any = true;
        PutLine(&out, plan.keys[i], eol, nullptr, 0);
    }
    out.text[out.length] = '\0';
    *length = out.length;
    return out.text;
}

bool SettingsWriteIni(const wchar_t* path, const Settings& settings, unsigned long* error)
{
    *error = 0;
    char* text = nullptr;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, 0, nullptr);
    if (file != INVALID_HANDLE_VALUE)
    {
        LARGE_INTEGER size = {};
        if (GetFileSizeEx(file, &size) && size.QuadPart < 1 << 20)
        {
            text = new (std::nothrow) char[size_t(size.QuadPart) + 1];
            DWORD read = 0;
            if (text != nullptr && ReadFile(file, text, DWORD(size.QuadPart), &read, nullptr))
                text[read] = '\0';
            else
            {
                delete[] text;
                text = nullptr;
            }
        }
        CloseHandle(file);
        if (text == nullptr)
        {
            *error = ERROR_READ_FAULT; // a file that exists but cannot be read is left alone
            return false;
        }
    }
    else if (GetLastError() != ERROR_FILE_NOT_FOUND)
    {
        *error = GetLastError();
        return false;
    }

    size_t length = 0;
    char* merged = SettingsMergeIni(text, settings, &length);
    delete[] text;
    if (merged == nullptr)
    {
        *error = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }

    // Written beside the file, then moved over it: the old file stays whole if anything goes wrong midway.
    wchar_t temporary[MAX_PATH];
    if (wcslen(path) + 5 >= MAX_PATH)
    {
        delete[] merged;
        *error = ERROR_FILENAME_EXCED_RANGE;
        return false;
    }
    wcscpy_s(temporary, MAX_PATH, path);
    wcscat_s(temporary, MAX_PATH, L".new");
    file = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        *error = GetLastError();
        delete[] merged;
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(file, merged, DWORD(length), &written, nullptr) && written == length;
    if (!ok)
        *error = GetLastError();
    CloseHandle(file);
    delete[] merged;
    if (!ok)
    {
        DeleteFileW(temporary);
        return false;
    }
    // The replace fails while someone has the file open (the reload thread reading it, an editor): a few more tries.
    for (unsigned attempt = 0;; ++attempt)
    {
        if (MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        *error = GetLastError();
        if (attempt == 10)
            break;
        Sleep(20);
    }
    DeleteFileW(temporary);
    return false;
}

size_t SettingsDiff(const Settings& before, const Settings& after, char* out, size_t size)
{
    const Plan a = MakePlan(before);
    const Plan b = MakePlan(after);
    size_t length = 0;
    out[0] = '\0';
    for (unsigned i = 0; i < b.count && length + 1 < size; ++i)
    {
        const Key& x = a.keys[i];
        const Key& y = b.keys[i];
        if (x.remove == y.remove && strcmp(x.value, y.value) == 0)
            continue;
        const int n = snprintf(out + length, size - length, "%s%s %s -> %s", length != 0 ? ", " : "", y.name,
                               x.remove ? "(model default)" : x.value, y.remove ? "(model default)" : y.value);
        if (n < 0)
            break;
        length += size_t(n) < size - length ? size_t(n) : size - length - 1;
    }
    return length;
}
