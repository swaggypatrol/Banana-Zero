// The panel: what the menu shows. Runs under the menu's lock, between ImGui::NewFrame and ImGui::Render, and works
// on the model (menu.cpp). A slider publishes every step while it is dragged, so the picture follows the mouse, and
// the drag is logged once, when the mouse lets go; a typed number, a box, a choice, a right-click (back to the
// default) and "reset all" commit at once.

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "freeze.h"
#include "imgui.h"
#include "keys.h"
#include "menu.h"
#include "menu_model.h"
#include "menu_style.h"
#include "nr_dx12.h"
#include "overlay_dx12.h"
#include "settings.h"
#include "settings_write.h"

namespace
{
const Settings g_defaults;

// Where each control was drawn, for menutest.
struct ItemRect
{
    const char* key;
    ImVec2 min;
    ImVec2 max;
    unsigned frame; // the draw it was last part of: a control on a page that is not showing is not found
};
ItemRect g_items[64];
unsigned g_itemCount = 0;
unsigned g_frame = 0;

void RecordRect(const char* key, ImVec2 min, ImVec2 max)
{
    for (unsigned i = 0; i < g_itemCount; ++i)
    {
        if (strcmp(g_items[i].key, key) == 0)
        {
            g_items[i].min = min;
            g_items[i].max = max;
            g_items[i].frame = g_frame;
            return;
        }
    }
    if (g_itemCount < sizeof g_items / sizeof g_items[0])
        g_items[g_itemCount++] = { key, min, max, g_frame };
}

void Record(const char* key) { RecordRect(key, ImGui::GetItemRectMin(), ImGui::GetItemRectMax()); }

constexpr float kPi = 3.14159265f;

// ImGui's arrows are sharp triangles; the rounded look wants rounded corners on them. A triangle `height` tall
// pointing down or right, each corner replaced by an arc of `radius`, filled.
void RoundedTriangle(ImDrawList* draw, ImVec2 centre, float height, float radius, ImGuiDir dir, ImU32 colour)
{
    const float half = height * 0.5f;
    const float side = height * 0.57735f; // half the base of an equilateral triangle
    ImVec2 p[3];
    if (dir == ImGuiDir_Down)
    {
        p[0] = ImVec2(centre.x - side, centre.y - half);
        p[1] = ImVec2(centre.x + side, centre.y - half);
        p[2] = ImVec2(centre.x, centre.y + half);
    }
    else
    {
        p[0] = ImVec2(centre.x - half, centre.y - side);
        p[1] = ImVec2(centre.x + half, centre.y);
        p[2] = ImVec2(centre.x - half, centre.y + side);
    }
    draw->PathClear();
    for (int i = 0; i < 3; ++i)
    {
        const ImVec2 prev = p[(i + 2) % 3], cur = p[i], next = p[(i + 1) % 3];
        ImVec2 a(prev.x - cur.x, prev.y - cur.y), b(next.x - cur.x, next.y - cur.y);
        const float la = sqrtf(a.x * a.x + a.y * a.y), lb = sqrtf(b.x * b.x + b.y * b.y);
        a = ImVec2(a.x / la, a.y / la);
        b = ImVec2(b.x / lb, b.y / lb);
        const float theta = acosf(a.x * b.x + a.y * b.y);  // the corner's angle
        const float tangent = radius / tanf(theta * 0.5f); // from the corner to where the arc meets each edge
        ImVec2 bisector(a.x + b.x, a.y + b.y);
        const float lbis = sqrtf(bisector.x * bisector.x + bisector.y * bisector.y);
        const float toCentre = radius / sinf(theta * 0.5f);
        const ImVec2 c(cur.x + bisector.x / lbis * toCentre, cur.y + bisector.y / lbis * toCentre);
        const ImVec2 t1(cur.x + a.x * tangent, cur.y + a.y * tangent);
        const ImVec2 t2(cur.x + b.x * tangent, cur.y + b.y * tangent);
        const float a1 = atan2f(t1.y - c.y, t1.x - c.x);
        float a2 = atan2f(t2.y - c.y, t2.x - c.x);
        if (a2 - a1 > kPi)
            a2 -= 2.0f * kPi;
        if (a2 - a1 < -kPi)
            a2 += 2.0f * kPi;
        draw->PathArcTo(c, radius, a1, a2, 6);
    }
    draw->PathFillConvex(colour);
}

// The title bar's collapse arrow drawn again with rounded corners over ImGui's: its square sits a frame padding
// in from the window's corner and is a font size wide (imgui.cpp, RenderWindowDecorations and CollapseButton).
// Right after Begin, whether or not the window is collapsed.
void RoundedCollapseArrow()
{
    const ImGuiStyle& style = ImGui::GetStyle();
    const float fs = ImGui::GetFontSize();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 min(wp.x + style.FramePadding.x, wp.y + style.FramePadding.y);
    const ImVec2 max(min.x + fs, min.y + fs);
    const bool collapsed = ImGui::IsWindowCollapsed();
    const bool hovered = ImGui::IsMouseHoveringRect(min, max, false);
    const bool held = hovered && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    ImU32 background = ImGui::GetColorU32(collapsed                  ? ImGuiCol_TitleBgCollapsed
                                          : ImGui::IsWindowFocused() ? ImGuiCol_TitleBgActive
                                                                     : ImGuiCol_TitleBg);
    if (hovered)
        background = ImGui::GetColorU32(held ? ImGuiCol_ButtonActive : ImGuiCol_ButtonHovered);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRectFullScreen();
    draw->AddRectFilled(min, max, background);
    RoundedTriangle(draw, ImVec2(min.x + fs * 0.5f, min.y + fs * 0.5f), fs * 0.6f, fs * 0.09f,
                    collapsed ? ImGuiDir_Right : ImGuiDir_Down, ImGui::GetColorU32(ImGuiCol_Text));
    draw->PopClipRect();
}

// A choice: ImGui's combo without its arrow button, a rounded arrow at the right end of the frame instead.
bool ComboBox(const char* key, const char* label, int* v, const char* const* names, int count)
{
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float height = ImGui::GetFrameHeight();
    const ImVec2 max(min.x + ImGui::CalcItemWidth(), min.y + height);
    bool changed = false;
    if (ImGui::BeginCombo(label, names[*v], ImGuiComboFlags_NoArrowButton))
    {
        for (int i = 0; i < count; ++i)
        {
            if (ImGui::Selectable(names[i], i == *v))
            {
                *v = i;
                changed = true;
            }
            if (i == *v)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    RecordRect(key, min, max);
    const float fs = ImGui::GetFontSize();
    RoundedTriangle(ImGui::GetWindowDrawList(), ImVec2(max.x - height * 0.5f, min.y + height * 0.5f), fs * 0.55f,
                    fs * 0.08f, ImGuiDir_Down, ImGui::GetColorU32(ImGuiCol_Text));
    return changed;
}

// A slider with notches: `notches` positions from lo to hi, the value snapping to them; when `centred` the middle
// notch is the neutral value and the fill runs from it. No number on the slider: the value shows while the mouse
// rests on it or drags, Ctrl+click types one (not snapped), a right-click is the caller's (the default back). `text`
// stands in for the grab when the value is not set (the model's default). The grab is a rounded rectangle with the
// frame's own rounding.
struct SliderState
{
    bool edited = false;   // the value changed under the mouse this frame: a step of the drag
    bool released = false; // the mouse let go after moving it: the drag is over
    bool typed = false;    // a number was typed and entered
    bool right = false;    // right-clicked
};

bool g_cardFroze = false;  // the card froze the frame: unticking the card unfreezes it (a freeze by hand stays)
ImGuiID g_dragId = 0;      // the slider under the mouse button
bool g_dragEdited = false; // it moved
float g_typed = 0.0f;      // the number being typed
bool g_dragStep = false;   // this frame: a slider moved under the mouse (published at once, menu.h MenuDragStepLocked)
bool g_dragEnded = false;  // this frame: a slider that moved was let go (the drag is logged)

SliderState Notches(const char* key, const char* label, float* v, float lo, float hi, int notches, bool centred,
                    const char* format, const char* text, bool ghost, float typeLo)
{
    SliderState state;
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImGuiIO& io = ImGui::GetIO();
    const float fs = ImGui::GetFontSize();
    const float width = ImGui::CalcItemWidth();
    const float height = ImGui::GetFrameHeight();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + width, min.y + height);
    ImGui::PushID(key);
    ImGui::InvisibleButton("slider", ImVec2(width, height));
    RecordRect(key, min, max);
    const ImGuiID id = ImGui::GetItemID();
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    const bool showValue = active || ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal);
    const float inset = fs * 0.1f;
    const float grabWidth = fs * 1.0f;
    const float x0 = min.x + inset + grabWidth * 0.5f; // the grab's centre at lo
    const float x1 = max.x - inset - grabWidth * 0.5f; // and at hi
    const float step = (hi - lo) / float(notches - 1);
    const int middle = (notches - 1) / 2;
    auto at = [&](float value) { return x0 + (x1 - x0) * ((value - lo) / (hi - lo)); };

    if (ImGui::IsItemActivated() && io.KeyCtrl)
    {
        g_typed = *v;
        ImGui::OpenPopup("type");
    }
    const bool typing = ImGui::IsPopupOpen("type");
    if (active && !typing)
    {
        float t = (io.MousePos.x - x0) / (x1 - x0);
        t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
        const int k = int(t * float(notches - 1) + 0.5f);
        const float snapped = lo + step * float(k);
        if (g_dragId != id)
        {
            g_dragId = id;
            g_dragEdited = false;
        }
        if (snapped != *v)
        {
            *v = snapped;
            state.edited = true;
            g_dragEdited = true;
        }
    }
    if (ImGui::IsItemDeactivated() && g_dragId == id)
    {
        state.released = g_dragEdited;
        g_dragId = 0;
        g_dragEdited = false;
    }
    state.right = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right);

    // The frame, the notches, the fill, the grab.
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float rounding = style.FrameRounding;
    draw->AddRectFilled(min, max,
                        ImGui::GetColorU32(active    ? ImGuiCol_FrameBgActive
                                           : hovered ? ImGuiCol_FrameBgHovered
                                                     : ImGuiCol_FrameBg),
                        rounding);
    if (style.FrameBorderSize > 0.0f)
        draw->AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Border), rounding, style.FrameBorderSize);
    const float tick = fs >= 30.0f ? 2.0f : 1.0f;
    for (int k = 0; k < notches; ++k)
    {
        const float x = x0 + (x1 - x0) * float(k) / float(notches - 1);
        const bool neutral = centred && k == middle;
        const float tall = neutral ? fs * 0.4f : fs * 0.2f;
        const float wide = neutral ? tick * 2.0f : tick;
        draw->AddRectFilled(ImVec2(x - wide * 0.5f, max.y - inset - tall), ImVec2(x + wide * 0.5f, max.y - inset),
                            ImGui::GetColorU32(ImGuiCol_TextDisabled, neutral ? 0.9f : 0.5f));
    }
    const float gx = at(*v < lo ? lo : *v > hi ? hi : *v);
    const ImVec2 grabMin(gx - grabWidth * 0.5f, min.y + inset);
    const ImVec2 grabMax(gx + grabWidth * 0.5f, max.y - inset);
    if (ghost)
    {
        // Not set: the model's own value, drawn as an outline where it is believed to lie; a click or a drag sets it.
        draw->AddRectFilled(grabMin, grabMax, ImGui::GetColorU32(ImGuiCol_SliderGrab, 0.12f), rounding);
        draw->AddRect(grabMin, grabMax, ImGui::GetColorU32(hovered ? ImGuiCol_SliderGrabActive : ImGuiCol_SliderGrab),
                      rounding, fs >= 30.0f ? 2.0f : 1.5f);
    }
    else
    {
        const float from = centred ? at(lo + step * float(middle)) : min.x + inset;
        draw->AddRectFilled(ImVec2(from < gx ? from : gx, min.y + inset), ImVec2(from < gx ? gx : from, max.y - inset),
                            ImGui::GetColorU32(ImGuiCol_SliderGrab, 0.28f), centred ? 0.0f : rounding,
                            centred ? ImDrawFlags_None : ImDrawFlags_RoundCornersLeft);
        draw->AddRectFilled(grabMin, grabMax,
                            ImGui::GetColorU32(active ? ImGuiCol_SliderGrabActive : ImGuiCol_SliderGrab), rounding);
    }
    if (text != nullptr)
    {
        // A word in the trough ("model default", "follows local structure"), small, at the end away from the grab.
        ImFont* font = ImGui::GetFont();
        const float hintSize = fs * 0.78f; // not "small": a Windows SDK header defines that name
        const ImVec2 size = font->CalcTextSizeA(hintSize, FLT_MAX, 0.0f, text);
        float tx = max.x - inset * 3.0f - size.x;
        if (tx < grabMax.x + inset * 2.0f)
            tx = min.x + inset * 3.0f;
        draw->AddText(font, hintSize, ImVec2(tx, min.y + (height - size.y) * 0.5f),
                      ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.9f), text);
    }
    if (showValue && !typing)
    {
        if (ghost)
            ImGui::SetTooltip("%s",
                              "The model's own default. Click or drag to take over, right-click to hand it back.");
        else if (text == nullptr)
            ImGui::SetTooltip(format, double(*v));
        else
            ImGui::SetTooltip("%s", text);
    }

    if (ImGui::BeginPopup("type"))
    {
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(fs * 6.0f);
        if (ImGui::InputFloat("##value", &g_typed, 0.0f, 0.0f, "%.3f",
                              ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
        {
            *v = g_typed < typeLo ? typeLo : g_typed > hi ? hi : g_typed;
            state.typed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
    ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
    ImGui::TextUnformatted(label);
    return state;
}

bool RightClicked() { return ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right); }

// A slider's drag, for the end of the panel: each step is published as it happens, the end is logged.
void Dragged(const SliderState& state)
{
    g_dragStep |= state.edited;
    g_dragEnded |= state.released;
}

// A setting's slider: each step of a drag is published at once; a typed number commits, and so does a right-click,
// which puts the default back.
bool Float(const char* key, const char* label, float* v, float lo, float hi, int notches, bool centred, float fallback,
           const char* format)
{
    const SliderState state = Notches(key, label, v, lo, hi, notches, centred, format, nullptr, false, lo);
    Dragged(state);
    if (state.right)
        *v = fallback;
    return state.typed || state.right;
}

// A model parameter: "model default" until it is touched; a right-click makes it the model's default again. With
// `below`, a value under zero means that text (skin structure follows local structure) and can be typed as -1.
bool TunableFloat(const char* key, const char* label, Tunable<float>* t, float lo, float hi, int notches, float nominal,
                  const char* format, const char* below = nullptr)
{
    float v = t->set ? t->value : nominal;
    const char* text = !t->set ? "model default" : (below != nullptr && v < 0.0f) ? below : nullptr;
    // Centred (the fill from the middle notch) only when the nominal value is the middle one.
    const bool centred = nominal > (lo + hi) * 0.5f - 1e-6f && nominal < (lo + hi) * 0.5f + 1e-6f;
    const SliderState state =
        Notches(key, label, &v, lo, hi, notches, centred, format, text, !t->set, below != nullptr ? -1.0f : lo);
    if (state.edited || state.typed)
    {
        t->set = true;
        t->value = below != nullptr && v < 0.0f ? -1.0f : v;
    }
    Dragged(state);
    if (state.right)
        t->set = false;
    return state.typed || state.right;
}

// A model parameter picked from names: shows `fallback`, the model's own default, until it is picked; a right-click
// hands it back to the model. A value past the last name shows as the last (the model reads Style 3 and up as 2).
bool TunableChoice(const char* key, const char* label, Tunable<unsigned>* t, unsigned fallback,
                   const char* const* names, int count)
{
    int v = int(t->set ? t->value : fallback);
    if (v >= count)
        v = count - 1;
    bool commit = ComboBox(key, label, &v, names, count);
    if (commit)
    {
        t->set = true;
        t->value = unsigned(v);
    }
    if (RightClicked())
    {
        t->set = false;
        commit = true;
    }
    return commit;
}

template <typename E>
bool Choice(const char* key, const char* label, E* value, E fallback, const char* const* names, int count)
{
    int v = int(*value);
    if (v >= count)
        v = 0;
    bool commit = ComboBox(key, label, &v, names, count);
    if (commit)
        *value = E(v);
    if (RightClicked())
    {
        *value = fallback;
        commit = true;
    }
    return commit;
}

bool Bool(const char* key, const char* label, bool* v, bool fallback)
{
    bool commit = ImGui::Checkbox(label, v);
    Record(key);
    if (RightClicked())
    {
        *v = fallback;
        commit = true;
    }
    return commit;
}

// The key bindings: press the key to bind while the button waits.
int g_listening = -1;     // which binding waits: 0 menu, 1 toggle, 2 freeze
bool g_wasDown[256] = {}; // keys down when the wait began: not those

bool KeyBinding(const char* key, int slot, const char* label, unsigned* vk)
{
    char name[16];
    KeyName(*vk, name, sizeof name);
    bool commit = false;
    ImGui::PushID(key);
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetFontSize() * 9.0f);
    if (g_listening == slot)
    {
        ImGui::Button("Press a key (Esc cancels)");
        for (unsigned k = 8; k < 255; ++k)
        {
            if (k == VK_LBUTTON || k == VK_RBUTTON || k == VK_MBUTTON || k == VK_XBUTTON1 || k == VK_XBUTTON2)
                continue;
            const bool down = (GetAsyncKeyState(int(k)) & 0x8000) != 0;
            if (!down)
            {
                g_wasDown[k] = false;
                continue;
            }
            if (g_wasDown[k])
                continue;
            g_listening = -1;
            if (k != VK_ESCAPE)
            {
                *vk = k;
                commit = true;
            }
            break;
        }
    }
    else
    {
        if (ImGui::Button(name, ImVec2(ImGui::GetFontSize() * 7.0f, 0.0f)))
        {
            g_listening = slot;
            for (unsigned k = 0; k < 256; ++k)
                g_wasDown[k] = (GetAsyncKeyState(int(k)) & 0x8000) != 0;
        }
        ImGui::SameLine();
        if (ImGui::Button("Clear"))
        {
            *vk = 0;
            commit = true;
        }
    }
    Record(key);
    ImGui::PopID();
    return commit;
}

void StatusLines(const MenuModel& m, const NrStatusState& st, bool have)
{
    if (!have)
    {
        ImGui::TextDisabled("%s", "NR is idle: the game has not run DLSS yet");
        return;
    }
    if (st.off)
    {
        ImGui::TextColored(MenuToneColour(MenuTone::Warn), "NR stopped: %s",
                           st.offReason != nullptr ? st.offReason : "?");
        return;
    }
    ImGui::TextDisabled("%ux%u %s%s%s  frames %llu  failed %llu  model builds %u", st.width, st.height,
                        st.feature == 13 ? "RR" : "SR", st.hdr ? " HDR" : "", st.linear ? "" : " (tone-mapped)",
                        static_cast<unsigned long long>(st.frames), static_cast<unsigned long long>(st.failed),
                        st.creates);
    if (m.abOff)
        ImGui::TextColored(MenuToneColour(MenuTone::Attention), "%s", "A/B: showing the original, NR paused");
    else if (!m.draft.enabled)
        ImGui::TextColored(MenuToneColour(MenuTone::Attention), "%s", "NR is switched off");
    if (FreezeActive())
        ImGui::TextColored(MenuToneColour(MenuTone::Info), "%s", "Frame frozen");
    if (!st.haveFeature)
        ImGui::TextDisabled("%s", "The model is not built yet");
}

void PreviewWindow(MenuModel& m)
{
    const float fs = ImGui::GetFontSize();
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - io.DisplaySize.x / 5.0f - fs * 2.0f, fs * 2.0f),
                            ImGuiCond_FirstUseEver);
    const bool open = ImGui::Begin("Model preview", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    RoundedCollapseArrow();
    if (!open)
    {
        ImGui::End();
        return;
    }
    NrPreviewState p = {};
    const bool have = NrPreview(&p);
    if (!have || p.picture == nullptr)
        ImGui::TextDisabled("%s", "Waiting for a picture (NR has to be running)");
    else
    {
        const ImTextureID id = OverlayPreviewTexture(p.picture, p.generation, DXGI_FORMAT_R16G16B16A16_FLOAT);
        const float width = io.DisplaySize.x / 5.0f;
        if (id != ImTextureID(0) && p.width != 0)
            ImGui::Image(ImTextureRef(id), ImVec2(width, width * float(p.height) / float(p.width)));
        ImGui::TextDisabled("%s", m.draft.preview == Preview::Output ? "What the model returns, before the composite"
                                                                     : "What the model sees");
    }
    if (have && p.haveStats)
    {
        if (!p.linearHdr)
            ImGui::TextDisabled("%s", "SDR or tone-mapped input: no white point, no shoulder, no histogram");
        else
        {
            // -12 to +6 EV of the -16 .. +8 EV histogram, 1/8 EV bins.
            constexpr int kFirst = (-12 - kNrPreviewEvMin) * int(kNrPreviewBinsPerEv);
            constexpr int kCount = 18 * int(kNrPreviewBinsPerEv);
            float values[kCount];
            float top = 1.0f;
            for (int i = 0; i < kCount; ++i)
            {
                values[i] = float(p.histogram[kFirst + i]);
                if (values[i] > top)
                    top = values[i];
            }
            ImGui::PlotHistogram("##histogram", values, kCount, 0, nullptr, 0.0f, top, ImVec2(-FLT_MIN, fs * 4.0f));
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const float whiteX = min.x + (max.x - min.x) * (12.0f / 18.0f);
            const float shoulderEv = p.shoulder > 0.0f ? log2f(p.shoulder) : -12.0f;
            const float shoulderX = min.x + (max.x - min.x) * ((shoulderEv + 12.0f) / 18.0f);
            draw->AddLine(ImVec2(whiteX, min.y), ImVec2(whiteX, max.y), ImGui::GetColorU32(ImGuiCol_Text, 0.8f), 1.0f);
            draw->AddLine(ImVec2(shoulderX, min.y), ImVec2(shoulderX, max.y), MenuToneU32(MenuTone::Accent, 0.8f),
                          1.0f);
            ImGui::TextDisabled("%s", "-12 EV .. 0 (white, thin line) .. +6 EV; coloured line = shoulder start");
            const float pixels = p.pixels != 0 ? float(p.pixels) : 1.0f;
            ImGui::Text("white W = %.4g   in shoulder %.1f%%   squeezed 3+ EV %.1f%%   crushed %.1f%%", p.white,
                        100.0f * float(p.inShoulder) / pixels, 100.0f * float(p.heavy) / pixels,
                        100.0f * float(p.dark) / pixels);
        }
    }
    ImGui::End();
}
} // namespace

bool MenuPanelListening() { return g_listening >= 0; }

bool MenuItemRect(const char* key, float* x0, float* y0, float* x1, float* y1)
{
    for (unsigned i = 0; i < g_itemCount; ++i)
    {
        if (strcmp(g_items[i].key, key) != 0 || g_items[i].frame != g_frame)
            continue;
        *x0 = g_items[i].min.x;
        *y0 = g_items[i].min.y;
        *x1 = g_items[i].max.x;
        *y1 = g_items[i].max.y;
        return true;
    }
    return false;
}

void MenuDraw()
{
    ++g_frame;
    MenuModel& m = MenuModelLocked();
    Settings& d = m.draft;
    const Settings& def = g_defaults;
    const float fs = ImGui::GetFontSize();
    const ImGuiIO& io = ImGui::GetIO();
    NrStatusState st = {};
    const bool haveStatus = NrStatus(&st);
    bool commit = false;
    const char* why = "panel";
    g_dragStep = false;
    g_dragEnded = false;

    ImGui::SetNextWindowPos(ImVec2(fs * 2.0f, fs * 2.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(fs * 36.0f, 0.0f), ImVec2(fs * 36.0f, io.DisplaySize.y * 0.9f));
    const bool open = ImGui::Begin("Banana-Zero Neural Rendering", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    RoundedCollapseArrow();
    if (!open)
    {
        ImGui::End();
        return;
    }
    ImGui::PushItemWidth(fs * 13.0f);
    StatusLines(m, st, haveStatus);

    // The row that is always there: NR on, A/B, the freeze.
    commit |= Bool("Enabled", "NR on", &d.enabled, def.enabled);
    ImGui::SameLine(fs * 8.0f);
    if (ImGui::Button(m.abOff ? "A/B: back to NR" : "A/B: show original"))
    {
        m.abOff = !m.abOff;
        commit = true;
        why = m.abOff ? "A/B off" : "A/B on";
    }
    Record("AB");
    ImGui::SameLine(fs * 19.0f);
    {
        bool frozen = FreezeWanted();
        if (!frozen)
            g_cardFroze = false; // gone by the key, a size change or the box: nothing for the card to undo
        if (ImGui::Checkbox("Freeze frame", &frozen))
        {
            FreezeRequest(frozen);
            g_cardFroze = false; // the user's now, either way
            if (!frozen && d.card)
            {
                d.card = false;
                commit = true;
            }
        }
        Record("Freeze");
    }

    // The pages: the tuning, the aids, the keys, the menu itself. Each fits a 1080-line frame.
    if (ImGui::BeginTabBar("##pages", ImGuiTabBarFlags_NoTooltip))
    {
        const bool tune = ImGui::BeginTabItem("Tuning");
        Record("TabTune");
        if (tune)
        {
            // Three things are taste: how much of the model's picture reaches the frame, whether its colour comes
            // along, and the model's style. Everything else waits under a header that opens closed: the composite
            // keeps the frame's exposure by construction (a ratio per pixel, capped at MaxGainEV) and the M4
            // measurements (567 statistics lines over two games, model defaults) put the model's median luminance
            // change within 0.05 EV in 441 of them and within 0.15 EV in 559, so nothing there needs a hand in
            // normal use. The ranges are the usable ones: the file still takes the wider ones in settings.cpp, and
            // a wider value shows at the slider's end until touched.
            ModelSettings& mm = d.model;
            commit |= Float("DetailStrength", "Strength", &d.detailStrength, 0.0f, 1.5f, 25, false, def.detailStrength,
                            "%.2f");
            commit |=
                Float("ColourStrength", "Colour", &d.colourStrength, 0.0f, 1.5f, 25, false, def.colourStrength, "%.2f");
            {
                // Three, as the model has (its default is Standard).
                const char* const styles[] = { "Standard", "Natural", "Cinematic" };
                commit |= TunableChoice("Style", "Style", &mm.style, 0, styles, 3);
            }

            const bool advanced = ImGui::CollapsingHeader("Advanced (rarely needed)");
            Record("Advanced");
            if (advanced)
            {
                // The model's other parameters. It reads them at every evaluation, so a change shows on the next frame.
                // Model intensity blends the model's result with its input inside the model and does nothing above
                // 1. Skin structure reaches the network only with the auto mask on (with it off, the model passes -1
                // for both the skin and the other areas), so it is greyed out without. No Preset: it picks a set of
                // network weights and the model DLL (310.8) embeds exactly one (WEIGHTS_HT; any other number falls
                // back to it inside the model), so every value is the same model. The ini key stays for a model
                // build that ships several.
                ImGui::SeparatorText("Model");
                commit |= TunableFloat("Intensity", "Model intensity", &mm.intensity, 0.0f, 1.0f, 21, 1.0f, "%.2f");
                commit |=
                    TunableFloat("LocalStructure", "Local structure", &mm.localStructure, 0.0f, 1.5f, 25, 1.0f, "%.2f");
                commit |= TunableFloat("LocalTone", "Local tone", &mm.localTone, 0.0f, 1.5f, 25, 1.0f, "%.2f");
                // While the sky sliders (the depth page) are in use, the model gets a control mask, and with one it
                // keeps its own auto mask off (the teardown): then neither the auto mask nor skin structure reaches it.
                const bool skyMask = d.skyTone != 1.0f || d.skyStructure != 1.0f;
                ImGui::BeginDisabled(skyMask);
                {
                    const char* const masks[] = { "Off", "On" };
                    commit |= TunableChoice("AutoMask", skyMask ? "Auto mask (off: sky sliders in use)" : "Auto mask",
                                            &mm.autoMask, 0, masks, 2);
                }
                ImGui::EndDisabled();
                const bool skin = !skyMask && mm.autoMask.set && mm.autoMask.value != 0;
                ImGui::BeginDisabled(!skin);
                commit |= TunableFloat("SkinStructure",
                                       skin      ? "Skin structure"
                                       : skyMask ? "Skin structure (off: sky sliders in use)"
                                                 : "Skin structure (needs Auto mask)",
                                       &mm.skinStructure, 0.0f, 1.5f, 25, -1.0f, "%.2f", "follows local structure");
                ImGui::EndDisabled();

                // The HDR encode (M4). InputType is ini-only: auto follows the game's IsHDR flag and both games
                // are right.
                ImGui::SeparatorText("HDR encode");
                {
                    const char* const whites[] = { "Game exposure", "Manual", "Scene meter", "Auto" };
                    commit |= Choice("WhiteSource", "White point from", &d.whiteSource, def.whiteSource, whites, 4);
                    if (haveStatus && d.whiteSource == WhiteSource::Auto)
                        ImGui::TextDisabled("%s", st.exposureTexture
                                                      ? "Auto is using the game's exposure texture"
                                                      : "Auto is using the scene meter (no exposure from the game)");
                    else if (haveStatus && d.whiteSource == WhiteSource::Exposure && !st.exposureTexture &&
                             !st.havePreExposure)
                        ImGui::TextDisabled("%s", "No exposure from the game, so this works like Manual");
                }
                commit |= Float("WhiteEV", "White point (EV)", &d.whiteEV, -2.0f, 2.0f, 25, true, def.whiteEV, "%+.2f");
                commit |=
                    Float("Shoulder", "Highlight shoulder", &d.shoulder, 0.5f, 0.9f, 21, true, def.shoulder, "%.2f");

                // The composite (M4).
                ImGui::SeparatorText("Composite");
                commit |=
                    Float("MaxGainEV", "Max gain (EV)", &d.maxGainEV, 0.0f, 1.5f, 25, false, def.maxGainEV, "%.2f");
                commit |= Float("HighlightRestore", "Highlight restore", &d.highlightRestore, 0.0f, 1.0f, 25, false,
                                def.highlightRestore, "%.2f");
            }
            ImGui::EndTabItem();
        }

        // The aids.
        const bool aids = ImGui::BeginTabItem("Compare");
        Record("TabCompare");
        if (aids)
        {
            commit |= Bool("Compare", "Split screen (original left, NR right)", &d.compare, def.compare);
            if (d.compare)
                commit |= Float("CompareSplit", "Divider", &d.compareSplit, 10.0f, 90.0f, 17, true, def.compareSplit,
                                "%.0f%%");
            const char* const previews[] = { "Off", "Model input", "Model output" };
            commit |= Choice("Preview", "Preview window", &d.preview, def.preview, previews, 3);
            commit |= Bool("Zebra", "Zebra stripes", &d.zebra, def.zebra);
            bool card = d.card;
            if (ImGui::Checkbox("Calibration card (freezes the frame while on)", &card))
            {
                d.card = card;
                if (card && !FreezeWanted())
                {
                    FreezeRequest(true);
                    g_cardFroze = true;
                }
                else if (!card && g_cardFroze)
                {
                    FreezeRequest(false);
                    g_cardFroze = false;
                }
                commit = true;
            }
            Record("Card");
            const char* const corners[] = { "Top left", "Top right", "Bottom left", "Bottom right" };
            commit |= Choice("CardCorner", "Card corner", &d.cardCorner, def.cardCorner, corners, 4);
            if (haveStatus && !st.linear)
                ImGui::TextDisabled("%s",
                                    "SDR or tone-mapped input: the card, zebra and histogram have nothing to say");
            ImGui::EndTabItem();
        }

        // What the game's depth adds, as experiments to judge by eye. The motion vectors dilated by depth: in
        // nrprobe's T7 worse right on thin moving things and better over the rest of the frame. The sky's own Local
        // tone and Local structure, through a control mask: in T8 the mask does exactly what the sliders do, and
        // what one part of the picture is given moves the rest by about a fifth as much.
        const bool depthPage = ImGui::BeginTabItem("Depth (experimental)");
        Record("TabDepth");
        if (depthPage)
        {
            ImGui::SeparatorText("Motion vectors");
            commit |= Bool("DilateMotion", "Dilate by depth", &d.dilateMotion, def.dilateMotion);
            ImGui::TextDisabled("%s", "Moving edges take the motion of what is in front. Judge it in motion.");

            ImGui::SeparatorText("Sky");
            commit |= Float("SkyTone", "Sky local tone", &d.skyTone, 0.0f, 1.5f, 25, false, def.skyTone, "%.2f");
            commit |= Float("SkyStructure", "Sky local structure", &d.skyStructure, 0.0f, 1.5f, 25, false,
                            def.skyStructure, "%.2f");
            commit |= Bool("ShowSky", "Show what counts as sky (purple stripes)", &d.showSky, def.showSky);
            ImGui::TextDisabled("%s", "1 = as the rest of the picture (times Local tone / Local structure).");
            ImGui::TextDisabled("%s", "Sky = the far end of the game's depth. Stripes are never saved.");
            if ((d.skyTone != 1.0f || d.skyStructure != 1.0f) && d.model.autoMask.set && d.model.autoMask.value != 0)
                ImGui::TextColored(MenuToneColour(MenuTone::Attention), "%s",
                                   "Auto mask is off while either sky slider is away from 1");
            ImGui::EndTabItem();
        }

        // The model on a smaller copy of the frame (ModelScale), after DLSS as always: its time follows its pixels
        // (nrprobe on the RTX 5090 at 4K: 7.1 ms whole, 4.4 at 75%, 3.8 at 67%, 2.9 at 50%), and its change comes
        // back to the full frame along the frame's own edges (nr_fit.hlsl). 100% is the model on the whole frame.
        const bool speedPage = ImGui::BeginTabItem("Speed (experimental)");
        Record("TabSpeed");
        if (speedPage)
        {
            commit |= Float("ModelScale", "Model input size", &d.modelScale, 50.0f, 100.0f, 11, false, def.modelScale,
                            "%.0f%%");
            ImGui::TextDisabled("%s", "Below 100% the model works on a smaller copy of the frame, and what it");
            ImGui::TextDisabled("%s", "changes is carried back to full size along the frame's own edges.");
            if (haveStatus && st.modelWidth != 0)
            {
                if (st.modelWidth == st.width && st.modelHeight == st.height)
                    ImGui::Text("Model input: the whole %ux%u frame", st.width, st.height);
                else
                    ImGui::Text("Model input: %ux%u of %ux%u", st.modelWidth, st.modelHeight, st.width, st.height);
            }
            if (haveStatus && st.haveTiming)
                ImGui::Text("NR on the GPU: %.2f ms, of which the model %.2f ms", double(st.gpuMs), double(st.modelMs));
            else
                ImGui::TextDisabled("%s", "NR on the GPU: no measurement yet");
            ImGui::EndTabItem();
        }

        // The keys.
        const bool keys = ImGui::BeginTabItem("Keys");
        Record("TabKeys");
        if (keys)
        {
            commit |= KeyBinding("MenuKey", 0, "Menu", &d.menuKey);
            commit |= KeyBinding("ToggleKey", 1, "A/B toggle", &d.toggleKey);
            commit |= KeyBinding("FreezeKey", 2, "Freeze frame", &d.freezeKey);
            ImGui::EndTabItem();
        }

        // The menu itself.
        const bool menu = ImGui::BeginTabItem("Menu");
        Record("TabMenu");
        if (menu)
        {
            {
                const char* const themes[] = { "Dark", "Kraft paper" };
                commit |= Choice("MenuTheme", "Theme", &d.menuTheme, def.menuTheme, themes, 2);
            }
            {
                const char* const colours[] = { "Auto", "SDR", "HDR" };
                commit |= Choice("MenuColour", "Menu colour", &d.menuColour, def.menuColour, colours, 3);
            }
            commit |= Float("MenuNits", "Menu brightness (nits, HDR)", &d.menuNits, 80.0f, 400.0f, 33, false,
                            def.menuNits, "%.0f");
            commit |= Float("Badge", "Green badge (seconds)", &d.badgeSeconds, 0.0f, 10.0f, 21, false, def.badgeSeconds,
                            "%.1f");
            ImGui::TextDisabled("Overlay: %s", OverlayState());
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    if (ImGui::Button("Reset all"))
        ImGui::OpenPopup("Reset everything?");
    Record("Defaults");
    ImGui::SameLine();
    const bool close = ImGui::Button("Close menu");
    Record("Close");
    if (close)
        MenuCloseLocked();
    if (ImGui::BeginPopupModal("Reset everything?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::TextUnformatted(
            "Every tuning knob goes back to factory settings.\nYour keys and the menu's look stay put.");
        if (ImGui::Button("Reset", ImVec2(fs * 6.0f, 0.0f)))
        {
            Settings reset = def;
            reset.generation = d.generation;
            reset.menuKey = d.menuKey;
            reset.toggleKey = d.toggleKey;
            reset.freezeKey = d.freezeKey;
            reset.menuColour = d.menuColour;
            reset.menuNits = d.menuNits;
            reset.menuTheme = d.menuTheme;
            reset.statsSeconds = d.statsSeconds;
            reset.dumpSeconds = d.dumpSeconds;
            d = reset;
            commit = true;
            why = "all defaults";
            ImGui::CloseCurrentPopup();
        }
        Record("DefaultsYes");
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(fs * 6.0f, 0.0f)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopItemWidth();
    ImGui::End();

    if (commit)
        MenuCommitLocked(why); // a step of a drag in the same frame goes with it
    else if (g_dragStep)
        MenuDragStepLocked();
    if (g_dragEnded)
        MenuDragEndedLocked();
    if (d.preview != Preview::Off)
        PreviewWindow(m);
}
