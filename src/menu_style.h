#pragma once

// The menu's look: ImGui's style for a theme at a scale, and the few colours the panel uses for
// words of its own (warnings, notes, the lines over the histogram), chosen to suit the theme. One place, so that
// the overlay in the game and the picture tool in the cloud draw the same thing.

#include "imgui.h"
#include "settings.h"

enum class MenuTone
{
    Warn,      // NR stopped, the model refused
    Attention, // A/B, disabled, rebuilding
    Info,      // frozen, the white-point line
    Accent     // the shoulder line
};

// Fills ImGui's style for `theme` at `scale` (1 at 1080 lines, 2 at 2160). Under the menu's lock, before NewFrame.
void MenuStyleApply(MenuTheme theme, float scale);

// A colour for the panel's own text, for the theme last applied (the dark one until then).
ImVec4 MenuToneColour(MenuTone tone);
ImU32 MenuToneU32(MenuTone tone, float alpha = 1.0f);
