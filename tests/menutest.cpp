// menutest: the menu without a GPU or a game. The key names, the ini write-back, the menu's model
// and the panel itself are run here: ImGui without a renderer, the panel drawn every frame, the mouse fed to it.
// Nothing is written outside a directory of its own under %TEMP%. The exit code is the number of failed checks.
//
//   build\Release\menutest.exe
//
// Checks:
//   - keys.cpp: names and codes both ways, the hex fallback, unknown names
//   - settings_write.cpp: a file with comments, unknown keys and StatsLog keeps every line it should; a model
//     parameter set back to the model's default loses its line; a new file gets its header; what was written reads
//     back the same; the sky's stripes are never written
//   - settings.cpp: ModelScale's steps (a third, whole percent, two thirds), and the file's value read as the nearest
//   - menu_model.cpp: the draft, A/B, when the file is due, a reload behind the menu
//   - the panel: a slider dragged with the mouse publishes every step at once and logs the drag once, when the mouse
//     lets go; a right-click puts the default back; a box commits at once; the card freezes the frame and the freeze
//     box unfreezes it, unticking the card undoes the freeze it made; skin structure takes no input until the auto
//     mask is on; "all defaults"; the depth page: a sky slider away from 1 greys out the auto mask until it is back,
//     the stripes and the dilation commit at once; the speed page: the model's input size publishes at once, a
//     right-click puts 100% back, a drag across and back moves in its steps, holds at its stops and finds every
//     step one way or the other, the pass's model size and GPU time are drawn; closing the menu writes the file,
//     and so does a second after a commit while it is open
//   - menu_input.cpp, on a hidden window with a loop of its own on a thread of its own, as a game's: one hook on its
//     thread, which sees our own message posted to the window; the raw mouse and keyboard data registered to a window
//     of ours on a thread of ours, the mouse with no window messages (RIDEV_NOLEGACY), and reaching it (F24, which no
//     keyboard has, and the mouse moved one count and back, sent with SendInput; skipped where Windows refuses it); a
//     raw mouse registration the game makes while the menu is open found at the next frame and ours put back; the
//     game's presses kept from it and its releases passed on; at the close no hook, the game's newest registration
//     back, and the game's presses its own again
//   - menu_input.cpp in a game shaped like GTA V (raw mouse and keyboard with no window and no flags, its window
//     holding the mouse capture): F24 held at the open (SendInput; skipped where refused) keeps the raw keyboard the
//     game's until it is let go; a click in the menu neither takes nor releases the capture and asks for no
//     WM_MOUSELEAVE, and ImGui gets it as its own events; in frames whose backend NewFrame (a stand-in) tells ImGui the
//     system cursor's position and a key event, a wheel notch in every frame is taken in its frame and every frame
//     ends at our pointer; at the close the game's registrations are back as they were, a button ImGui had down is up
//     at the next open, and where the game's registration cannot be put back (its window gone) ours is removed

#include <windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include <atomic>

#include "backends/imgui_impl_win32.h"
#include "freeze.h"
#include "imgui.h"
#include "imgui_internal.h" // the input queue, to see what the menu told ImGui
#include "keys.h"
#include "log.h"
#include "menu.h"
#include "menu_input.h"
#include "menu_model.h"
#include "nr_dx12.h"
#include "overlay_dx12.h"
#include "settings.h"
#include "settings_write.h"

// What the DLL has and this test stands in for: the overlay (overlay_dx12.cpp) and the NR pass (nr_dx12.cpp).
NrStatusState g_fakeStatus = {};
bool OverlayOpen(ID3D12GraphicsCommandList*) { return true; }
void OverlayClose() {}
void OverlayRelease() {}
ImTextureID OverlayPreviewTexture(ID3D12Resource*, unsigned, DXGI_FORMAT) { return ImTextureID(0); }
bool OverlayFrameSize(unsigned* width, unsigned* height)
{
    *width = *height = 0;
    return false;
}
const char* OverlayState() { return "menutest"; }
bool NrStatus(NrStatusState* out)
{
    *out = g_fakeStatus;
    return g_fakeStatus.attempted;
}
bool NrPreview(NrPreviewState* out)
{
    *out = {};
    return false;
}

namespace
{
int g_checks = 0;
int g_failed = 0;
wchar_t g_directory[MAX_PATH];
wchar_t g_ini[MAX_PATH];
wchar_t g_logPath[MAX_PATH];

void Check(bool ok, const char* format, ...)
{
    ++g_checks;
    if (!ok)
        ++g_failed;
    std::fputs(ok ? "ok    " : "FAIL  ", stdout);
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

bool ReadAll(const wchar_t* path, char* out, size_t size)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr)
        return false;
    const size_t n = fread(out, 1, size - 1, f);
    out[n] = '\0';
    fclose(f);
    return true;
}

bool WriteAll(const wchar_t* path, const char* text)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || f == nullptr)
        return false;
    fwrite(text, 1, strlen(text), f);
    fclose(f);
    return true;
}

// Waits for the writer thread: the file holds `needle` within `seconds`.
bool FileHas(const char* needle, double seconds)
{
    const double until = LogClock() + seconds;
    do
    {
        char text[8192];
        if (ReadAll(g_ini, text, sizeof text) && strstr(text, needle) != nullptr)
            return true;
        Sleep(20);
    } while (LogClock() < until);
    return false;
}

bool Near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }

// How many times the menu's log holds `needle` so far (read beside the open log, which shares reading and writing).
int LogCount(const char* needle)
{
    const HANDLE f = CreateFileW(g_logPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return -1;
    static char text[65536];
    DWORD read = 0;
    const BOOL ok = ReadFile(f, text, sizeof text - 1, &read, nullptr);
    CloseHandle(f);
    if (!ok)
        return -1;
    text[read] = '\0';
    int count = 0;
    for (const char* at = strstr(text, needle); at != nullptr; at = strstr(at + 1, needle))
        ++count;
    return count;
}

// ---------------------------------------------------------------------------------------------------------------

void TestKeys()
{
    char name[16];
    Check(strcmp(KeyName(0x23, name, sizeof name), "End") == 0, "keys: 0x23 is End");
    Check(strcmp(KeyName(0x7B, name, sizeof name), "F12") == 0, "keys: 0x7B is F12");
    Check(strcmp(KeyName(0x65, name, sizeof name), "Numpad5") == 0, "keys: 0x65 is Numpad5");
    Check(strcmp(KeyName(0x41, name, sizeof name), "A") == 0, "keys: 0x41 is A");
    Check(strcmp(KeyName(0, name, sizeof name), "None") == 0, "keys: 0 is None");
    Check(strcmp(KeyName(0xE7, name, sizeof name), "0xE7") == 0, "keys: 0xE7 has no name (%s)", name);
    unsigned vk = 99;
    Check(KeyFromName("end", &vk) && vk == 0x23, "keys: end -> 0x23");
    Check(KeyFromName("F12", &vk) && vk == 0x7B, "keys: F12 -> 0x7B");
    Check(KeyFromName("numpad5", &vk) && vk == 0x65, "keys: numpad5 -> 0x65");
    Check(KeyFromName("a", &vk) && vk == 0x41, "keys: a -> 0x41");
    Check(KeyFromName("0x2e", &vk) && vk == 0x2E, "keys: 0x2e -> 0x2E");
    Check(KeyFromName("none", &vk) && vk == 0, "keys: none -> 0");
    vk = 99;
    Check(!KeyFromName("banana", &vk) && vk == 99, "keys: banana is no key");
    Check(!KeyFromName("0x1FF", &vk), "keys: 0x1FF is out of range");
    // Every named key comes back to the same code.
    bool all = true;
    for (unsigned k = 1; k < 255; ++k)
    {
        KeyName(k, name, sizeof name);
        unsigned back = 0;
        if (!KeyFromName(name, &back) || back != k)
            all = false;
    }
    Check(all, "keys: every code names itself and comes back");
}

void TestWrite()
{
    const char* text = "; my notes\r\n"
                       "Enabled = 1 ; keep this comment\r\n"
                       "Preset=3\r\n"
                       "Foo = bar\r\n"
                       "StatsLog = 2\r\n"
                       "ColorStrength = 0.5\r\n";
    Settings s;
    s.enabled = false;
    s.model.preset.set = false;
    s.model.intensity.set = true;
    s.model.intensity.value = 0.8f;
    s.statsSeconds = 0.0f;
    s.colourStrength = 1.25f;
    s.menuKey = 0x7B; // F12
    size_t length = 0;
    char* merged = SettingsMergeIni(text, s, &length);
    Check(merged != nullptr, "write: merge gives text");
    if (merged == nullptr)
        return;
    Check(strstr(merged, "; my notes\r\n") != nullptr, "write: the comment line stays");
    Check(strstr(merged, "Enabled = 0  ; keep this comment\r\n") != nullptr, "write: Enabled updated, comment kept");
    Check(strstr(merged, "Preset") == nullptr, "write: a model parameter back to the model's default loses its line");
    Check(strstr(merged, "Foo = bar\r\n") != nullptr, "write: an unknown key stays");
    Check(strstr(merged, "StatsLog = 2\r\n") != nullptr, "write: StatsLog is never touched");
    Check(strstr(merged, "ColourStrength = 1.25\r\n") != nullptr && strstr(merged, "ColorStrength") == nullptr,
          "write: a line under the other spelling of a key becomes the key's own line");
    Check(strstr(merged, "Intensity = 0.8\r\n") != nullptr, "write: a set model parameter is added");
    Check(strstr(merged, "MenuKey = F12\r\n") != nullptr, "write: a key away from its default is added");
    Check(strstr(merged, "Shoulder") == nullptr, "write: a value at its default is not added");
    Check(strstr(merged, "\n\n") == nullptr && strchr(merged, '\n') != nullptr && strstr(merged, "\r\n") != nullptr,
          "write: CRLF kept, no blank lines made");
    Check(WriteAll(g_ini, merged), "write: the merged text goes to the test's ini");
    delete[] merged;

    const Settings* loaded = SettingsLoad(g_ini);
    Check(loaded->generation == 1, "write: the file read (generation %u)", loaded->generation);
    Check(!loaded->enabled, "write: Enabled 0 read back");
    Check(!loaded->model.preset.set, "write: Preset not set");
    Check(loaded->model.intensity.set && Near(loaded->model.intensity.value, 0.8f, 1e-6f),
          "write: Intensity read back");
    Check(Near(loaded->statsSeconds, 2.0f, 1e-6f), "write: StatsLog 2 read back");
    Check(Near(loaded->colourStrength, 1.25f, 1e-6f), "write: ColourStrength read back");
    Check(loaded->menuKey == 0x7B, "write: MenuKey F12 read back");

    // A new file: the header and only the keys away from their defaults.
    Settings fresh;
    fresh.whiteEV = -1.5f;
    merged = SettingsMergeIni(nullptr, fresh, &length);
    Check(merged != nullptr && merged[0] == ';' && strstr(merged, "WhiteEV = -1.5\r\n") != nullptr &&
              strstr(merged, "Enabled") == nullptr,
          "write: a new file gets a header and WhiteEV only");
    delete[] merged;

    // Written with SettingsWriteIni, read back identical through SettingsLoad.
    Settings full;
    full.model.preset = { true, 4 };
    full.model.style = { true, 2 };
    full.model.skinStructure = { true, -1.0f };
    full.model.autoMask = { true, 0 };
    full.inputType = InputType::LinearHdr;
    full.whiteSource = WhiteSource::Manual;
    full.shoulder = 0.85f;
    full.preview = Preview::Output;
    full.zebra = false;
    full.cardCorner = Corner::TopRight;
    full.compare = true;
    full.compareSplit = 33.0f;
    full.toggleKey = 0x70; // F1
    full.freezeKey = 0x71; // F2
    full.menuColour = MenuColour::Hdr;
    full.menuNits = 250.0f;
    full.dilateMotion = true;
    full.skyTone = 0.5f;
    full.skyStructure = 1.25f;
    full.showSky = true;
    full.modelScale = kModelScaleTwoThirds;
    unsigned long error = 0;
    Check(SettingsWriteIni(g_ini, full, &error), "write: SettingsWriteIni (error %lu)", error);
    loaded = SettingsLoad(g_ini);
    Check(loaded->model == full.model, "write: the model parameters read back the same");
    Check(loaded->inputType == full.inputType && loaded->whiteSource == full.whiteSource &&
              Near(loaded->shoulder, full.shoulder, 1e-6f) && loaded->preview == full.preview &&
              loaded->zebra == full.zebra && loaded->cardCorner == full.cardCorner,
          "write: the encode, composite and aid keys read back the same");
    Check(loaded->compare == full.compare && Near(loaded->compareSplit, full.compareSplit, 1e-6f) &&
              loaded->toggleKey == full.toggleKey && loaded->freezeKey == full.freezeKey &&
              loaded->menuColour == full.menuColour && Near(loaded->menuNits, full.menuNits, 1e-6f),
          "write: the menu's keys read back the same");
    Check(loaded->dilateMotion == full.dilateMotion && Near(loaded->skyTone, full.skyTone, 1e-6f) &&
              Near(loaded->skyStructure, full.skyStructure, 1e-6f),
          "write: the depth page's keys read back the same");
    Check(loaded->modelScale == full.modelScale, "write: the speed page's key reads back the same (%.4g%%)",
          double(loaded->modelScale));
    Check(!loaded->showSky && !FileHas("ShowSky", 0.0), "write: the sky's stripes are never written");
    Check(loaded->enabled && Near(loaded->statsSeconds, 2.0f, 1e-6f), "write: StatsLog still 2, Enabled back to 1");
}

// ModelScale's steps: a third, each whole percent from 34, two thirds in place of 67; anything else goes to the
// nearest, the file's value too.
void TestModelScaleSteps()
{
    struct Case
    {
        float in, out;
    };
    const Case cases[] = { { 0.0f, kModelScaleThird },
                           { 33.3f, kModelScaleThird },
                           { 33.6f, kModelScaleThird },
                           { 33.7f, 34.0f },
                           { 49.6f, 50.0f },
                           { 66.3f, 66.0f },
                           { 66.4f, kModelScaleTwoThirds },
                           { 67.0f, kModelScaleTwoThirds },
                           { 67.3f, kModelScaleTwoThirds },
                           { 67.4f, 68.0f },
                           { 99.4f, 99.0f },
                           { 99.6f, 100.0f },
                           { 150.0f, 100.0f },
                           { std::nanf(""), 100.0f } };
    int wrong = 0;
    for (const Case& c : cases)
    {
        const float got = ModelScaleStep(c.in);
        if (got != c.out)
        {
            Check(false, "steps: %g goes to %g, not %g", double(c.in), double(got), double(c.out));
            ++wrong;
        }
    }
    Check(wrong == 0, "steps: values go to the nearest step");
    for (const float stop : kModelScaleStops)
        Check(ModelScaleStep(stop) == stop, "steps: the stop %.4g%% is a step", double(stop));

    WriteAll(g_ini, "ModelScale = 42.4\r\n");
    Check(SettingsLoad(g_ini)->modelScale == 42.0f, "steps: ModelScale = 42.4 in the file reads as 42");
    WriteAll(g_ini, "ModelScale = 67\r\n");
    Check(SettingsLoad(g_ini)->modelScale == kModelScaleTwoThirds, "steps: 67 reads as two thirds");
    WriteAll(g_ini, "ModelScale = 33.3\r\n");
    Check(SettingsLoad(g_ini)->modelScale == kModelScaleThird && LogCount("(ModelScale = 33.3)") == 0,
          "steps: 33.3 reads as a third, and the log has nothing to say about it");
    WriteAll(g_ini, "ModelScale = 20\r\n");
    Check(SettingsLoad(g_ini)->modelScale == kModelScaleThird &&
              LogCount("out of range, clamped (ModelScale = 20)") == 1,
          "steps: 20 reads as a third, and the log says it was out of range");
}

// A stand-in for SettingsPublish that counts.
unsigned g_published = 0;
Settings g_lastPublished;
const Settings* FakePublish(const Settings& s)
{
    ++g_published;
    g_lastPublished = s;
    g_lastPublished.generation = 1000 + g_published;
    return &g_lastPublished;
}

void TestModel()
{
    Settings current;
    current.generation = 7;
    current.model.intensity = { true, 0.9f };
    MenuModel m;
    MenuModelInit(&m, current);
    Check(m.generation == 7 && !m.dirty && !m.abOff, "model: init takes the snapshot");
    Check(!MenuModelWriteDue(m, 100.0), "model: nothing due after init");
    Check(!MenuModelSync(&m, current), "model: the same generation is no reload");

    m.draft.whiteEV = 1.0f;
    const Settings* p = MenuModelCommit(&m, 10.0, &FakePublish);
    Check(g_published == 1 && Near(p->whiteEV, 1.0f, 1e-6f) && p->enabled, "model: a commit publishes the draft");
    Check(m.dirty && m.generation == p->generation, "model: dirty, generation follows the publish");
    m.open = true;
    Check(!MenuModelWriteDue(m, 10.5), "model: open, not due at 0.5 s");
    Check(MenuModelWriteDue(m, 11.0), "model: open, due at 1 s");
    m.open = false;
    Check(MenuModelWriteDue(m, 10.5), "model: closed, due at once");
    MenuModelWritten(&m);
    Check(!m.dirty && !MenuModelWriteDue(m, 20.0), "model: written clears dirty");

    m.abOff = true;
    p = MenuModelCommit(&m, 12.0, &FakePublish);
    Check(g_published == 2 && !p->enabled && m.draft.enabled, "model: A/B publishes Enabled 0, the draft keeps 1");
    Check(!m.dirty, "model: A/B alone makes nothing due for the file");

    Settings reloaded = current;
    reloaded.generation = 8;
    reloaded.whiteEV = -2.0f;
    Check(MenuModelSync(&m, reloaded), "model: a foreign generation is a reload");
    Check(Near(m.draft.whiteEV, -2.0f, 1e-6f) && !m.abOff && !m.dirty && m.generation == 8,
          "model: the draft follows the file, A/B cleared");

    KeyEdge edge;
    Check(!KeyEdgeUpdate(&edge, false) && KeyEdgeUpdate(&edge, true) && !KeyEdgeUpdate(&edge, true) &&
              !KeyEdgeUpdate(&edge, false) && KeyEdgeUpdate(&edge, true),
          "model: a key edge fires once per press");
}

// ---------------------------------------------------------------------------------------------------------------
// The panel, headless.

void Frame()
{
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    MenuLock();
    MenuDraw();
    MenuUnlock();
    ImGui::Render();
}

bool Centre(const char* key, float* x, float* y, float atX = 0.5f)
{
    Frame(); // the layout of a frame follows what the frame before changed: read a rectangle after that
    float x0, y0, x1, y1;
    if (!MenuItemRect(key, &x0, &y0, &x1, &y1))
        return false;
    *x = x0 + (x1 - x0) * atX;
    *y = (y0 + y1) * 0.5f;
    return true;
}

void MoveTo(float x, float y)
{
    ImGui::GetIO().AddMousePosEvent(x, y);
    Frame();
}

void Button(int button, bool down)
{
    ImGui::GetIO().AddMouseButtonEvent(button, down);
    Frame();
}

// Click a control's centre (or a point along its width) with the left button: move, press, release.
bool Click(const char* key, float atX = 0.5f, int button = 0)
{
    float x, y;
    if (!Centre(key, &x, &y, atX))
        return false;
    MoveTo(x, y);
    Button(button, true);
    Button(button, false);
    return true;
}

unsigned Generation() { return SettingsCurrent()->generation; }

void TestPanel()
{
    // The file the menu starts from.
    WriteAll(g_ini, "Enabled = 1\r\nIntensity = 0.7\r\nStatsLog = 0\r\nDumpEvery = 0\r\n");
    const Settings* start = SettingsLoad(g_ini);
    const unsigned g0 = start->generation;

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1920.0f, 1200.0f);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // no renderer here: the atlas stays unbuilt, allowed
    io.IniFilename = nullptr;
    io.Fonts->AddFontDefault();

    g_fakeStatus = {};
    g_fakeStatus.attempted = true;
    g_fakeStatus.haveFeature = true;
    g_fakeStatus.ready = true;
    g_fakeStatus.width = 3840;
    g_fakeStatus.height = 2160;
    g_fakeStatus.feature = 13;
    g_fakeStatus.hdr = true;
    g_fakeStatus.linear = true;
    g_fakeStatus.havePreExposure = true;

    MenuOnEvaluate(nullptr); // takes the snapshot as the draft
    MenuSetOpen(true);
    Check(MenuIsOpen(), "panel: the menu opens");
    Frame();
    Frame();
    float x, y;
    Check(Centre("DetailStrength", &x, &y), "panel: the strength slider was drawn");
    Check(!Centre("Intensity", &x, &y), "panel: the model's parameters wait under the advanced header");
    Check(Click("Advanced"), "panel: open the advanced header");
    Check(Centre("Intensity", &x, &y), "panel: the Intensity slider was drawn");
    Check(Centre("Close", &x, &y), "panel: the close button was drawn");
    Check(Generation() == g0, "panel: drawing publishes nothing");

    // A slider: press a tenth of the way along (the grab sits at 0.7, away from there), drag to six tenths, release.
    // Each step is published as it happens; letting go publishes nothing more and logs the drag in one line.
    Check(Centre("Intensity", &x, &y, 0.1f), "panel: Intensity rect");
    MoveTo(x, y);
    Button(0, true);
    Frame();
    {
        const float shown = SettingsCurrent()->model.intensity.value;
        Check(Generation() == g0 + 1 && SettingsCurrent()->model.intensity.set && shown < 0.4f,
              "panel: pressing publishes the value under the mouse at once (%.2f, generation %u)", shown,
              Generation());
    }
    Centre("Intensity", &x, &y, 0.6f);
    MoveTo(x, y);
    Frame();
    Check(Generation() == g0 + 2 && Near(SettingsCurrent()->model.intensity.value, 0.6f, 0.1f),
          "panel: dragging publishes the new value at once (%.2f)", double(SettingsCurrent()->model.intensity.value));
    Check(LogCount("menu slider") == 0 && LogCount("menu panel") == 0, "panel: no log line while the slider is dragged");
    Button(0, false);
    Frame();
    Check(Generation() == g0 + 2, "panel: letting go publishes nothing more (generation %u)", Generation());
    Check(LogCount("menu slider: Intensity 0.7 -> ") == 1 && LogCount("menu slider") == 1,
          "panel: letting go logs the drag once, from where it started");
    const float intensity = SettingsCurrent()->model.intensity.value;
    Check(SettingsCurrent()->model.intensity.set && Near(intensity, 0.6f, 0.1f),
          "panel: the published Intensity is where the mouse let go (%.2f)", intensity);
    Check(!FileHas("Intensity = 1", 0.3), "panel: the file is not written within 0.3 s of a commit");
    Sleep(1000);
    MenuOnEvaluate(nullptr);
    char expected[32];
    snprintf(expected, sizeof expected, "Intensity = %.4g", double(intensity));
    Check(FileHas(expected, 5.0), "panel: a second after the commit the file holds %s", expected);
    Check(FileHas("StatsLog = 0", 0.0) && FileHas("DumpEvery = 0", 0.0), "panel: StatsLog and DumpEvery untouched");

    // A right-click: the model's default again (not set).
    Check(Click("Intensity", 0.5f, 1), "panel: right-click Intensity");
    Frame();
    const unsigned g1 = Generation();
    Check(g1 == g0 + 3 && !SettingsCurrent()->model.intensity.set, "panel: right-click makes it the model's default");

    // A box commits at once.
    Check(Click("Enabled"), "panel: click Enabled");
    Check(Generation() == g1 + 1 && !SettingsCurrent()->enabled, "panel: Enabled off published at once");
    Check(Click("Enabled"), "panel: click Enabled again");
    Check(Generation() == g1 + 2 && SettingsCurrent()->enabled, "panel: Enabled on again");

    // A/B: the pass sees Enabled 0, the draft keeps 1.
    Check(Click("AB"), "panel: click A/B");
    {
        MenuLock();
        const bool draftEnabled = MenuModelLocked().draft.enabled;
        const bool abOff = MenuModelLocked().abOff;
        MenuUnlock();
        Check(Generation() == g1 + 3 && !SettingsCurrent()->enabled && draftEnabled && abOff,
              "panel: A/B publishes Enabled 0 and keeps the draft's 1");
    }
    Check(Click("AB"), "panel: click A/B again");
    Check(Generation() == g1 + 4 && SettingsCurrent()->enabled, "panel: A/B back");

    // The card freezes; the freeze box unfreezes and takes the card with it. The card is on the compare page.
    Check(!FreezeWanted(), "panel: not frozen to begin with");
    Check(!Centre("Card", &x, &y), "panel: the card is not there while the tuning page shows");
    Check(Click("TabCompare"), "panel: open the compare page");
    Check(!Centre("Intensity", &x, &y), "panel: the tuning page is hidden now");
    Check(Click("Card"), "panel: click the card");
    Check(FreezeWanted() && SettingsCurrent()->card && Generation() == g1 + 5, "panel: the card freezes the frame");
    Check(Click("Freeze"), "panel: click the freeze box");
    Check(!FreezeWanted(), "panel: unfrozen");
    Check(Generation() == g1 + 6 && !SettingsCurrent()->card, "panel: the card went with the freeze");
    // Unticking the card undoes the freeze it made; a freeze made by hand stays.
    Check(Click("Card"), "panel: click the card again");
    Check(FreezeWanted() && SettingsCurrent()->card && Generation() == g1 + 7, "panel: the card freezes again");
    Check(Click("Card"), "panel: untick the card");
    Check(!FreezeWanted() && !SettingsCurrent()->card && Generation() == g1 + 8,
          "panel: the card off undoes the freeze it made");
    Check(Click("Freeze") && FreezeWanted(), "panel: freeze by hand");
    Check(Click("Card") && SettingsCurrent()->card && Generation() == g1 + 9,
          "panel: the card on a frame frozen by hand");
    Check(Click("Card") && !SettingsCurrent()->card && Generation() == g1 + 10, "panel: the card off again");
    Check(FreezeWanted(), "panel: the freeze made by hand stays");
    Check(Click("Freeze") && !FreezeWanted(), "panel: unfrozen by hand");
    Check(Click("TabTune"), "panel: back to the tuning page");
    Check(Centre("Intensity", &x, &y), "panel: the tuning page shows again");

    // A choice commits at once and a reload behind the menu is followed.
    WriteAll(g_ini, "Enabled = 1\r\nIntensity = 0.3\r\nWhiteEV = 2\r\nStatsLog = 0\r\nDumpEvery = 0\r\n");
    Check(SettingsReloadIfChanged(), "panel: the file changed behind the menu and was read");
    MenuOnEvaluate(nullptr);
    Frame();
    {
        MenuLock();
        const Settings& d = MenuModelLocked().draft;
        MenuUnlock();
        Check(d.model.intensity.set && Near(d.model.intensity.value, 0.3f, 1e-6f) && Near(d.whiteEV, 2.0f, 1e-6f),
              "panel: the draft follows the reloaded file");
    }
    const unsigned g2 = Generation();

    // Skin structure reaches the model only with the auto mask on: greyed out, it takes no clicks until then.
    Check(Centre("SkinStructure", &x, &y), "panel: the skin structure slider was drawn");
    Check(Click("SkinStructure", 0.8f) && Generation() == g2 && !SettingsCurrent()->model.skinStructure.set,
          "panel: skin structure takes no click while the auto mask is off");
    {
        MenuLock();
        MenuModelLocked().draft.model.autoMask = { true, 1u };
        MenuCommitLocked("test");
        MenuUnlock();
    }
    Check(Generation() == g2 + 1 && SettingsCurrent()->model.autoMask.set, "panel: auto mask on published");
    Check(Click("SkinStructure", 0.8f), "panel: click skin structure with the auto mask on");
    Check(Generation() == g2 + 2 && SettingsCurrent()->model.skinStructure.set &&
              SettingsCurrent()->model.skinStructure.value > 0.9f,
          "panel: with the auto mask on, a click on skin structure sets it (%.2f)",
          double(SettingsCurrent()->model.skinStructure.value));

    // All defaults, through the confirmation.
    Check(Click("Defaults"), "panel: click all defaults");
    Frame();
    Check(Click("DefaultsYes"), "panel: confirm");
    Check(Generation() == g2 + 3 && !SettingsCurrent()->model.intensity.set &&
              !SettingsCurrent()->model.autoMask.set && !SettingsCurrent()->model.skinStructure.set &&
              Near(SettingsCurrent()->whiteEV, 0.0f, 1e-6f),
          "panel: all defaults published");

    // The depth page. A sky slider away from 1 gives the model a control mask, and with one the model keeps its own
    // auto mask off: the auto mask takes no input until both sky sliders are back at 1.
    const unsigned g3 = Generation();
    Check(!Centre("SkyStructure", &x, &y), "panel: the sky sliders wait on the depth page");
    Check(Click("TabDepth"), "panel: open the depth page");
    Check(!Centre("DetailStrength", &x, &y), "panel: the tuning page is hidden now");
    Check(Click("SkyStructure", 0.1f), "panel: click sky local structure near its left end");
    Check(Generation() == g3 + 1 && SettingsCurrent()->skyStructure < 0.3f,
          "panel: the click publishes the value under the mouse at once (%.2f, generation %u)",
          double(SettingsCurrent()->skyStructure), Generation());
    Check(Click("ShowSky") && SettingsCurrent()->showSky && Generation() == g3 + 2, "panel: the sky's stripes on");
    Check(Click("TabTune") && Centre("AutoMask", &x, &y), "panel: back on the tuning page, the auto mask is drawn");
    Check(Click("AutoMask", 0.5f, 1) && Generation() == g3 + 2,
          "panel: the auto mask takes no right-click while a sky slider is away from 1");
    Check(Click("TabDepth") && Click("SkyStructure", 0.5f, 1), "panel: right-click sky local structure");
    Check(Generation() == g3 + 3 && SettingsCurrent()->skyStructure == 1.0f, "panel: sky local structure back to 1");
    Check(Click("TabTune") && Click("AutoMask", 0.5f, 1) && Generation() == g3 + 4,
          "panel: with both sky sliders at 1 the auto mask takes a right-click again");
    Check(Click("TabDepth") && Click("DilateMotion") && SettingsCurrent()->dilateMotion && Generation() == g3 + 5,
          "panel: dilate by depth on, published at once");
    Check(Click("TabTune"), "panel: back to the tuning page");

    // The speed page: the model's input size is a slider like the others, under it the size the pass gives the model
    // and its time on the GPU, as the pass reports them.
    const unsigned g4 = Generation();
    g_fakeStatus.modelWidth = 2560;
    g_fakeStatus.modelHeight = 1440;
    g_fakeStatus.haveTiming = true;
    g_fakeStatus.gpuMs = 4.1f;
    g_fakeStatus.modelMs = 3.8f;
    Check(!Centre("ModelScale", &x, &y), "panel: the model's input size waits on the speed page");
    Check(Click("TabSpeed"), "panel: open the speed page");
    Check(!Centre("DetailStrength", &x, &y), "panel: the tuning page is hidden now");
    Check(Click("ModelScale", 0.1f), "panel: click the model's input size near its left end");
    Check(Generation() == g4 + 1 && SettingsCurrent()->modelScale < 60.0f,
          "panel: the click publishes the size under the mouse at once (%.0f%%, generation %u)",
          double(SettingsCurrent()->modelScale), Generation());
    Check(Click("ModelScale", 0.5f, 1) && Generation() == g4 + 2 && SettingsCurrent()->modelScale == 100.0f,
          "panel: a right-click puts the model's input size back to 100%%");
    {
        const bool clicked = Click("ModelScale", 0.5f);
        const float size = SettingsCurrent()->modelScale;
        Check(clicked && Generation() == g4 + 3 && size == kModelScaleTwoThirds,
              "panel: a click halfway along is two thirds (%.4g%%)", double(size));
    }

    // A drag from the left end to the right and back, a quarter of a pixel at a time. Each value is a step and comes
    // in order; a stop between the ends holds the grab well past where the next step would begin, so the step after
    // it comes up only on the way back, the one before it only on the way there; every step comes up one way or the
    // other.
    {
        struct Seen
        {
            float value;
            int there, back; // quarter pixels shown, each way
        };
        Seen seen[80] = {};
        int count = 0;
        bool stepsOnly = true, inOrder = true;
        float last = 0.0f;
        auto note = [&](bool there)
        {
            const float value = SettingsCurrent()->modelScale;
            stepsOnly &= ModelScaleStep(value) == value;
            inOrder &= there ? value >= last : value <= last;
            last = value;
            int i = 0;
            while (i < count && seen[i].value != value)
                ++i;
            if (i == count && count < 80)
                seen[count++] = { value, 0, 0 };
            if (i < count)
                ++(there ? seen[i].there : seen[i].back);
        };
        auto shown = [&](float value, bool there)
        {
            for (int i = 0; i < count; ++i)
            {
                if (seen[i].value == value)
                    return there ? seen[i].there : seen[i].back;
            }
            return 0;
        };
        const int logged = LogCount("menu slider: ModelScale");
        float left = 0.0f, right = 0.0f;
        Check(Centre("ModelScale", &left, &y, 0.0f) && Centre("ModelScale", &right, &y, 1.0f),
              "panel: the model's input size slider's ends");
        MoveTo(left, y);
        Button(0, true);
        last = SettingsCurrent()->modelScale;
        Check(last == kModelScaleThird, "panel: pressed at its left end, the size is a third (%.4g%%)", double(last));
        for (float at = left; at <= right; at += 0.25f)
        {
            MoveTo(at, y);
            note(true);
        }
        Check(last == 100.0f, "panel: dragged to the right end, the size is 100%% (%.4g%%)", double(last));
        for (float at = right; at >= left; at -= 0.25f)
        {
            MoveTo(at, y);
            note(false);
        }
        Button(0, false);
        Check(stepsOnly && inOrder, "panel: the drag gives only steps, in order (%d values)", count);
        int missing = 0;
        for (float step = 34.0f; step <= 100.0f; step += 1.0f)
        {
            const float value = step == 67.0f ? kModelScaleTwoThirds : step;
            missing += shown(value, true) + shown(value, false) == 0 ? 1 : 0;
        }
        missing += shown(kModelScaleThird, true) + shown(kModelScaleThird, false) == 0 ? 1 : 0;
        Check(missing == 0 && count == 68, "panel: every step came up one way or the other (%d missing, %d values)",
              missing, count);
        // A plain step's share of the drag, the mean of ten (ImGui takes the mouse to whole pixels, so one alone may
        // be a pixel more or less), against the stops': a stop holds over about two and a half steps.
        float plainThere = 0.0f, plainBack = 0.0f;
        for (float step = 35.0f; step < 45.0f; step += 1.0f)
        {
            plainThere += float(shown(step, true)) / 10.0f;
            plainBack += float(shown(step, false)) / 10.0f;
        }
        for (const float stop : { 50.0f, kModelScaleTwoThirds, 80.0f })
        {
            Check(float(shown(stop, true)) >= 1.75f * plainThere && float(shown(stop, false)) >= 1.75f * plainBack,
                  "panel: the stop %.4g%% holds the grab (%d and %d quarter pixels, a plain step %.1f and %.1f)",
                  double(stop), shown(stop, true), shown(stop, false), double(plainThere), double(plainBack));
        }
        Check(shown(49.0f, true) > 0 && shown(49.0f, false) == 0 && shown(51.0f, true) == 0 && shown(51.0f, false) > 0,
              "panel: 49%% comes up on the way to the stop at 50%%, 51%% on the way back");
        Check(SettingsCurrent()->modelScale == kModelScaleThird && LogCount("menu slider: ModelScale") == logged + 1,
              "panel: let go at the left end, a third, the drag logged once");
    }
    g_fakeStatus.haveTiming = false;
    Frame();
    Check(Click("TabTune"), "panel: back to the tuning page");

    // Closing writes the file at once.
    {
        MenuLock();
        MenuModelLocked().draft.shoulder = 0.9f;
        MenuCommitLocked("test");
        MenuUnlock();
    }
    Check(Click("Close"), "panel: click close");
    Check(!MenuIsOpen(), "panel: the menu is closed");
    Check(FileHas("Shoulder = 0.9", 5.0), "panel: the file was written on close");
    Check(!FileHas("Intensity", 0.0) && FileHas("WhiteEV = 0", 0.0),
          "panel: a model parameter back at the model's default lost its line, a key with a line keeps it");
    Check(FileHas("StatsLog = 0", 0.0), "panel: StatsLog still there");
    Check(FileHas("DilateMotion = 1", 0.0) && !FileHas("SkyStructure", 0.0) && !FileHas("ShowSky", 0.0),
          "panel: DilateMotion written; a sky slider back at 1 and the stripes not");
    Check(FileHas("ModelScale = 33.33", 0.0), "panel: ModelScale written");

    ImGui::DestroyContext();
}

// ---------------------------------------------------------------------------------------------------------------
// The menu's input (menu_input.cpp) on a window with a loop of its own on a thread of its own, as a game's.

struct GameWindow
{
    HWND window;
    HANDLE ready;
    std::atomic<unsigned> keyDowns, keyUps, buttonDowns, buttonUps, captureChanges;
};

// Sent to the game's window, so that it runs on the game's thread: wParam says what.
constexpr UINT kGameCall = WM_APP + 1;
enum GameCall : WPARAM
{
    kTakeCapture = 1, // the game takes the mouse capture, as GTA V does; returns GetCapture()
    kCapture,         // returns GetCapture()
    kDropCapture,     // ReleaseCapture
    kMakeWindow,      // another window of the game's; returns it
    kDestroyWindow,   // destroys the window in lParam
};

LRESULT CALLBACK GameProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == kGameCall)
    {
        switch (wParam)
        {
        case kTakeCapture:
            SetCapture(window);
            return reinterpret_cast<LRESULT>(GetCapture());
        case kCapture:
            return reinterpret_cast<LRESULT>(GetCapture());
        case kDropCapture:
            ReleaseCapture();
            return 0;
        case kMakeWindow:
            return reinterpret_cast<LRESULT>(CreateWindowExW(0, L"BananaZeroMenutestGame", L"", WS_OVERLAPPEDWINDOW, 0,
                                                             0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr),
                                                             nullptr));
        case kDestroyWindow:
            DestroyWindow(reinterpret_cast<HWND>(lParam));
            return 0;
        default:
            return 0;
        }
    }
    if (GameWindow* game = reinterpret_cast<GameWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA)))
    {
        if (message == WM_KEYDOWN)
            ++game->keyDowns;
        else if (message == WM_KEYUP)
            ++game->keyUps;
        else if (message == WM_LBUTTONDOWN)
            ++game->buttonDowns;
        else if (message == WM_LBUTTONUP)
            ++game->buttonUps;
        else if (message == WM_CAPTURECHANGED)
            ++game->captureChanges;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

DWORD WINAPI GameThread(void* context)
{
    GameWindow* game = static_cast<GameWindow*>(context);
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof windowClass;
    windowClass.lpfnWndProc = &GameProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"BananaZeroMenutestGame";
    RegisterClassExW(&windowClass);
    // Never shown: the test takes no focus from whatever is in front.
    game->window = CreateWindowExW(0, windowClass.lpszClassName, L"menutest", WS_OVERLAPPEDWINDOW, 0, 0, 640, 360,
                                   nullptr, nullptr, windowClass.hInstance, nullptr);
    if (game->window != nullptr)
        SetWindowLongPtrW(game->window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(game));
    SetEvent(game->ready);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

// Waits for `done` within `seconds`.
template <typename F> bool Within(double seconds, F done)
{
    const double until = LogClock() + seconds;
    while (!done())
    {
        if (LogClock() > until)
            return false;
        Sleep(10);
    }
    return true;
}

MenuInputStats Counts()
{
    MenuInputStats stats = {};
    MenuInputCounts(&stats);
    return stats;
}

// Who this process's raw mouse and keyboard data goes to: `count` registrations, the mouse's and the keyboard's, and
// the mouse's flags.
void Registered(UINT* count, HWND* mouse, HWND* keyboard, DWORD* mouseFlags = nullptr)
{
    *count = 0;
    *mouse = *keyboard = nullptr;
    RAWINPUTDEVICE devices[16];
    UINT size = 16;
    const UINT got = GetRegisteredRawInputDevices(devices, &size, sizeof(RAWINPUTDEVICE));
    if (got == UINT(-1))
        return;
    *count = got;
    for (UINT i = 0; i < got; ++i)
    {
        if (devices[i].usUsagePage == 1 && devices[i].usUsage == 2)
        {
            *mouse = devices[i].hwndTarget;
            if (mouseFlags != nullptr)
                *mouseFlags = devices[i].dwFlags;
        }
        if (devices[i].usUsagePage == 1 && devices[i].usUsage == 6)
            *keyboard = devices[i].hwndTarget;
    }
}

// This process's raw registration for a usage (2 the mouse, 6 the keyboard): whether there is one, its window and
// flags.
bool RegisteredFor(USHORT usage, HWND* target, DWORD* flags)
{
    *target = nullptr;
    *flags = 0;
    RAWINPUTDEVICE devices[16];
    UINT size = 16;
    const UINT got = GetRegisteredRawInputDevices(devices, &size, sizeof(RAWINPUTDEVICE));
    for (UINT i = 0; got != UINT(-1) && i < got; ++i)
        if (devices[i].usUsagePage == 1 && devices[i].usUsage == usage)
        {
            *target = devices[i].hwndTarget;
            *flags = devices[i].dwFlags;
            return true;
        }
    return false;
}

// One frame of the open menu's input, as the drawing thread runs it.
void AttachFrame(HWND window)
{
    MenuLock();
    MenuInputAttach(window);
    MenuUnlock();
}

// The open menu's next frames until the raw data of `usage` (2 the mouse, 6 the keyboard, 0 both) is registered to
// our window, `seconds` at most: a key or a button held on this PC keeps that device's the game's until it is let go.
bool AttachUntilOurs(HWND window, USHORT usage, double seconds)
{
    const double until = LogClock() + seconds;
    for (;;)
    {
        AttachFrame(window);
        const HWND ours = Counts().inputWindow;
        HWND target = nullptr;
        DWORD flags = 0;
        const bool mouse = RegisteredFor(2, &target, &flags) && target == ours;
        const bool keyboard = RegisteredFor(6, &target, &flags) && target == ours;
        if (ours != nullptr && (usage == 2 ? mouse : usage == 6 ? keyboard : mouse && keyboard))
            return true;
        if (LogClock() > until)
            return false;
        Sleep(10);
    }
}

// Whether the game's window still holds the mouse capture it took, and has been told of no change to it.
bool CaptureKept(GameWindow& game)
{
    return reinterpret_cast<HWND>(SendMessageW(game.window, kGameCall, kCapture, 0)) == game.window &&
           game.captureChanges == 0;
}

// The mouse events ImGui has queued and not yet taken: presses and releases of `button`, and the last position.
void QueuedMouse(int button, unsigned* downs, unsigned* ups, ImVec2* last)
{
    *downs = *ups = 0;
    *last = ImVec2(-1.0f, -1.0f);
    MenuLock();
    for (const ImGuiInputEvent& e : ImGui::GetCurrentContext()->InputEventsQueue)
    {
        if (e.Type == ImGuiInputEventType_MouseButton && e.MouseButton.Button == button)
            ++*(e.MouseButton.Down ? downs : ups);
        if (e.Type == ImGuiInputEventType_MousePos)
            *last = ImVec2(e.MousePos.PosX, e.MousePos.PosY);
    }
    MenuUnlock();
}

// The backend's NewFrame on a game in front whose system cursor is away from our pointer: the cursor's position, and
// in every third frame a key event after it (its key workarounds queue Shift and Win releases there). Told after them,
// our pointer made two positions in every frame: ImGui's input trickling then held a wheel notch back a frame for
// each, and a frame with the key event in between ended at the cursor.
unsigned g_backendFrames = 0;
void StandInBackendNewFrame()
{
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(1.0f, 2.0f);
    if (g_backendFrames % 3 == 0)
        io.AddKeyEvent(ImGuiKey_F23, (g_backendFrames / 3) % 2 == 0);
    ++g_backendFrames;
}

// WM_MOUSELEAVE as the game's thread takes it from its queue, before the menu's hook (which keeps it from the game):
// a hook put on after the menu's runs before it. Only TrackMouseEvent makes the message.
std::atomic<unsigned> g_mouseLeaves { 0 };
LRESULT CALLBACK CountLeaves(int code, WPARAM removal, LPARAM lParam)
{
    const MSG* msg = reinterpret_cast<const MSG*>(lParam);
    if (code == HC_ACTION && removal == PM_REMOVE && msg != nullptr && msg->message == WM_MOUSELEAVE)
        ++g_mouseLeaves;
    return CallNextHookEx(nullptr, code, removal, lParam);
}

// Input sent with SendInput: F24, which no keyboard has, and the mouse one count to the right and back, which leaves
// the cursor where it was. False if Windows refused it (no desktop of ours in front: a locked screen).
bool SendHarmless(unsigned times)
{
    for (unsigned i = 0; i < times; ++i)
    {
        INPUT inputs[4] = {};
        inputs[0].type = inputs[1].type = INPUT_KEYBOARD;
        inputs[0].ki.wVk = inputs[1].ki.wVk = VK_F24;
        inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
        inputs[2].type = inputs[3].type = INPUT_MOUSE;
        inputs[2].mi.dwFlags = inputs[3].mi.dwFlags = MOUSEEVENTF_MOVE;
        inputs[2].mi.dx = 1;
        inputs[3].mi.dx = -1;
        if (SendInput(4, inputs, sizeof(INPUT)) != 4)
            return false;
    }
    return true;
}

void TestInput()
{
    UINT before = 0;
    HWND mouse = nullptr, keyboard = nullptr;
    Registered(&before, &mouse, &keyboard);

    GameWindow game = {};
    game.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD gameThreadId = 0;
    const HANDLE gameThread = CreateThread(nullptr, 0, &GameThread, &game, 0, &gameThreadId);
    WaitForSingleObject(game.ready, 5000);
    Check(game.window != nullptr, "input: a game window with a loop of its own");
    if (game.window == nullptr)
        return;

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(640.0f, 360.0f);
    io.IniFilename = nullptr;
    ImGui_ImplWin32_Init(game.window);

    MenuLock();
    MenuInputAttach(game.window);
    MenuUnlock();
    AttachUntilOurs(game.window, 0, 3.0);
    UINT count = 0;
    DWORD mouseFlags = 0;
    MenuInputStats s = Counts();
    Registered(&count, &mouse, &keyboard, &mouseFlags);
    Check(s.hooks == 1 && s.inputWindow != nullptr && s.registered && mouse == s.inputWindow &&
              keyboard == s.inputWindow && GetWindowThreadProcessId(s.inputWindow, nullptr) != gameThreadId &&
              GetWindowThreadProcessId(s.inputWindow, nullptr) != GetCurrentThreadId(),
          "input: open: one hook, the raw mouse and keyboard to a window of ours on a thread of its own");
    Check((mouseFlags & RIDEV_NOLEGACY) != 0, "input: open: the mouse makes no window messages (flags 0x%lX)",
          mouseFlags);
    Check(Within(2.0, [] { return Counts().probesSeen == 1; }), "input: the hook saw our own message to the window");

    // The game registers its raw mouse again while the menu is open (Gears of War E-Day did, within a second or two):
    // at the next frame ours is back, and at the close the game's newest is what goes back.
    RAWINPUTDEVICE gameMouse = {};
    gameMouse.usUsagePage = 1;
    gameMouse.usUsage = 2;
    gameMouse.hwndTarget = game.window;
    Check(RegisterRawInputDevices(&gameMouse, 1, sizeof gameMouse) != FALSE,
          "input: the game registers its raw mouse to its window");
    MenuLock();
    MenuInputAttach(game.window); // the next frame
    MenuUnlock();
    Registered(&count, &mouse, &keyboard, &mouseFlags);
    Check(Counts().retaken == 1 && mouse == s.inputWindow && (mouseFlags & RIDEV_NOLEGACY) != 0,
          "input: the game's own registration found over ours, and ours back at the next frame");

    // The game's key and button presses go to the menu, the releases to the game as well.
    PostMessageW(game.window, WM_KEYDOWN, 'A', 0x001E0001);
    PostMessageW(game.window, WM_KEYUP, 'A', LPARAM(0xC01E0001));
    PostMessageW(game.window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(20, 20));
    PostMessageW(game.window, WM_LBUTTONUP, 0, MAKELPARAM(20, 20));
    Check(Within(2.0, [&] { return game.keyUps == 1 && game.buttonUps == 1; }) && game.keyDowns == 0 &&
              game.buttonDowns == 0,
          "input: the presses stay with the menu, the releases reach the game");
    s = Counts();
    Check(s.keyMessages >= 2 && s.mouseMessages >= 2, "input: %u key and %u mouse messages through the hook",
          s.keyMessages, s.mouseMessages);

    // The raw data comes to our thread whatever the game's loop does.
    if (SendHarmless(3))
        Check(Within(2.0, [] { const MenuInputStats c = Counts(); return c.rawKeyEvents >= 6 && c.rawMouseEvents >= 6; }),
              "input: the raw keyboard and mouse data reached our window (%u key, %u mouse events)",
              Counts().rawKeyEvents, Counts().rawMouseEvents);
    else
        std::printf("skip  input: SendInput was refused (error %lu): no raw data sent\n", GetLastError());

    MenuLock();
    MenuInputRelease();
    MenuUnlock();
    UINT after = 0;
    Registered(&after, &mouse, &keyboard, &mouseFlags);
    s = Counts();
    Check(s.hooks == 0 && !s.registered && after == before + 1 && mouse == game.window && mouseFlags == 0 &&
              keyboard == nullptr,
          "input: closed: no hook, the game's newest mouse registration back, no keyboard one (%u)", after);
    gameMouse.dwFlags = RIDEV_REMOVE;
    gameMouse.hwndTarget = nullptr;
    RegisterRawInputDevices(&gameMouse, 1, sizeof gameMouse);
    Registered(&after, &mouse, &keyboard);
    Check(after == before && mouse == nullptr, "input: and none once the game removes its own");
    PostMessageW(game.window, WM_KEYDOWN, 'A', 0x001E0001);
    Check(Within(2.0, [&] { return game.keyDowns == 1; }), "input: after the close the game gets its presses");

    MenuInputDetach();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    PostMessageW(game.window, WM_CLOSE, 0, 0);
    PostThreadMessageW(gameThreadId, WM_QUIT, 0, 0);
    WaitForSingleObject(gameThread, 5000);
    CloseHandle(gameThread);
    CloseHandle(game.ready);
}

// The menu's input in a game shaped like GTA V: its raw mouse and keyboard registered with no window (the data
// follows the keyboard focus) and no flags, and its window holding the mouse capture. The menu in v0.1.3 ran the Win32
// backend's mouse handling on the game's thread, which released that capture at every click, and GTA took no mouse
// after the close.
void TestInputGame()
{
    GameWindow game = {};
    game.ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD gameThreadId = 0;
    const HANDLE gameThread = CreateThread(nullptr, 0, &GameThread, &game, 0, &gameThreadId);
    WaitForSingleObject(game.ready, 5000);
    Check(game.window != nullptr, "game input: a game window with a loop of its own");
    if (game.window == nullptr)
        return;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(640.0f, 360.0f);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // no renderer here: the atlas stays unbuilt, allowed
    io.IniFilename = nullptr;
    io.Fonts->AddFontDefault();
    ImGui_ImplWin32_Init(game.window);

    const RAWINPUTDEVICE gta[2] = { { 1, 2, 0, nullptr }, { 1, 6, 0, nullptr } };
    Check(RegisterRawInputDevices(gta, 2, sizeof(RAWINPUTDEVICE)) != FALSE,
          "game input: the game registers its raw mouse and keyboard with no window and no flags");
    const HWND held = reinterpret_cast<HWND>(SendMessageW(game.window, kGameCall, kTakeCapture, 0));
    Check(held == game.window, "game input: the game's window holds the mouse capture");

    // F24 held as the menu opens, as a key the game saw go down: the keyboard stays the game's until it is let go.
    // (SendInput can be refused, or blocked without a word when a window of higher integrity is in front.)
    INPUT f24 = {};
    f24.type = INPUT_KEYBOARD;
    f24.ki.wVk = VK_F24;
    const bool pressed = SendInput(1, &f24, sizeof f24) == 1;
    const bool keyHeld = pressed && Within(1.0, [] { return (GetAsyncKeyState(VK_F24) & 0x8000) != 0; });
    if (!keyHeld)
        std::printf("skip  game input: F24 could not be held with SendInput\n");

    AttachFrame(game.window);
    const HWND ours = Counts().inputWindow;
    HWND target = nullptr;
    DWORD flags = 0;
    bool registered = false;
    if (keyHeld)
    {
        // At once: a button held on this PC would keep the mouse waiting, and the keyboard's wait would end with it.
        AttachFrame(game.window);
        registered = RegisteredFor(6, &target, &flags); // before the check: its arguments are read first
        Check(registered && target == nullptr && flags == 0,
              "game input: open with F24 held: the raw keyboard is still the game's (window %p, flags 0x%lX)", target,
              flags);
    }
    if (pressed)
    {
        f24.ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(1, &f24, sizeof f24);
        Check(Within(1.0, [] { return (GetAsyncKeyState(VK_F24) & 0x8000) == 0; }), "game input: F24 is let go");
    }
    const HHOOK leaves = SetWindowsHookExW(WH_GETMESSAGE, &CountLeaves, nullptr, gameThreadId);
    g_mouseLeaves = 0;
    Check(leaves != nullptr, "game input: a hook of the test's counts WM_MOUSELEAVE on the game's thread");
    AttachUntilOurs(game.window, 2, 3.0);
    registered = RegisteredFor(2, &target, &flags);
    Check(ours != nullptr && registered && target == ours && (flags & RIDEV_NOLEGACY) != 0,
          "game input: open: the raw mouse is ours, with no window messages (flags 0x%lX)", flags);
    Check(AttachUntilOurs(game.window, 6, 3.0), "game input: the raw keyboard is ours%s",
          keyHeld ? " once F24 is let go" : "");

    // A click before any raw data came passes the hook, as the game's own messages: ImGui gets it, the game keeps its
    // capture, and nothing asks Windows for WM_MOUSELEAVE on its window (a hidden window is never under the cursor,
    // so a request for one would bring it at once). Raw data never moves our pointer here: no window of this process
    // is in front.
    const unsigned ups = game.buttonUps;
    PostMessageW(game.window, WM_MOUSEMOVE, 0, MAKELPARAM(30, 40));
    PostMessageW(game.window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(30, 40));
    PostMessageW(game.window, WM_LBUTTONUP, 0, MAKELPARAM(30, 40));
    Check(Within(2.0, [&] { return game.buttonUps == ups + 1; }) && game.buttonDowns == 0,
          "game input: the press stays with the menu, the release reaches the game");
    Sleep(100); // a WM_MOUSELEAVE, had one been asked for, comes at once
    Check(CaptureKept(game),
          "game input: after a click in the menu the game's window still holds its capture (%u WM_CAPTURECHANGED)",
          unsigned(game.captureChanges));
    Check(g_mouseLeaves == 0, "game input: no WM_MOUSELEAVE was asked for on the game's window (%u came)",
          unsigned(g_mouseLeaves));
    unsigned downs = 0, released = 0;
    ImVec2 at;
    QueuedMouse(0, &downs, &released, &at);
    Check(downs == 1 && released == 1 && at.x == 30.0f && at.y == 40.0f,
          "game input: ImGui was told the click at 30,40 (%u presses, %u releases, last at %.0f,%.0f)", downs, released,
          at.x, at.y);
    // Frames as DrawLocked runs them, the backend's NewFrame a stand-in for a game in front whose system cursor is away
    // from our pointer, and a wheel notch from the raw data before each: every notch must be taken in its own frame and
    // every frame must end at our pointer, with nothing left over.
    const ImVector<ImGuiInputEvent>& queue = ImGui::GetCurrentContext()->InputEventsQueue;
    for (int frame = 0; frame < 10 && (frame == 0 || queue.Size != 0); ++frame)
    {
        MenuLock(); // the click above, taken first
        MenuInputNewFrame(&StandInBackendNewFrame);
        ImGui::NewFrame();
        ImGui::EndFrame();
        MenuUnlock();
    }
    unsigned notches = 0, atOurs = 0;
    for (int frame = 0; frame < 12; ++frame)
    {
        MenuLock();
        io.AddMouseWheelEvent(0.0f, 1.0f);
        MenuInputNewFrame(&StandInBackendNewFrame);
        ImGui::NewFrame();
        notches += io.MouseWheel == 1.0f ? 1 : 0;
        atOurs += io.MousePos.x == 30.0f && io.MousePos.y == 40.0f ? 1 : 0;
        ImGui::EndFrame();
        MenuUnlock();
    }
    Check(notches == 12 && atOurs == 12 && queue.Size == 0,
          "game input: 12 frames with a notch each: %u notches taken in their frame, %u frames at our pointer, %d "
          "events left",
          notches, atOurs, queue.Size);

    // A button down for ImGui as the menu closes (a slider dragged as End closed it) is up for it at the next open.
    MenuLock();
    io.AddMouseButtonEvent(0, true);
    MenuInputNewFrame(&StandInBackendNewFrame);
    ImGui::NewFrame();
    const bool downAtClose = io.MouseDown[0];
    ImGui::EndFrame();
    MenuUnlock();

    MenuLock();
    MenuInputRelease();
    MenuUnlock();
    UnhookWindowsHookEx(leaves);
    HWND mouseTarget = nullptr, keyboardTarget = nullptr;
    DWORD mouseFlags = 0, keyboardFlags = 0;
    const bool haveMouse = RegisteredFor(2, &mouseTarget, &mouseFlags);
    const bool haveKeyboard = RegisteredFor(6, &keyboardTarget, &keyboardFlags);
    Check(haveMouse && mouseTarget == nullptr && mouseFlags == 0 && haveKeyboard && keyboardTarget == nullptr &&
              keyboardFlags == 0 && !Counts().registered,
          "game input: closed: the game's registrations are back as they were");
    Check(CaptureKept(game), "game input: closed: the game's window still holds its capture");

    // The game's registration cannot be put back (its window has gone while the menu was open): ours must not stay,
    // or with RIDEV_NOLEGACY the game would get no mouse at all.
    const HWND other = reinterpret_cast<HWND>(SendMessageW(game.window, kGameCall, kMakeWindow, 0));
    const RAWINPUTDEVICE toOther = { 1, 2, 0, other };
    Check(other != nullptr && RegisterRawInputDevices(&toOther, 1, sizeof toOther) != FALSE,
          "game input: the game registers its raw mouse to another window of its own");
    Check(AttachUntilOurs(game.window, 2, 3.0), "game input: reopened, the raw mouse is ours again");
    Check(downAtClose && !io.MouseDown[0], "game input: the button ImGui had down at the close is up at the open");
    SendMessageW(game.window, kGameCall, kDestroyWindow, reinterpret_cast<LPARAM>(other));
    const int removed = LogCount("ours was still the raw mouse registration after the close: removed");
    MenuLock();
    MenuInputRelease();
    MenuUnlock();
    registered = RegisteredFor(2, &target, &flags);
    Check(!registered && LogCount("ours was still the raw mouse registration after the close: removed") == removed + 1,
          "game input: the game's window gone, ours was removed at the close rather than left (%s)",
          registered ? "a mouse registration is left" : "no mouse registration left");

    SendMessageW(game.window, kGameCall, kDropCapture, 0);
    RAWINPUTDEVICE off[2] = { { 1, 2, RIDEV_REMOVE, nullptr }, { 1, 6, RIDEV_REMOVE, nullptr } };
    RegisterRawInputDevices(off, 2, sizeof(RAWINPUTDEVICE));
    MenuInputDetach();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    PostMessageW(game.window, WM_CLOSE, 0, 0);
    PostThreadMessageW(gameThreadId, WM_QUIT, 0, 0);
    WaitForSingleObject(gameThread, 5000);
    CloseHandle(gameThread);
    CloseHandle(game.ready);
}
} // namespace

int main()
{
    const DWORD length = GetTempPathW(MAX_PATH, g_directory);
    if (length == 0 || length + 32 >= MAX_PATH)
    {
        std::puts("no temp directory");
        return 1;
    }
    wcscat_s(g_directory, MAX_PATH, L"banana-zero-menutest\\");
    CreateDirectoryW(g_directory, nullptr);
    swprintf_s(g_ini, MAX_PATH, L"%sdlssnr.ini", g_directory);
    swprintf_s(g_logPath, MAX_PATH, L"%smenutest.log", g_directory);
    LogOpen(g_logPath);
    std::printf("menutest: files under %ls\n", g_directory);

    TestKeys();
    TestWrite();
    TestModelScaleSteps();
    TestModel();
    TestPanel();
    TestInput();
    TestInputGame();

    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    if (g_log != INVALID_HANDLE_VALUE)
    {
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
        char text[16384];
        if (ReadAll(g_logPath, text, sizeof text))
            std::printf("--- the menu's log ---\n%s", text);
    }
    return g_failed;
}
