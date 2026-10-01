#pragma once

// Settings: one immutable snapshot, published through an atomic pointer, read once per
// evaluation without a lock. dlssnr.ini beside the DLL is read at the first Neural Rendering evaluation, and read
// again whenever it changes (M4). The menu (M3) publishes a new snapshot at every step of a dragged slider
// and writes the changed keys back to the file (settings_write.h). Keys the file does not name keep their defaults;
// for the model's own parameters, that is the model's own default.

#include <cstddef>

// A model parameter as the user may have set it. `set` false: not in the file, so the model's own default.
template <typename T> struct Tunable
{
    bool set = false;
    T value {};

    bool operator==(const Tunable&) const = default;
};

enum class InputType : unsigned
{
    Auto = 0,       // follow the game's IsHDR creation flag
    LinearHdr = 1,  // linear HDR: white point, shoulder, sRGB
    ToneMapped = 2, // display-encoded already: the encode is a copy, the composite has no shoulder
};

enum class WhiteSource : unsigned
{
    Exposure = 0, // the game's pre-exposure over its exposure texture (whichever it gives), times 2^WhiteEV
    Manual = 1,   // 2^WhiteEV in the frame's own units
    Scene = 2,    // the scene meter: the frame's middle brightness 2.32 EV below the white point, times 2^WhiteEV
    Auto = 3,     // Exposure when the game gives an exposure texture, else Scene
};

enum class Preview : unsigned
{
    Off = 0,
    Input = 1,  // what the model sees
    Output = 2, // what the model made, before the composite
};

enum class Corner : unsigned
{
    TopLeft = 0,
    TopRight = 1,
    BottomLeft = 2,
    BottomRight = 3,
};

// How the menu is composited onto the swap chain (M3): by the back buffer's format and the display's mode,
// or as the user says when that guess is wrong.
enum class MenuTheme : unsigned
{
    Dark = 0,  // ImGui's dark colours, a little rounding
    Paper = 1, // warm kraft paper, clay accent
};

enum class MenuColour : unsigned
{
    Auto = 0, // 8-bit: as it is; FP16: scRGB; 10-bit with the display in HDR mode: PQ
    Sdr = 1,  // the back buffer holds display-encoded SDR whatever its format
    Hdr = 2,  // FP16: scRGB at MenuNits; 10-bit: PQ at MenuNits
};

// The model's own parameters. The model reads Preset when it creates its feature and the rest at every evaluation
// (nr_dx12.cpp, FillTunables), so a change shows from the next frame.
struct ModelSettings
{
    Tunable<unsigned> preset;      // Preset: DLSSNR.Hint.Render.Preset
    Tunable<float> intensity;      // Intensity: DLSSNR.Intensity, 0 .. 1 (the model treats more as 1)
    Tunable<unsigned> style;       // Style: DLSSNR.Style, 0 standard, 1 natural, 2 cinematic (the model reads 3+ as 2)
    Tunable<float> localStructure; // LocalStructure: DLSSNR.LocalStructureStrength
    Tunable<float> localTone;      // LocalTone: DLSSNR.LocalToneStrength
    Tunable<float> skinStructure;  // SkinStructure: DLSSNR.SkinStructureStrength, -1 = follow local structure
    Tunable<unsigned> autoMask;    // AutoMask: DLSSNR.UseAutoMask, 0 or 1; skin structure acts only with it on

    bool operator==(const ModelSettings&) const = default;
};

struct Settings
{
    unsigned generation = 0; // 0 for the built-in defaults, then 1, 2, ... for each read of the file

    bool enabled = true;       // Enabled: the whole NR pass. 0 = the game runs as if dxgi.dll only forwarded.
    float badgeSeconds = 3.0f; // Badge: the corner square for this many seconds after NR starts; 0 = never.

    ModelSettings model;

    // The HDR encode (M4).
    InputType inputType = InputType::Auto;       // InputType
    WhiteSource whiteSource = WhiteSource::Auto; // WhiteSource: auto, confirmed by the second playtest
    float whiteEV = 0.0f;                        // WhiteEV: -4 .. +4
    float shoulder = 0.70f;                      // Shoulder: 0.50 .. 0.95

    // The composite.
    float detailStrength = 1.0f;   // DetailStrength: 0 .. 2; 0 leaves the frame exactly as DLSS made it
    float colourStrength = 1.0f;   // ColourStrength: 0 .. 2
    float maxGainEV = 1.0f;        // MaxGainEV: 0 .. 2
    float highlightRestore = 0.0f; // HighlightRestore: 0 .. 1; 0 since the first playtest

    // The tuning aids.
    Preview preview = Preview::Off;       // Preview
    bool zebra = true;                    // Zebra
    Corner cardCorner = Corner::BottomLeft; // CardCorner

    // Three switches only the file can set.
    bool card = false;          // Card: the calibration card on the live frame (the menu will not store it)
    // Both off since the defaults were measured; StatsLog 2 and DumpEvery 60 measure again (tools/bzstats.py and
    // tools/bzdump.py read what they write).
    float statsSeconds = 0.0f; // StatsLog: a statistics line every this many seconds; 0 = never
    float dumpSeconds = 0.0f;  // DumpEvery: a frame dump every this many seconds, at most 16; 0 = never

    // The menu (M3), after M4's fields, which stay as they are.
    bool compare = false;                      // Compare: split screen, the original on the left, NR on the right
    float compareSplit = 50.0f;                // CompareSplit: the original's share of the width, 10 .. 90 percent
    unsigned menuKey = 0x23;                   // MenuKey: virtual-key code (keys.h); VK_END. 0 = not bound
    unsigned toggleKey = 0;                    // ToggleKey: NR on and off (A/B)
    unsigned freezeKey = 0;                    // FreezeKey: freeze the frame and open the menu; again = unfreeze
    MenuColour menuColour = MenuColour::Auto;  // MenuColour
    float menuNits = 200.0f;                   // MenuNits: the menu's white on an HDR display, 80 .. 400
    MenuTheme menuTheme = MenuTheme::Paper;    // MenuTheme: dark | paper

    // What the game's depth adds: the menu's experimental page. All of it does nothing until set.
    bool dilateMotion = false; // DilateMotion: the model gets the motion vectors dilated by depth
    float skyTone = 1.0f;      // SkyTone: Local tone on the sky, as a factor on LocalTone: 0 .. 2; 1 = as elsewhere
    float skyStructure = 1.0f; // SkyStructure: the same for Local structure
    bool showSky = false;      // ShowSky: stripes over what counts as sky (the menu will not store it)
};

// Reads the file (absent is fine: defaults), publishes the snapshot and logs what it read. Returns the snapshot.
const Settings* SettingsLoad(const wchar_t* iniPath);

// Reads the file again if its time stamp or size changed since the last read; for a background thread. True when a
// new snapshot was published. Snapshots it replaces are freed a few seconds later, when no evaluation can still be
// looking at them.
bool SettingsReloadIfChanged();

// The current snapshot; the defaults until SettingsLoad. Never null.
const Settings* SettingsCurrent();

// The menu committed (M3): publishes a copy of `settings` as the next generation and retires the one before, like a
// reload does. Returns the published snapshot. Any thread; a few at once are fine.
const Settings* SettingsPublish(const Settings& settings);

// The menu wrote the file: its new time stamp and size are noted so that SettingsReloadIfChanged does not read our
// own write back (the snapshot already holds it).
void SettingsNoteWritten();

// The path SettingsLoad was given, empty until then. Where the menu writes.
const wchar_t* SettingsPath();

// "Enabled 1, Badge 3.0 s; HDR InputType auto, ..." for the log.
void SettingsDescribe(const Settings& settings, char* out, size_t size);

// " Preset 3, Intensity 0.8" (each with a leading space), or " the model's defaults".
void SettingsDescribeModel(const ModelSettings& model, char* out, size_t size);
