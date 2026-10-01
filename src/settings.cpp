// Settings: see settings.h. The ini parser is our own: a line is `key = value`, `;` or `#` starts
// a comment, `[section]` lines are accepted and ignored, keys and word values are matched ignoring case. Anything
// else is logged and skipped; the file is never written here (that is the menu's job from M3 on).

#include "settings.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <type_traits>

#include "keys.h"
#include "log.h"

namespace
{
const Settings g_defaults;
std::atomic<const Settings*> g_current { &g_defaults };

// The file last read, for SettingsReloadIfChanged. Written by SettingsLoad only; the first load happens before the
// background thread that calls SettingsReloadIfChanged exists, every later one on that thread.
wchar_t g_path[MAX_PATH] = {};
FILETIME g_stamp = {};
ULONGLONG g_size = ~0ull;
unsigned g_generation = 0;
// Guards the generation counter and the retire list: SettingsLoad runs on the background thread, SettingsPublish
// (the menu, M3) on the present thread and the render thread.
SRWLOCK g_publishLock = SRWLOCK_INIT;

// Snapshots replaced by a reload or by the menu. An evaluation reads the pointer once and is done with it within a
// frame, so a replaced snapshot is freed once it has been out of use for kRetireSeconds. A dragged slider publishes
// a snapshot at every step, up to one a frame, hence the room for a few seconds of them.
constexpr double kRetireSeconds = 5.0;
struct Retired
{
    const Settings* settings = nullptr;
    double at = 0.0;
};
Retired g_retired[256];

void Retire(const Settings* old)
{
    if (old == &g_defaults || old == nullptr)
        return;
    const double now = LogClock();
    Retired* slot = nullptr;
    for (Retired& r : g_retired)
    {
        if (r.settings != nullptr && now - r.at > kRetireSeconds)
        {
            delete r.settings;
            r = {};
        }
        if (r.settings == nullptr && slot == nullptr)
            slot = &r;
    }
    if (slot != nullptr)
        *slot = { old, now };
    // All of them replaced within the last few seconds: keep the old one for ever rather than free it too soon.
}

// Reads a whole file, NUL-terminated; nullptr if it does not exist or cannot be read. Freed with delete[].
char* ReadFileText(const wchar_t* path, size_t* size)
{
    const HANDLE file =
        CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return nullptr;
    LARGE_INTEGER length = {};
    char* text = nullptr;
    if (GetFileSizeEx(file, &length) && length.QuadPart < 1 << 20)
    {
        text = new (std::nothrow) char[size_t(length.QuadPart) + 1];
        DWORD read = 0;
        if (text != nullptr && ReadFile(file, text, DWORD(length.QuadPart), &read, nullptr))
        {
            text[read] = '\0';
            *size = read;
        }
        else
        {
            delete[] text;
            text = nullptr;
        }
    }
    CloseHandle(file);
    return text;
}

// The file's time stamp and size; false (and zeroes) when it does not exist.
bool Stat(const wchar_t* path, FILETIME* stamp, ULONGLONG* size)
{
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data))
    {
        *stamp = {};
        *size = 0;
        return false;
    }
    *stamp = data.ftLastWriteTime;
    *size = (ULONGLONG(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    return true;
}

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Trims both ends in place; returns the start.
char* Trim(char* text)
{
    while (IsSpace(*text))
        ++text;
    size_t length = strlen(text);
    while (length > 0 && IsSpace(text[length - 1]))
        text[--length] = '\0';
    return text;
}

bool ParseBool(const char* value, bool* out)
{
    if (_stricmp(value, "1") == 0 || _stricmp(value, "true") == 0 || _stricmp(value, "on") == 0 ||
        _stricmp(value, "yes") == 0)
        *out = true;
    else if (_stricmp(value, "0") == 0 || _stricmp(value, "false") == 0 || _stricmp(value, "off") == 0 ||
             _stricmp(value, "no") == 0)
        *out = false;
    else
        return false;
    return true;
}

bool ParseFloat(const char* value, float* out)
{
    char* end = nullptr;
    const float parsed = strtof(value, &end);
    if (end == value || *end != '\0' || !std::isfinite(parsed))
        return false;
    *out = parsed;
    return true;
}

bool ParseUnsigned(const char* value, unsigned* out)
{
    if (*value == '-')
        return false;
    char* end = nullptr;
    const unsigned long parsed = strtoul(value, &end, 10);
    if (end == value || *end != '\0' || parsed > 0xFFFFFFFFul)
        return false;
    *out = unsigned(parsed);
    return true;
}

// A float within [low, high]: out-of-range values are brought inside and the problem says so.
bool ParseRange(const char* value, float low, float high, float* out, const char** problem)
{
    float parsed = 0.0f;
    if (!ParseFloat(value, &parsed))
        return false;
    if (parsed < low || parsed > high)
    {
        parsed = parsed < low ? low : high;
        *problem = "out of range, clamped";
    }
    *out = parsed;
    return true;
}

// One of `names` (ignoring case) or its index as a number.
template <typename Enum, size_t N>
bool ParseChoice(const char* value, const char* const (&names)[N], const unsigned (&values)[N], Enum* out)
{
    for (size_t i = 0; i < N; ++i)
    {
        if (_stricmp(value, names[i]) == 0)
        {
            *out = Enum(values[i]);
            return true;
        }
    }
    unsigned number = 0;
    if (!ParseUnsigned(value, &number))
        return false;
    for (size_t i = 0; i < N; ++i)
    {
        if (values[i] == number)
        {
            *out = Enum(number);
            return true;
        }
    }
    return false;
}

template <typename T> bool ParseTunable(const char* value, Tunable<T>* out)
{
    T parsed {};
    const bool ok = [&] {
        if constexpr (std::is_same_v<T, float>)
            return ParseFloat(value, &parsed);
        else
            return ParseUnsigned(value, &parsed);
    }();
    if (ok)
    {
        out->set = true;
        out->value = parsed;
    }
    return ok;
}

const char* const kInputNames[] = { "auto", "linear", "hdr", "tonemapped", "sdr" };
const unsigned kInputValues[] = { 0, 1, 1, 2, 2 };
const char* const kWhiteNames[] = { "auto", "exposure", "game", "scene", "manual" };
const unsigned kWhiteValues[] = { 3, 0, 0, 2, 1 };
const char* const kPreviewNames[] = { "off", "input", "output" };
const unsigned kPreviewValues[] = { 0, 1, 2 };
const char* const kCornerNames[] = { "topleft", "tl", "topright", "tr", "bottomleft", "bl", "bottomright", "br" };
const unsigned kCornerValues[] = { 0, 0, 1, 1, 2, 2, 3, 3 };
const char* const kMenuColourNames[] = { "auto", "sdr", "hdr" };
const unsigned kMenuColourValues[] = { 0, 1, 2 };
const char* const kMenuThemeNames[] = { "dark", "paper" };
const unsigned kMenuThemeValues[] = { 0, 1 };

bool ParseKey(const char* value, unsigned* out) { return KeyFromName(value, out); }

// One `key = value` line into the snapshot. False if the key is unknown or the value does not parse; a value it
// accepted but changed (clamped into range) leaves a note in *problem.
bool Apply(Settings* s, const char* key, const char* value, const char** problem)
{
    *problem = nullptr;
    if (_stricmp(key, "Enabled") == 0)
        return ParseBool(value, &s->enabled);
    if (_stricmp(key, "Badge") == 0)
        return ParseRange(value, 0.0f, 10.0f, &s->badgeSeconds, problem);

    ModelSettings& m = s->model;
    if (_stricmp(key, "Preset") == 0)
        return ParseTunable(value, &m.preset);
    if (_stricmp(key, "Intensity") == 0)
        return ParseTunable(value, &m.intensity);
    if (_stricmp(key, "Style") == 0)
        return ParseTunable(value, &m.style) && m.style.value <= 2;
    if (_stricmp(key, "LocalStructure") == 0)
        return ParseTunable(value, &m.localStructure);
    if (_stricmp(key, "LocalTone") == 0)
        return ParseTunable(value, &m.localTone);
    if (_stricmp(key, "SkinStructure") == 0)
        return ParseTunable(value, &m.skinStructure);
    if (_stricmp(key, "AutoMask") == 0)
    {
        bool on = false;
        if (!ParseBool(value, &on))
            return false;
        m.autoMask.set = true;
        m.autoMask.value = on ? 1u : 0u;
        return true;
    }

    if (_stricmp(key, "InputType") == 0)
        return ParseChoice(value, kInputNames, kInputValues, &s->inputType);
    if (_stricmp(key, "WhiteSource") == 0)
        return ParseChoice(value, kWhiteNames, kWhiteValues, &s->whiteSource);
    if (_stricmp(key, "WhiteEV") == 0)
        return ParseRange(value, -4.0f, 4.0f, &s->whiteEV, problem);
    if (_stricmp(key, "Shoulder") == 0)
        return ParseRange(value, 0.5f, 0.95f, &s->shoulder, problem);
    if (_stricmp(key, "DetailStrength") == 0)
        return ParseRange(value, 0.0f, 2.0f, &s->detailStrength, problem);
    if (_stricmp(key, "ColourStrength") == 0 || _stricmp(key, "ColorStrength") == 0)
        return ParseRange(value, 0.0f, 2.0f, &s->colourStrength, problem);
    if (_stricmp(key, "MaxGainEV") == 0)
        return ParseRange(value, 0.0f, 2.0f, &s->maxGainEV, problem);
    if (_stricmp(key, "HighlightRestore") == 0)
        return ParseRange(value, 0.0f, 1.0f, &s->highlightRestore, problem);

    if (_stricmp(key, "Preview") == 0)
        return ParseChoice(value, kPreviewNames, kPreviewValues, &s->preview);
    if (_stricmp(key, "Zebra") == 0)
        return ParseBool(value, &s->zebra);
    if (_stricmp(key, "CardCorner") == 0)
        return ParseChoice(value, kCornerNames, kCornerValues, &s->cardCorner);
    if (_stricmp(key, "Card") == 0)
        return ParseBool(value, &s->card);
    if (_stricmp(key, "StatsLog") == 0)
    {
        if (!ParseRange(value, 0.0f, 3600.0f, &s->statsSeconds, problem))
            return false;
        if (s->statsSeconds > 0.0f && s->statsSeconds < 0.5f)
        {
            s->statsSeconds = 0.5f;
            *problem = "out of range, clamped";
        }
        return true;
    }
    if (_stricmp(key, "DumpEvery") == 0)
    {
        if (!ParseRange(value, 0.0f, 3600.0f, &s->dumpSeconds, problem))
            return false;
        if (s->dumpSeconds > 0.0f && s->dumpSeconds < 5.0f)
        {
            s->dumpSeconds = 5.0f;
            *problem = "out of range, clamped";
        }
        return true;
    }

    // The menu's own keys (M3).
    if (_stricmp(key, "Compare") == 0)
        return ParseBool(value, &s->compare);
    if (_stricmp(key, "CompareSplit") == 0)
        return ParseRange(value, 10.0f, 90.0f, &s->compareSplit, problem);
    if (_stricmp(key, "MenuKey") == 0)
        return ParseKey(value, &s->menuKey);
    if (_stricmp(key, "ToggleKey") == 0)
        return ParseKey(value, &s->toggleKey);
    if (_stricmp(key, "FreezeKey") == 0)
        return ParseKey(value, &s->freezeKey);
    if (_stricmp(key, "MenuColour") == 0 || _stricmp(key, "MenuColor") == 0)
        return ParseChoice(value, kMenuColourNames, kMenuColourValues, &s->menuColour);
    if (_stricmp(key, "MenuNits") == 0)
        return ParseRange(value, 80.0f, 400.0f, &s->menuNits, problem);
    if (_stricmp(key, "MenuTheme") == 0)
        return ParseChoice(value, kMenuThemeNames, kMenuThemeValues, &s->menuTheme);

    // What the game's depth adds.
    if (_stricmp(key, "DilateMotion") == 0)
        return ParseBool(value, &s->dilateMotion);
    if (_stricmp(key, "SkyTone") == 0)
        return ParseRange(value, 0.0f, 2.0f, &s->skyTone, problem);
    if (_stricmp(key, "SkyStructure") == 0)
        return ParseRange(value, 0.0f, 2.0f, &s->skyStructure, problem);
    if (_stricmp(key, "ShowSky") == 0)
        return ParseBool(value, &s->showSky);
    *problem = "unknown key";
    return false;
}

void Append(char* out, size_t size, size_t* length, const char* format, ...)
{
    if (*length + 1 >= size)
        return;
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(out + *length, size - *length, format, args);
    va_end(args);
    if (written > 0)
        *length += size_t(written) < size - *length ? size_t(written) : size - *length - 1;
}

template <typename T> void DescribeTunable(char* out, size_t size, size_t* length, bool* any, const char* name,
                                           const Tunable<T>& t)
{
    if (!t.set)
        return;
    const char* separator = *any ? ", " : " ";
    *any = true;
    if constexpr (std::is_same_v<T, float>)
        Append(out, size, length, "%s%s %.3g", separator, name, double(t.value));
    else
        Append(out, size, length, "%s%s %u", separator, name, unsigned(t.value));
}

const char* const kInputText[] = { "auto", "linear", "tonemapped" };
const char* const kWhiteText[] = { "exposure", "manual", "scene", "auto" };
const char* const kPreviewText[] = { "off", "input", "output" };
const char* const kCornerText[] = { "top left", "top right", "bottom left", "bottom right" };
const char* const kMenuColourText[] = { "auto", "sdr", "hdr" };
const char* const kMenuThemeText[] = { "dark", "paper" };
} // namespace

void SettingsDescribe(const Settings& s, char* out, size_t size)
{
    size_t length = 0;
    out[0] = '\0';
    Append(out, size, &length,
           "Enabled %d, Badge %.1f s; HDR InputType %s, WhiteSource %s, WhiteEV %+.2f, Shoulder %.2f; composite "
           "DetailStrength %.2f, ColourStrength %.2f, MaxGainEV %.2f, HighlightRestore %.2f; aids Preview %s, Zebra %d, "
           "Card %d (%s); StatsLog %.1f s, DumpEvery %.0f s; model parameters:",
           s.enabled ? 1 : 0, double(s.badgeSeconds), kInputText[unsigned(s.inputType) % 3],
           kWhiteText[unsigned(s.whiteSource) % 4], double(s.whiteEV), double(s.shoulder), double(s.detailStrength),
           double(s.colourStrength), double(s.maxGainEV), double(s.highlightRestore),
           kPreviewText[unsigned(s.preview) % 3], s.zebra ? 1 : 0, s.card ? 1 : 0,
           kCornerText[unsigned(s.cardCorner) % 4], double(s.statsSeconds), double(s.dumpSeconds));
    SettingsDescribeModel(s.model, out + length, size - length);
    length += strlen(out + length);
    char menuKey[16], toggleKey[16], freezeKey[16];
    Append(out, size, &length,
           "; menu Compare %d at %.0f%%, MenuKey %s, ToggleKey %s, FreezeKey %s, MenuColour %s, MenuNits %.0f, MenuTheme %s",
           s.compare ? 1 : 0, double(s.compareSplit), KeyName(s.menuKey, menuKey, sizeof menuKey),
           KeyName(s.toggleKey, toggleKey, sizeof toggleKey), KeyName(s.freezeKey, freezeKey, sizeof freezeKey),
           kMenuColourText[unsigned(s.menuColour) % 3], double(s.menuNits), kMenuThemeText[unsigned(s.menuTheme) % 2]);
    Append(out, size, &length, "; depth DilateMotion %d, SkyTone %.2f, SkyStructure %.2f, ShowSky %d",
           s.dilateMotion ? 1 : 0, double(s.skyTone), double(s.skyStructure), s.showSky ? 1 : 0);
}

void SettingsDescribeModel(const ModelSettings& m, char* out, size_t size)
{
    size_t length = 0;
    out[0] = '\0';
    bool any = false;
    DescribeTunable(out, size, &length, &any, "Preset", m.preset);
    DescribeTunable(out, size, &length, &any, "Intensity", m.intensity);
    DescribeTunable(out, size, &length, &any, "Style", m.style);
    DescribeTunable(out, size, &length, &any, "LocalStructure", m.localStructure);
    DescribeTunable(out, size, &length, &any, "LocalTone", m.localTone);
    DescribeTunable(out, size, &length, &any, "SkinStructure", m.skinStructure);
    DescribeTunable(out, size, &length, &any, "AutoMask", m.autoMask);
    if (!any)
        Append(out, size, &length, " the model's defaults");
}

const Settings* SettingsLoad(const wchar_t* iniPath)
{
    Settings* settings = new (std::nothrow) Settings;
    if (settings == nullptr)
        return SettingsCurrent();
    AcquireSRWLockExclusive(&g_publishLock);
    settings->generation = ++g_generation;
    ReleaseSRWLockExclusive(&g_publishLock);
    if (iniPath != g_path)
        wcscpy_s(g_path, MAX_PATH, iniPath);
    Stat(g_path, &g_stamp, &g_size);

    size_t size = 0;
    char* text = ReadFileText(g_path, &size);
    const bool found = text != nullptr;
    unsigned applied = 0, problems = 0;
    if (found)
    {
        unsigned lineNumber = 0;
        for (char* line = text; line != nullptr && *line != '\0';)
        {
            ++lineNumber;
            char* next = strchr(line, '\n');
            if (next != nullptr)
                *next++ = '\0';
            char* content = line;
            if (char* comment = strpbrk(content, ";#"))
                *comment = '\0';
            content = Trim(content);
            line = next;
            if (*content == '\0' || *content == '[')
                continue;
            char* equals = strchr(content, '=');
            if (equals == nullptr)
            {
                Log("%ls line %u: not `key = value`, ignored", g_path, lineNumber);
                ++problems;
                continue;
            }
            *equals = '\0';
            const char* key = Trim(content);
            const char* value = Trim(equals + 1);
            const char* problem = nullptr;
            if (Apply(settings, key, value, &problem))
            {
                ++applied;
                if (problem != nullptr)
                    Log("%ls line %u: %s (%s = %s)", g_path, lineNumber, problem, key, value);
            }
            else
            {
                Log("%ls line %u: %s (%s = %s), ignored", g_path, lineNumber, problem != nullptr ? problem : "bad value",
                    key, value);
                ++problems;
            }
        }
        delete[] text;
    }

    char described[1024];
    SettingsDescribe(*settings, described, sizeof described);
    Log("settings%s: %s%ls (%u keys%s): %s", settings->generation > 1 ? " reloaded" : "", found ? "" : "no file at ",
        g_path, applied, problems != 0 ? ", some ignored, see above" : "", described);

    AcquireSRWLockExclusive(&g_publishLock);
    Retire(g_current.exchange(settings, std::memory_order_acq_rel));
    ReleaseSRWLockExclusive(&g_publishLock);
    return settings;
}

bool SettingsReloadIfChanged()
{
    if (g_path[0] == L'\0')
        return false;
    FILETIME stamp = {};
    ULONGLONG size = 0;
    Stat(g_path, &stamp, &size);
    if (CompareFileTime(&stamp, &g_stamp) == 0 && size == g_size)
        return false;
    // Editors save in more than one step; give the file a moment to settle, then read what is there.
    Sleep(100);
    SettingsLoad(g_path);
    return true;
}

const Settings* SettingsCurrent() { return g_current.load(std::memory_order_acquire); }

const Settings* SettingsPublish(const Settings& settings)
{
    Settings* copy = new (std::nothrow) Settings(settings);
    if (copy == nullptr)
        return SettingsCurrent();
    AcquireSRWLockExclusive(&g_publishLock);
    copy->generation = ++g_generation;
    Retire(g_current.exchange(copy, std::memory_order_acq_rel));
    ReleaseSRWLockExclusive(&g_publishLock);
    return copy;
}

void SettingsNoteWritten()
{
    if (g_path[0] != L'\0')
        Stat(g_path, &g_stamp, &g_size);
}

const wchar_t* SettingsPath() { return g_path; }
