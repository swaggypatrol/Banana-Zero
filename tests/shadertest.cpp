// shadertest: runs the NR shaders (src/nr.hlsl, src/nr_stats.hlsl and src/nr_fit.hlsl, as the build embeds them in
// nr_shader.h, nr_stats_shader.h and nr_fit_shader.h) on this PC's D3D12 device over synthetic frames, and checks what
// they write against a CPU copy of their formulas. No game, no model, nothing written outside this process.
//
//   build\Release\shadertest.exe
//
// For the two Output formats the games use (R16G16B16A16_FLOAT, R11G11B10_FLOAT), and a display-encoded one
// (R8G8B8A8_UNORM), it checks:
//   - the encode: white point (with and without an exposure texture), shoulder, sRGB; a copy for display-encoded input
//   - that a model returning its input, and DetailStrength 0, leave the Output bit for bit as the game made it
//   - the composite for a model that brightens by half a stop, one that brightens by three (the MaxGainEV clamp) and
//     one with HighlightRestore at work, against the CPU formula, and that pixels outside the frame are left alone
//   - the calibration card: the same in the model's input whatever the white point, and its value times the white
//     point in the Output
//   - the statistics: counts, histograms, and the frame stamps at both ends
//   - the scene meter: the white point it works out afresh, eased from an older one, and kept when there is nothing
//     to measure, against the CPU from the same histogram and from the sorted pixels themselves
//   - the frame dump: header, closing word, pictures, and the zebra classes in the proxy's alpha
//   - the preview's zebra stripes
//   - ModelScale, the model on a smaller copy of the frame: the encode's area average (nothing written beyond the
//     model's picture), the fit, the composite that brings the change back to the frame's size, a model returning its
//     input and DetailStrength 0 still leaving the Output bit for bit, and a uniform change arriving as that change
// Formats whose typed UAV loads this GPU lacks are skipped, as dxgi.dll skips them. Then, for every depth format
// dxgi.dll reads (typeless and fully typed, drawn as a depth-stencil texture), with the depth inverted and not:
//   - the sky's control mask
//   - the motion vectors dilated by depth, at the render and at the display resolution, inside larger textures
//   - Show sky's stripes in the composite, and every other pixel left as it was
// And, first, the game's compute state dxgi.dll binds again after its pass (src/list_state.cpp), as it learns it
// from this process's own lists on the D3D12 runtime the games use. Everything after runs through those hooks.
// The exit code is the number of failed checks.

#include <windows.h>

#include <d3d12.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

#include "list_state.h"
#include "nr_fit_shader.h"
#include "nr_shader.h"
#include "nr_shared.h"
#include "nr_stats_shader.h"

namespace
{
int g_checks = 0;
int g_failed = 0;

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

void Say(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

[[noreturn]] void Die(const char* what, HRESULT hr)
{
    Say("FAIL  %s: 0x%08lX", what, static_cast<unsigned long>(hr));
    std::exit(100);
}

void Must(HRESULT hr, const char* what)
{
    if (FAILED(hr))
        Die(what, hr);
}

// ---------------------------------------------------------------------------------------------------------------
// Number formats.

float HalfToFloat(uint16_t h)
{
    const uint32_t exponent = (h >> 10) & 0x1F;
    const uint32_t mantissa = h & 0x3FF;
    float value;
    if (exponent == 0)
        value = std::ldexp(float(mantissa), -24);
    else if (exponent == 31)
        value = mantissa == 0 ? INFINITY : NAN;
    else
        value = std::ldexp(float(mantissa | 0x400), int(exponent) - 25);
    return (h & 0x8000) != 0 ? -value : value;
}

// Round to nearest even, as the GPU stores.
uint16_t FloatToHalf(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    const uint16_t sign = uint16_t((bits >> 16) & 0x8000);
    const uint32_t magnitude = bits & 0x7FFFFFFF;
    if (magnitude > 0x7F800000)
        return uint16_t(sign | 0x7E00);
    if (magnitude >= 0x477FF000) // 65520 and up round to infinity
        return uint16_t(sign | 0x7C00);
    if (magnitude < 0x38800000) // below the smallest normal half: a multiple of 2^-24
        return uint16_t(sign | uint16_t(std::nearbyint(std::fabs(value) * 16777216.0f)));
    uint32_t half = ((magnitude >> 23) - 112) << 10 | ((magnitude & 0x7FFFFF) >> 13);
    const uint32_t rest = magnitude & 0x1FFF;
    if (rest > 0x1000 || (rest == 0x1000 && (half & 1) != 0))
        ++half; // a carry into the exponent is still right
    return uint16_t(sign | half);
}

// The unsigned small floats of R11G11B10_FLOAT: 5 exponent bits (bias 15), 6 or 5 mantissa bits, no sign.
float SmallToFloat(uint32_t bits, int mantissaBits)
{
    const uint32_t mantissa = bits & ((1u << mantissaBits) - 1);
    const uint32_t exponent = bits >> mantissaBits;
    if (exponent == 0)
        return std::ldexp(float(mantissa), -14 - mantissaBits);
    if (exponent == 31)
        return mantissa == 0 ? INFINITY : NAN;
    return std::ldexp(float(mantissa | (1u << mantissaBits)), int(exponent) - 15 - mantissaBits);
}

uint32_t FloatToSmall(float value, int mantissaBits)
{
    if (std::isnan(value))
        return (31u << mantissaBits) | 1u;
    if (!(value > 0.0f))
        return 0;
    if (std::isinf(value))
        return 31u << mantissaBits;
    int exponent = 0;
    const float fraction = std::frexp(value, &exponent); // value = fraction 2^exponent, fraction in [0.5, 1)
    const int biased = exponent - 1 + 15;
    uint32_t result;
    if (biased <= 0)
        result = uint32_t(std::nearbyint(std::ldexp(value, 14 + mantissaBits)));
    else
        result = (uint32_t(biased) << mantissaBits) +
                 uint32_t(std::nearbyint(std::ldexp(2.0f * fraction - 1.0f, mantissaBits)));
    const uint32_t largest = (30u << mantissaBits) | ((1u << mantissaBits) - 1);
    return result > largest ? largest : result;
}

struct Pixel
{
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
};

struct Format
{
    DXGI_FORMAT dxgi;
    const char* name;
    UINT bytes;  // per pixel
    bool linear; // linear HDR, as the games give it; else display-encoded
};

const Format kHalf = { DXGI_FORMAT_R16G16B16A16_FLOAT, "R16G16B16A16_FLOAT", 8, true };
const Format kSmall = { DXGI_FORMAT_R11G11B10_FLOAT, "R11G11B10_FLOAT", 4, true };
const Format kUnorm = { DXGI_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM", 4, false };

uint8_t ToUnorm8(float v) { return uint8_t(std::nearbyint((std::isnan(v) ? 0.0f : std::clamp(v, 0.0f, 1.0f)) * 255.0f)); }

void Encode(const Format& f, const Pixel& p, uint8_t* out)
{
    if (f.dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT)
    {
        const uint16_t h[4] = { FloatToHalf(p.r), FloatToHalf(p.g), FloatToHalf(p.b), FloatToHalf(p.a) };
        std::memcpy(out, h, 8);
    }
    else if (f.dxgi == DXGI_FORMAT_R11G11B10_FLOAT)
    {
        const uint32_t bits = FloatToSmall(p.r, 6) | FloatToSmall(p.g, 6) << 11 | FloatToSmall(p.b, 5) << 22;
        std::memcpy(out, &bits, 4);
    }
    else
    {
        out[0] = ToUnorm8(p.r);
        out[1] = ToUnorm8(p.g);
        out[2] = ToUnorm8(p.b);
        out[3] = ToUnorm8(p.a);
    }
}

Pixel Decode(const Format& f, const uint8_t* in)
{
    if (f.dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT)
    {
        uint16_t h[4];
        std::memcpy(h, in, 8);
        return { HalfToFloat(h[0]), HalfToFloat(h[1]), HalfToFloat(h[2]), HalfToFloat(h[3]) };
    }
    if (f.dxgi == DXGI_FORMAT_R11G11B10_FLOAT)
    {
        uint32_t bits;
        std::memcpy(&bits, in, 4);
        return { SmallToFloat(bits & 0x7FF, 6), SmallToFloat((bits >> 11) & 0x7FF, 6), SmallToFloat(bits >> 22, 5), 1.0f };
    }
    return { in[0] / 255.0f, in[1] / 255.0f, in[2] / 255.0f, in[3] / 255.0f };
}

Pixel Quantise(const Format& f, const Pixel& p)
{
    uint8_t bytes[8];
    Encode(f, p, bytes);
    return Decode(f, bytes);
}

// Within two steps of the format (the GPU's float maths and ours differ by far less than one), NaN matching NaN.
bool Close(const Format& f, int channel, float got, float want)
{
    if (std::isnan(got) || std::isnan(want))
        return std::isnan(got) && std::isnan(want);
    if (std::isinf(got) || std::isinf(want))
        return got == want;
    float relative, absolute;
    if (f.dxgi == DXGI_FORMAT_R16G16B16A16_FLOAT)
        relative = 1.0f / 512.0f, absolute = 1.2e-7f;
    else if (f.dxgi == DXGI_FORMAT_R11G11B10_FLOAT)
        relative = channel == 2 ? 1.0f / 16.0f : 1.0f / 32.0f, absolute = 4.0e-6f;
    else
        relative = 0.0f, absolute = 1.5f / 255.0f;
    return std::fabs(got - want) <= std::max(absolute, relative * std::fabs(want));
}

bool ClosePixel(const Format& f, const Pixel& got, const Pixel& want)
{
    return Close(f, 0, got.r, want.r) && Close(f, 1, got.g, want.g) && Close(f, 2, got.b, want.b);
}

// ---------------------------------------------------------------------------------------------------------------
// The CPU copy of nr_common.hlsli and nr.hlsl. Same operations in the same order, in float.

namespace ref
{
struct F3
{
    float r, g, b;
};
F3 operator+(F3 a, F3 b) { return { a.r + b.r, a.g + b.g, a.b + b.b }; }
F3 operator-(F3 a, F3 b) { return { a.r - b.r, a.g - b.g, a.b - b.b }; }
F3 operator*(F3 a, float s) { return { a.r * s, a.g * s, a.b * s }; }
F3 operator/(F3 a, float s) { return { a.r / s, a.g / s, a.b / s }; }

const F3 kLuma = { 0.2126f, 0.7152f, 0.0722f };
const float kGainFloor = 1.0f / 4096.0f;
const float kChromaFloor = 1.0f / 16384.0f;
const float kMaxU = 1.0f - 1.0f / 4096.0f;
const float kMaxOutput = 65024.0f;
const float kHeavyT = 1.8284271f;
const float kHeavyU = 0.64644661f;

float Dot(F3 a, F3 b) { return a.r * b.r + a.g * b.g + a.b * b.b; }
float Max(float a, float b) { return std::isnan(a) ? b : std::isnan(b) ? a : (a > b ? a : b); } // HLSL max
float Min(float a, float b) { return std::isnan(a) ? b : std::isnan(b) ? a : (a < b ? a : b); }
float Max3(F3 v) { return Max(v.r, Max(v.g, v.b)); }
float Saturate(float v) { return std::isnan(v) ? 0.0f : Min(Max(v, 0.0f), 1.0f); }
F3 Saturate(F3 v) { return { Saturate(v.r), Saturate(v.g), Saturate(v.b) }; }
float Clean1(float v) { return std::isnan(v) ? 0.0f : Min(Max(v, 0.0f), 1.0e30f); }
F3 Clean(F3 c) { return { Clean1(c.r), Clean1(c.g), Clean1(c.b) }; }
float SrgbEncode1(float v) { return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f; }
F3 SrgbEncode(F3 v) { return { SrgbEncode1(v.r), SrgbEncode1(v.g), SrgbEncode1(v.b) }; }
float SrgbDecode1(float e) { return e <= 0.04045f ? e / 12.92f : std::pow((e + 0.055f) / 1.055f, 2.4f); }
F3 SrgbDecode(F3 e) { return { SrgbDecode1(e.r), SrgbDecode1(e.g), SrgbDecode1(e.b) }; }

float WhitePoint(float whiteScale, bool exposureFlag, float exposure)
{
    float white = whiteScale;
    if (exposureFlag && exposure > 1.0e-20f && exposure < 1.0e20f)
        white /= exposure;
    return white > 1.0e-20f && white < 1.0e20f ? white : 1.0f;
}

float Shoulder(float m, float s)
{
    if (m <= s)
        return m;
    const float t = (m - s) / (1.0f - s);
    return s + (1.0f - s) * (t / (1.0f + t));
}

float ShoulderInverse(float y, float s)
{
    if (y <= s)
        return y;
    const float u = Min((y - s) / (1.0f - s), kMaxU);
    return s + (1.0f - s) * (u / (1.0f - u));
}

float InverseScale(float y, float s) { return y > s ? ShoulderInverse(y, s) / Max(y, 1.0e-20f) : 1.0f; }
float RestoreScale(float y, float s) { return InverseScale(Min(y, s + kHeavyU * (1.0f - s)), s); }

F3 EncodeLinear(F3 frame, float white, float s)
{
    const F3 x = Clean(frame) / white;
    const float m = Max3(x);
    return m > s ? x * (Shoulder(m, s) / m) : x;
}

F3 ProxyLinear(F3 e) { return SrgbDecode(Saturate(e)); }
F3 ModelLinear(F3 e, F3 pe)
{
    const F3 c = { std::isnan(e.r) ? pe.r : e.r, std::isnan(e.g) ? pe.g : e.g, std::isnan(e.b) ? pe.b : e.b };
    return SrgbDecode({ Min(Max(c.r, 0.0f), 2.0f), Min(Max(c.g, 0.0f), 2.0f), Min(Max(c.b, 0.0f), 2.0f) });
}

struct Knobs
{
    float shoulder = 0.7f, detail = 1.0f, colour = 1.0f, maxGain = 1.0f, highlight = 0.0f;
};

float ModelGain(F3 p, F3 o, bool linear, const Knobs& k)
{
    const float yp = Dot(p, kLuma);
    const float yo = Dot(o, kLuma);
    float gain = yo == yp ? 0.0f : std::log2((yo + kGainFloor) / (yp + kGainFloor));
    if (linear && k.highlight != 0.0f)
    {
        const float mp = Max3(p);
        const float mo = Max3(o);
        if (mp > k.shoulder || mo > k.shoulder)
            gain += k.highlight * (std::log2(RestoreScale(mo, k.shoulder)) - std::log2(RestoreScale(mp, k.shoulder)));
    }
    return gain;
}

F3 ModelChroma(F3 p, F3 o)
{
    const float yp = Dot(p, kLuma);
    const float yo = Dot(o, kLuma);
    return yp > kChromaFloor && yo > kChromaFloor ? o / yo - p / yp : F3 { 0.0f, 0.0f, 0.0f };
}

F3 ApplyChange(F3 frame, float change, F3 chroma, bool linear, const Knobs& k)
{
    const float gain = Min(Max(change * k.detail, -k.maxGain), k.maxGain);
    const F3 c = linear ? frame : SrgbDecode(Saturate(frame));
    const F3 scaled = c * std::exp2(gain);
    const F3 positive = { Max(scaled.r, 0.0f), Max(scaled.g, 0.0f), Max(scaled.b, 0.0f) };
    F3 result = scaled + chroma * (k.detail * k.colour * Dot(positive, kLuma));
    result = { Min(Max(result.r, Min(scaled.r, 0.0f)), Max(c.r, kMaxOutput)),
               Min(Max(result.g, Min(scaled.g, 0.0f)), Max(c.g, kMaxOutput)),
               Min(Max(result.b, Min(scaled.b, 0.0f)), Max(c.b, kMaxOutput)) };
    return linear ? result : SrgbEncode(Saturate(result));
}

F3 Composite(F3 frame, F3 pe, F3 me, bool linear, const Knobs& k)
{
    const F3 p = ProxyLinear(pe);
    const F3 o = ModelLinear(me, pe);
    return ApplyChange(frame, ModelGain(p, o, linear, k), ModelChroma(p, o), linear, k);
}

// ModelScale: the light the model's input holds for a frame value, the guide, the colour change from its red and blue.
F3 ProxyLight(F3 frame, float white, bool linear, float s)
{
    if (!linear)
        return SrgbDecode(Saturate(Clean(frame)));
    const F3 e = EncodeLinear(frame, white, s);
    return { Min(e.r, 1.0f), Min(e.g, 1.0f), Min(e.b, 1.0f) };
}
float Guide(F3 p) { return std::log2(Dot(p, kLuma) + kGainFloor); }
F3 ChromaFromRedBlue(float r, float b) { return { r, -(kLuma.r * r + kLuma.b * b) / kLuma.g, b }; }
const float kFitEps = 0.02f; // nr_fit.hlsl

unsigned ZebraClass(F3 pe, bool linear, float s)
{
    if (!linear)
        return 0;
    const float m = Max3(SrgbDecode(Saturate(pe)));
    if (m > s + kHeavyU * (1.0f - s))
        return 2;
    return m > s ? 1 : 0;
}

struct Rect
{
    unsigned x, y, w, h;
};

bool InCard(unsigned qx, unsigned qy, const Rect& r) { return qx - r.x < r.w && qy - r.y < r.h; }

F3 Card(unsigned qx, unsigned qy, const Rect& r)
{
    const float px = float(qx - r.x) + 0.5f, py = float(qy - r.y) + 0.5f;
    const float w = float(r.w), h = float(r.h);
    const float gap = Max(2.0f, std::floor(w / 240.0f));
    const float row1 = std::floor(h * 0.40f);
    const float row2 = std::floor(h * 0.70f);
    float x0, x1, y0, y1;
    F3 value;
    if (py < row1)
    {
        const float cell = w / 13.0f;
        const float i = Min(std::floor(px / cell), 12.0f);
        x0 = i * cell;
        x1 = x0 + cell;
        y0 = 0.0f;
        y1 = row1;
        const float ev = i - 8.0f;
        const float v = std::exp2(ev);
        value = { v, v, v };
        if (ev >= 1.0f)
        {
            const float cx = (x0 + x1) * 0.5f, cy = (y0 + y1) * 0.5f;
            const float radius = 0.30f * Min(cell, row1);
            const float d1x = px - cx, d1y = py - cy;
            const float d2x = px - (cx + 0.30f * radius), d2y = py - (cy + -0.30f * radius);
            if (d1x * d1x + d1y * d1y < radius * radius && d2x * d2x + d2y * d2y >= 0.64f * radius * radius)
                value = value * std::exp2(1.0f / 3.0f);
        }
    }
    else if (py < row2)
    {
        const float cell = w / 3.0f;
        const float i = Min(std::floor(px / cell), 2.0f);
        x0 = i * cell;
        x1 = x0 + cell;
        y0 = row1;
        y1 = row2;
        if (i < 0.5f)
            value = { 0.18f, 0.18f, 0.18f };
        else if (i < 1.5f)
            value = { 1.0f, 1.0f, 1.0f };
        else
        {
            const float dx = px - (x0 + x1) * 0.5f, dy = py - (y0 + y1) * 0.5f;
            const float radius = 0.08f * h;
            const float v = dx * dx + dy * dy < radius * radius ? 64.0f : 0.0f;
            value = { v, v, v };
        }
    }
    else
    {
        const float cell = w / 6.0f;
        const float i = Min(std::floor(px / cell), 5.0f);
        x0 = i * cell;
        x1 = x0 + cell;
        y0 = row2;
        y1 = h;
        const float level = i < 2.5f ? 1.0f : 4.0f;
        const float k = i < 2.5f ? i : i - 3.0f;
        value = { k < 0.5f ? level : 0.0f, k > 0.5f && k < 1.5f ? level : 0.0f, k > 1.5f ? level : 0.0f };
    }
    if (px < x0 + gap || px >= x1 - gap || py < y0 + gap || py >= y1 - gap)
        value = { 0.0f, 0.0f, 0.0f };
    return value;
}
} // namespace ref

ref::F3 Rgb(const Pixel& p) { return { p.r, p.g, p.b }; }
Pixel WithAlpha(ref::F3 c, float a) { return { c.r, c.g, c.b, a }; }
bool Finite(const Pixel& p) { return std::isfinite(p.r) && std::isfinite(p.g) && std::isfinite(p.b); }

// ---------------------------------------------------------------------------------------------------------------
// The device, and the same root signature as dxgi.dll's (nr_shared.h): the constants, then t0-t6, u0-u2.

struct Gpu
{
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE event = nullptr;
    uint64_t fenceValue = 0;
    ID3D12RootSignature* root = nullptr;
    ID3D12PipelineState* nr = nullptr;
    ID3D12PipelineState* stats = nullptr;
    ID3D12PipelineState* fit = nullptr;
    ID3D12DescriptorHeap* heap = nullptr;
    UINT increment = 0;
    unsigned nextTable = 0;
    std::vector<ID3D12Resource*> staging; // upload and readback buffers of the list being recorded
};
Gpu g;

constexpr unsigned kTables = 64;
constexpr unsigned kSrvs = 7;           // a table: t0-t6, then u0-u2
constexpr unsigned kSlots = kSrvs + 3;

void Submit()
{
    Must(g.list->Close(), "Close");
    ID3D12CommandList* lists[] = { g.list };
    g.queue->ExecuteCommandLists(1, lists);
    Must(g.queue->Signal(g.fence, ++g.fenceValue), "Signal");
    if (g.fence->GetCompletedValue() < g.fenceValue)
    {
        Must(g.fence->SetEventOnCompletion(g.fenceValue, g.event), "SetEventOnCompletion");
        WaitForSingleObject(g.event, INFINITE);
    }
    const HRESULT removed = g.device->GetDeviceRemovedReason();
    if (FAILED(removed))
        Die("the device was removed", removed);
    Must(g.allocator->Reset(), "allocator Reset");
    Must(g.list->Reset(g.allocator, nullptr), "list Reset");
    for (ID3D12Resource* r : g.staging)
        r->Release();
    g.staging.clear();
    g.nextTable = 0;
}

void Init()
{
    Must(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.device)), "D3D12CreateDevice");
    D3D12_COMMAND_QUEUE_DESC queue = {};
    queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Must(g.device->CreateCommandQueue(&queue, IID_PPV_ARGS(&g.queue)), "CreateCommandQueue");
    Must(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.allocator)),
         "CreateCommandAllocator");
    Must(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocator, nullptr, IID_PPV_ARGS(&g.list)),
         "CreateCommandList");
    Must(g.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence)), "CreateFence");
    g.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = kSrvs;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = kSlots - kSrvs;
    ranges[1].OffsetInDescriptorsFromTableStart = kSrvs;
    D3D12_ROOT_PARAMETER parameters[2] = {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = NR_CONSTANTS_DWORDS;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 2;
    parameters[1].DescriptorTable.pDescriptorRanges = ranges;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 2;
    desc.pParameters = parameters;
    ID3DBlob* blob = nullptr;
    ID3DBlob* error = nullptr;
    Must(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error), "D3D12SerializeRootSignature");
    Must(g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g.root)),
         "CreateRootSignature");
    blob->Release();

    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline = {};
    pipeline.pRootSignature = g.root;
    pipeline.CS = { nr_cso, sizeof nr_cso };
    Must(g.device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&g.nr)), "CreateComputePipelineState(nr)");
    pipeline.CS = { nr_stats_cso, sizeof nr_stats_cso };
    Must(g.device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&g.stats)),
         "CreateComputePipelineState(nr_stats)");
    pipeline.CS = { nr_fit_cso, sizeof nr_fit_cso };
    Must(g.device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&g.fit)), "CreateComputePipelineState(nr_fit)");

    D3D12_DESCRIPTOR_HEAP_DESC heap = {};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = kTables * kSlots;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Must(g.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g.heap)), "CreateDescriptorHeap");
    g.increment = g.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
}

bool TypedLoad(DXGI_FORMAT format)
{
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {};
    support.Format = format;
    return SUCCEEDED(g.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof support)) &&
           (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0;
}

ID3D12Resource* MakeTexture(DXGI_FORMAT format, UINT width, UINT height, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    // The game's exposure texture is a plain one, as a game's would be. Every other texture here is written by a
    // shader, the scene white point too (R32_FLOAT as well, made in the UAV state).
    const bool plain = format == DXGI_FORMAT_R32_FLOAT && state != D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    desc.Flags = plain ? D3D12_RESOURCE_FLAG_NONE : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* resource = nullptr;
    Must(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource)),
         "CreateCommittedResource(texture)");
    return resource;
}

ID3D12Resource* MakeBuffer(UINT64 bytes, D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = type;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = type == D3D12_HEAP_TYPE_DEFAULT ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    const D3D12_RESOURCE_STATES state = type == D3D12_HEAP_TYPE_UPLOAD     ? D3D12_RESOURCE_STATE_GENERIC_READ
                                        : type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
                                                                           : D3D12_RESOURCE_STATE_COMMON;
    ID3D12Resource* resource = nullptr;
    Must(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource)),
         "CreateCommittedResource(buffer)");
    return resource;
}

void Barrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (before == after)
        return;
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    g.list->ResourceBarrier(1, &barrier);
}

void UavBarrier(ID3D12Resource* resource)
{
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    g.list->ResourceBarrier(1, &barrier);
}

// Writes `data` (rows packed tightly) into a texture that is in `state`, and leaves it there.
void Upload(ID3D12Resource* texture, const std::vector<uint8_t>& data, UINT bytesPerPixel, D3D12_RESOURCE_STATES state)
{
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rows = 0;
    UINT64 rowBytes = 0, total = 0;
    g.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowBytes, &total);
    ID3D12Resource* staging = MakeBuffer(total, D3D12_HEAP_TYPE_UPLOAD);
    g.staging.push_back(staging);
    uint8_t* mapped = nullptr;
    Must(staging->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "Map(upload)");
    const size_t packed = size_t(desc.Width) * bytesPerPixel;
    for (UINT y = 0; y < rows; ++y)
        std::memcpy(mapped + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch, data.data() + y * packed, packed);
    staging->Unmap(0, nullptr);
    Barrier(texture, state, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION to = {};
    to.pResource = texture;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION from = {};
    from.pResource = staging;
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint = footprint;
    g.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    Barrier(texture, D3D12_RESOURCE_STATE_COPY_DEST, state);
}

// Runs everything recorded so far plus a copy of the texture (in `state`, left there), and returns its rows packed.
std::vector<uint8_t> Read(ID3D12Resource* texture, UINT bytesPerPixel, D3D12_RESOURCE_STATES state)
{
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT rows = 0;
    UINT64 rowBytes = 0, total = 0;
    g.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowBytes, &total);
    ID3D12Resource* staging = MakeBuffer(total, D3D12_HEAP_TYPE_READBACK);
    staging->AddRef(); // kept past Submit, which releases the list's staging buffers
    g.staging.push_back(staging);
    Barrier(texture, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION to = {};
    to.pResource = staging;
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION from = {};
    from.pResource = texture;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.SubresourceIndex = 0;
    g.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    Barrier(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    Submit();
    const size_t packed = size_t(desc.Width) * bytesPerPixel;
    std::vector<uint8_t> out(packed * rows);
    uint8_t* mapped = nullptr;
    Must(staging->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "Map(readback)");
    for (UINT y = 0; y < rows; ++y)
        std::memcpy(out.data() + y * packed, mapped + footprint.Offset + size_t(y) * footprint.Footprint.RowPitch, packed);
    staging->Unmap(0, nullptr);
    staging->Release();
    return out;
}

// The same for a buffer in `state`.
std::vector<uint32_t> ReadBuffer(ID3D12Resource* buffer, UINT64 bytes, D3D12_RESOURCE_STATES state)
{
    ID3D12Resource* staging = MakeBuffer(bytes, D3D12_HEAP_TYPE_READBACK);
    staging->AddRef();
    g.staging.push_back(staging);
    Barrier(buffer, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g.list->CopyBufferRegion(staging, 0, buffer, 0, bytes);
    Barrier(buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    Submit();
    std::vector<uint32_t> out(size_t(bytes / 4));
    void* mapped = nullptr;
    Must(staging->Map(0, nullptr, &mapped), "Map(readback buffer)");
    std::memcpy(out.data(), mapped, out.size() * 4);
    staging->Unmap(0, nullptr);
    staging->Release();
    return out;
}

struct View
{
    ID3D12Resource* resource = nullptr;
    DXGI_FORMAT format = DXGI_FORMAT_R16G16B16A16_FLOAT;
};

D3D12_CPU_DESCRIPTOR_HANDLE Cpu(unsigned slot)
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = g.heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += SIZE_T(slot) * g.increment;
    return handle;
}

unsigned NextTable()
{
    if (g.nextTable == kTables)
        Die("out of descriptor tables", E_FAIL);
    return g.nextTable++ * kSlots;
}

void WriteSrvs(unsigned table, const View (&srvs)[kSrvs])
{
    for (unsigned i = 0; i < kSrvs; ++i)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = srvs[i].format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        g.device->CreateShaderResourceView(srvs[i].resource, &srv, Cpu(table + i));
    }
}

void WriteTextureUav(unsigned slot, View view)
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = view.format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g.device->CreateUnorderedAccessView(view.resource, nullptr, &uav, Cpu(slot));
}

// A descriptor table of nr.hlsl or nr_stats.hlsl as dxgi.dll lays it out; slots left empty hold null descriptors.
// t4-t6 are the fit's textures, which only the composite with ModelScale reads.
unsigned Table(View t0, View t1, View t2, View t3, View u0, ID3D12Resource* u1 = nullptr, UINT64 u1Bytes = 0,
               View t4 = {}, View t5 = {}, View t6 = {})
{
    const unsigned table = NextTable();
    WriteSrvs(table, { t0, t1, t2, t3, t4, t5, t6 });
    WriteTextureUav(table + kSrvs, u0);
    D3D12_UNORDERED_ACCESS_VIEW_DESC raw = {};
    raw.Format = DXGI_FORMAT_R32_TYPELESS;
    raw.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    raw.Buffer.NumElements = u1 != nullptr ? UINT(u1Bytes / 4) : 1;
    raw.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    g.device->CreateUnorderedAccessView(u1, nullptr, &raw, Cpu(table + kSrvs + 1));
    WriteTextureUav(table + kSrvs + 2, {});
    return table;
}

// The fit's table (nr_fit.hlsl): the model's output and its input, then its three textures through u0-u2.
unsigned FitTable(View model, View proxy, View slope, View value, View raw)
{
    const unsigned table = NextTable();
    WriteSrvs(table, { {}, model, proxy, {}, {}, {}, {} });
    WriteTextureUav(table + kSrvs, slope);
    WriteTextureUav(table + kSrvs + 1, value);
    WriteTextureUav(table + kSrvs + 2, raw);
    return table;
}

void Dispatch(ID3D12PipelineState* pipeline, unsigned table, const NrConstants& c, UINT groupsX, UINT groupsY)
{
    ID3D12DescriptorHeap* heaps[] = { g.heap };
    g.list->SetDescriptorHeaps(1, heaps);
    g.list->SetComputeRootSignature(g.root);
    g.list->SetPipelineState(pipeline);
    g.list->SetComputeRoot32BitConstants(0, NR_CONSTANTS_DWORDS, &c, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE handle = g.heap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += UINT64(table) * g.increment;
    g.list->SetComputeRootDescriptorTable(1, handle);
    g.list->Dispatch(groupsX, groupsY, 1);
}

UINT Groups(UINT size, UINT per) { return (size + per - 1) / per; }

// ---------------------------------------------------------------------------------------------------------------
// A synthetic frame: a ramp from 12 EV below the white point to 6 above, in six colours, with a NaN, an infinity,
// negative, zero and tiny values in its first row, inside a larger texture whose border must never change. A
// display-encoded frame is a plain 0..1 ramp in the same colours.

constexpr float kWhite = 2.5f;

struct Scene
{
    const Format* format = nullptr;
    UINT width = 0, height = 0;  // the frame
    UINT baseX = 0, baseY = 0;   // where it lies in the texture
    UINT textureWidth = 0, textureHeight = 0;
    std::vector<uint8_t> bytes;  // the texture as uploaded
    std::vector<Pixel> texture;  // the same, decoded: what the GPU reads
    ID3D12Resource* output = nullptr;   // the game's Output, in the UAV state between steps
    ID3D12Resource* proxy = nullptr;    // RGBA16F, UAV between steps
    ID3D12Resource* model = nullptr;    // RGBA16F, NPSR between steps
    ID3D12Resource* exposure = nullptr; // R32_FLOAT 1x1, NPSR

    const Pixel& Frame(UINT x, UINT y) const { return texture[size_t(y + baseY) * textureWidth + x + baseX]; }
};

Pixel SceneValue(const Format& f, UINT x, UINT y, UINT width)
{
    static const float kHues[6][3] = { { 1.0f, 1.0f, 1.0f },   { 1.0f, 0.62f, 0.31f }, { 0.33f, 1.0f, 0.41f },
                                       { 0.27f, 0.36f, 1.0f }, { 1.0f, 0.06f, 0.03f }, { 0.03f, 0.91f, 1.0f } };
    const float* hue = kHues[(y / 3) % 6];
    if (!f.linear)
    {
        const float v = (float(x) + 0.5f) / float(width);
        return { v * hue[0], v * hue[1], v * hue[2], 0.75f };
    }
    if (y == 0 && x < 6)
    {
        switch (x)
        {
        case 0: return { NAN, 0.5f, 0.5f, 0.75f };
        case 1: return { 0.5f, INFINITY, 0.5f, 0.75f };
        case 2: return { -0.5f, 0.2f, 0.2f, 0.75f };
        case 3: return { 0.0f, 0.0f, 0.0f, 0.75f };
        case 4: return { 1.0e-9f, 0.0f, 0.0f, 0.75f };
        default: return { -0.0f, -0.0f, -0.0f, 0.75f };
        }
    }
    const float ev = -12.0f + 18.0f * (float(x) + 0.37f) / float(width);
    const float v = kWhite * std::exp2(ev);
    return { v * hue[0], v * hue[1], v * hue[2], 0.75f };
}

Scene MakeScene(const Format& f, UINT width, UINT height, UINT border)
{
    Scene s;
    s.format = &f;
    s.width = width;
    s.height = height;
    s.baseX = border;
    s.baseY = border / 2;
    s.textureWidth = width + 2 * border;
    s.textureHeight = height + border;
    s.bytes.resize(size_t(s.textureWidth) * s.textureHeight * f.bytes);
    s.texture.resize(size_t(s.textureWidth) * s.textureHeight);
    for (UINT y = 0; y < s.textureHeight; ++y)
    {
        for (UINT x = 0; x < s.textureWidth; ++x)
        {
            const bool inside = x >= s.baseX && x < s.baseX + width && y >= s.baseY && y < s.baseY + height;
            const Pixel p = inside ? SceneValue(f, x - s.baseX, y - s.baseY, width) : Pixel { 0.5f, 0.25f, 0.125f, 0.5f };
            const size_t i = size_t(y) * s.textureWidth + x;
            Encode(f, p, s.bytes.data() + i * f.bytes);
            s.texture[i] = Decode(f, s.bytes.data() + i * f.bytes);
        }
    }
    s.output = MakeTexture(f.dxgi, s.textureWidth, s.textureHeight, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    s.proxy = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    s.model = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    s.exposure = MakeTexture(DXGI_FORMAT_R32_FLOAT, 1, 1, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return s;
}

void Drop(Scene& s)
{
    ID3D12Resource* const resources[] = { s.output, s.proxy, s.model, s.exposure };
    for (ID3D12Resource* r : resources)
    {
        if (r != nullptr)
            r->Release();
    }
    s = {};
}

// The settings of one run of the passes, as nr_dx12.cpp turns the ini keys into constants.
struct Setup
{
    bool linear = true;
    float whiteScale = kWhite;
    bool useExposure = false;
    float exposure = 1.0f;
    ref::Knobs knobs;
    bool card = false;
    ref::Rect cardRect = { 6, 5, 96, 40 };
    uint32_t frame = 12345;
    UINT modelWidth = 0, modelHeight = 0; // ModelScale: the model's picture; 0 = the frame's size
};

UINT ModelW(const Scene& s, const Setup& u) { return u.modelWidth != 0 ? u.modelWidth : s.width; }
UINT ModelH(const Scene& s, const Setup& u) { return u.modelHeight != 0 ? u.modelHeight : s.height; }

NrConstants Constants(const Scene& s, const Setup& u, uint32_t mode)
{
    NrConstants c = {};
    c.mode = mode;
    c.width = s.width;
    c.height = s.height;
    c.baseX = s.baseX;
    c.baseY = s.baseY;
    c.frame = u.frame;
    c.whiteScale = u.whiteScale;
    c.shoulder = u.knobs.shoulder;
    c.detail = u.knobs.detail;
    c.colour = u.knobs.colour;
    c.maxGain = u.knobs.maxGain;
    c.highlight = u.knobs.highlight;
    if (u.linear)
        c.flags |= NR_FLAG_LINEAR_HDR;
    if (u.useExposure)
        c.flags |= NR_FLAG_EXPOSURE;
    if (u.card)
    {
        c.flags |= NR_FLAG_CARD;
        c.cardX = u.cardRect.x;
        c.cardY = u.cardRect.y;
        c.cardW = u.cardRect.w;
        c.cardH = u.cardRect.h;
    }
    c.modelWidth = ModelW(s, u);
    c.modelHeight = ModelH(s, u);
    c.stepX = float(s.width) / float(c.modelWidth);
    c.stepY = float(s.height) / float(c.modelHeight);
    if (c.modelWidth != s.width || c.modelHeight != s.height)
        c.flags |= NR_FLAG_SCALED;
    return c;
}

float WhiteOf(const Setup& u) { return u.linear ? ref::WhitePoint(u.whiteScale, u.useExposure, u.exposure) : 1.0f; }

View OutputView(const Scene& s) { return { s.output, s.format->dxgi }; }
View ExposureView(const Scene& s, const Setup& u) { return u.useExposure ? View { s.exposure, DXGI_FORMAT_R32_FLOAT } : View {}; }

// The game's frame back into the Output, the exposure value into its texture.
void Reset(Scene& s, const Setup& u)
{
    Upload(s.output, s.bytes, s.format->bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    std::vector<uint8_t> exposure(4);
    std::memcpy(exposure.data(), &u.exposure, 4);
    Upload(s.exposure, exposure, 4, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

// Records the encode: Output -> proxy, over the model's picture. The Output is back in UAV and the proxy still in UAV
// afterwards.
void RecordEncode(Scene& s, const Setup& u)
{
    Barrier(s.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const unsigned table = Table(OutputView(s), {}, {}, ExposureView(s, u), { s.proxy, DXGI_FORMAT_R16G16B16A16_FLOAT });
    Dispatch(g.nr, table, Constants(s, u, NR_MODE_ENCODE), Groups(ModelW(s, u), 8), Groups(ModelH(s, u), 8));
    Barrier(s.output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    UavBarrier(s.proxy);
}

std::vector<Pixel> DecodeAll(const Format& f, const std::vector<uint8_t>& bytes)
{
    std::vector<Pixel> out(bytes.size() / f.bytes);
    for (size_t i = 0; i < out.size(); ++i)
        out[i] = Decode(f, bytes.data() + i * f.bytes);
    return out;
}

std::vector<uint8_t> Encode(const std::vector<Pixel>& pixels)
{
    std::vector<uint8_t> out(pixels.size() * 8);
    for (size_t i = 0; i < pixels.size(); ++i)
        Encode(kHalf, pixels[i], out.data() + i * 8);
    return out;
}

// Encodes and reads the proxy back (as half floats) and decoded.
std::vector<uint8_t> EncodeAndReadProxy(Scene& s, const Setup& u)
{
    Reset(s, u);
    RecordEncode(s, u);
    return Read(s.proxy, 8, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

// The CPU encode of frame pixel (x, y).
Pixel ExpectedProxy(const Scene& s, const Setup& u, UINT x, UINT y)
{
    const Pixel& f = s.Frame(x, y);
    if (!u.linear)
        return WithAlpha(ref::Saturate(ref::Clean(Rgb(f))), 1.0f);
    const float white = WhiteOf(u);
    const ref::F3 c = u.card && ref::InCard(x, y, u.cardRect) ? ref::Card(x, y, u.cardRect) * white : Rgb(f);
    ref::F3 e = ref::EncodeLinear(c, white, u.knobs.shoulder);
    e = { ref::Min(e.r, 1.0f), ref::Min(e.g, 1.0f), ref::Min(e.b, 1.0f) };
    return WithAlpha(ref::SrgbEncode(e), 1.0f);
}

void CheckEncode(Scene& s, const Setup& u, const char* what)
{
    const std::vector<Pixel> proxy = DecodeAll(kHalf, EncodeAndReadProxy(s, u));
    size_t bad = 0;
    UINT firstX = 0, firstY = 0;
    Pixel got, want;
    for (UINT y = 0; y < s.height; ++y)
    {
        for (UINT x = 0; x < s.width; ++x)
        {
            const Pixel p = proxy[size_t(y) * s.width + x];
            const Pixel e = ExpectedProxy(s, u, x, y);
            if (!ClosePixel(kHalf, p, e) || p.a != 1.0f)
            {
                if (bad++ == 0)
                    firstX = x, firstY = y, got = p, want = e;
            }
        }
    }
    if (bad == 0)
        Check(true, "encode, %s, %s: %ux%u pixels as the CPU formula", s.format->name, what, s.width, s.height);
    else
        Check(false, "encode, %s, %s: %zu pixels differ; first (%u,%u) got %g %g %g %g, want %g %g %g", s.format->name,
              what, bad, firstX, firstY, got.r, got.g, got.b, got.a, want.r, want.g, want.b);
}

// A model: the proxy's linear light changed by `change`, clipped to 0..1 and sRGB-encoded again as half floats.
std::vector<Pixel> MakeModel(const std::vector<Pixel>& proxy, const std::function<ref::F3(ref::F3, size_t)>& change)
{
    std::vector<Pixel> model(proxy.size());
    for (size_t i = 0; i < proxy.size(); ++i)
    {
        ref::F3 o = change(ref::ProxyLinear(Rgb(proxy[i])), i);
        o = { std::clamp(o.r, 0.0f, 1.0f), std::clamp(o.g, 0.0f, 1.0f), std::clamp(o.b, 0.0f, 1.0f) };
        model[i] = Quantise(kHalf, WithAlpha(ref::SrgbEncode(o), 1.0f));
    }
    return model;
}

// Runs encode, the given model output, and the composite; returns the Output texture's bytes.
std::vector<uint8_t> RunComposite(Scene& s, const Setup& u, const std::vector<uint8_t>& modelBytes)
{
    Reset(s, u);
    RecordEncode(s, u);
    Upload(s.model, modelBytes, 8, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(s.proxy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const unsigned table = Table({}, { s.model, DXGI_FORMAT_R16G16B16A16_FLOAT }, { s.proxy, DXGI_FORMAT_R16G16B16A16_FLOAT },
                                 ExposureView(s, u), OutputView(s));
    Dispatch(g.nr, table, Constants(s, u, NR_MODE_COMPOSITE), Groups(s.width, 8), Groups(s.height, 8));
    Barrier(s.proxy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    return Read(s.output, s.format->bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

// What the composite must leave in texture pixel i (x, y in the texture), given the proxy and model the GPU used.
// `exact` is set when the pixel must be the game's own bits.
Pixel ExpectedOutput(const Scene& s, const Setup& u, const std::vector<Pixel>& proxy, const std::vector<Pixel>& model,
                     UINT tx, UINT ty, bool* exact)
{
    const size_t i = size_t(ty) * s.textureWidth + tx;
    *exact = true;
    if (tx < s.baseX || tx >= s.baseX + s.width || ty < s.baseY || ty >= s.baseY + s.height)
        return s.texture[i];
    const UINT x = tx - s.baseX, y = ty - s.baseY;
    Pixel frame = s.texture[i];
    const float white = WhiteOf(u);
    bool written = false;
    if (u.card && ref::InCard(x, y, u.cardRect))
    {
        const ref::F3 c = ref::Card(x, y, u.cardRect) * white;
        frame = { c.r, c.g, c.b, frame.a };
        written = true;
    }
    Pixel result = frame;
    const Pixel& pe = proxy[size_t(y) * s.width + x];
    const Pixel& me = model[size_t(y) * s.width + x];
    if (u.knobs.detail != 0.0f && Finite(frame) && (me.r != pe.r || me.g != pe.g || me.b != pe.b))
    {
        const ref::F3 r = ref::Composite(Rgb(frame), Rgb(pe), Rgb(me), u.linear, u.knobs);
        result = { r.r, r.g, r.b, frame.a };
        written = true;
    }
    *exact = !written;
    return written ? Quantise(*s.format, result) : result;
}

// What the composite must leave in a texture pixel, and whether that is the game's own bits.
using Expected = std::function<Pixel(UINT tx, UINT ty, bool* exact)>;

// Compares a composite's Output with the CPU: untouched pixels bit for bit, the rest within two steps of the format,
// or within `spread` times the pixel's largest channel. ModelScale needs that: there a channel is the sum of terms as
// large as the pixel's largest, which the GPU's float maths and the CPU's can part on by a few millionths of that, and
// where they nearly cancel (a colour change taking a small channel to almost nothing) two steps of what is left are
// less than that.
void CheckOutput(const Scene& s, const std::vector<uint8_t>& output, const Expected& expected, const char* what,
                 float spread = 0.0f)
{
    size_t bad = 0, changed = 0;
    UINT firstX = 0, firstY = 0, worstX = 0, worstY = 0;
    Pixel got, want, worstGot, worstWant;
    float worst = 0.0f; // the largest miss in a channel, over the pixel's largest channel
    for (UINT ty = 0; ty < s.textureHeight; ++ty)
    {
        for (UINT tx = 0; tx < s.textureWidth; ++tx)
        {
            bool exact = false;
            const Pixel e = expected(tx, ty, &exact);
            const size_t i = size_t(ty) * s.textureWidth + tx;
            const Pixel o = Decode(*s.format, output.data() + i * s.format->bytes);
            bool ok;
            if (exact)
                ok = std::memcmp(output.data() + i * s.format->bytes, s.bytes.data() + i * s.format->bytes,
                                 s.format->bytes) == 0;
            else
            {
                ++changed;
                const float slack = spread * std::max({ std::fabs(e.r), std::fabs(e.g), std::fabs(e.b) });
                auto within = [&](int c, float value, float wanted)
                { return Close(*s.format, c, value, wanted) || std::fabs(value - wanted) <= slack; };
                ok = within(0, o.r, e.r) && within(1, o.g, e.g) && within(2, o.b, e.b) &&
                     (s.format->bytes == 4 && s.format->linear ? true : o.a == e.a);
            }
            if (!ok)
            {
                if (bad++ == 0)
                    firstX = tx, firstY = ty, got = o, want = e;
                const float scale = std::max({ std::fabs(e.r), std::fabs(e.g), std::fabs(e.b), 1.0e-30f });
                const float miss =
                    std::max({ std::fabs(o.r - e.r), std::fabs(o.g - e.g), std::fabs(o.b - e.b) }) / scale;
                if (!(miss <= worst)) // NaN counts as the worst
                    worst = miss, worstX = tx, worstY = ty, worstGot = o, worstWant = e;
            }
        }
    }
    if (bad == 0)
        Check(true, "composite, %s, %s: %zu pixels as the CPU formula, the other %zu bit for bit as they were",
              s.format->name, what, changed, size_t(s.textureWidth) * s.textureHeight - changed);
    else
        Check(false,
              "composite, %s, %s: %zu pixels wrong; first (%u,%u) got %g %g %g %g, want %g %g %g %g; worst "
              "(%u,%u), off by %g of its largest channel: got %g %g %g, want %g %g %g",
              s.format->name, what, bad, firstX, firstY, got.r, got.g, got.b, got.a, want.r, want.g, want.b, want.a,
              worstX, worstY, worst, worstGot.r, worstGot.g, worstGot.b, worstWant.r, worstWant.g, worstWant.b);
}

void CheckComposite(const Scene& s, const Setup& u, const std::vector<Pixel>& proxy, const std::vector<Pixel>& model,
                    const std::vector<uint8_t>& output, const char* what)
{
    CheckOutput(
        s, output,
        [&](UINT tx, UINT ty, bool* exact) { return ExpectedOutput(s, u, proxy, model, tx, ty, exact); }, what);
}

// The whole Output bit for bit as the game made it.
void CheckUntouched(const Scene& s, const std::vector<uint8_t>& output, const char* what)
{
    size_t bad = 0, first = 0;
    for (size_t i = 0; i < s.texture.size(); ++i)
    {
        const size_t at = i * s.format->bytes;
        if (std::memcmp(output.data() + at, s.bytes.data() + at, s.format->bytes) != 0 && bad++ == 0)
            first = i;
    }
    if (bad == 0)
        Check(true, "composite, %s, %s: the Output is bit for bit the game's (0 pixels differ)", s.format->name, what);
    else
    {
        const Pixel o = Decode(*s.format, output.data() + first * s.format->bytes);
        const Pixel& e = s.texture[first];
        Check(false,
              "composite, %s, %s: the Output is bit for bit the game's (%zu pixels differ; first (%zu,%zu) got "
              "%g %g %g %g, was %g %g %g %g)",
              s.format->name, what, bad, first % s.textureWidth, first / s.textureWidth, o.r, o.g, o.b, o.a, e.r, e.g,
              e.b, e.a);
    }
}

// The Output over the frame, in the game's units, for brightness ratios.
float Luma(const Pixel& p) { return 0.2126f * p.r + 0.7152f * p.g + 0.0722f * p.b; }

// Over the pixels where the model's change was not clipped and far above the gain floor, Output / frame luminance
// must be 2^ev: a check of the composite that does not lean on the CPU copy of its formula.
void CheckRatio(const Scene& s, const std::vector<Pixel>& proxy, const std::vector<uint8_t>& output, float ev,
                const char* what)
{
    size_t counted = 0, bad = 0;
    double worst = 0.0;
    for (UINT y = 1; y < s.height; ++y)
    {
        for (UINT x = 0; x < s.width; ++x)
        {
            const ref::F3 p = ref::ProxyLinear(Rgb(proxy[size_t(y) * s.width + x]));
            // Both the proxy and the model below the shoulder and well above the floor.
            if (ref::Max3(p) > 0.08f || ref::Dot(p, ref::kLuma) < 0.02f)
                continue;
            const Pixel& f = s.Frame(x, y);
            const size_t i = size_t(y + s.baseY) * s.textureWidth + x + s.baseX;
            const Pixel o = Decode(*s.format, output.data() + i * s.format->bytes);
            const double ratio = std::log2(double(Luma(o)) / double(Luma(f)));
            worst = std::max(worst, std::fabs(ratio - ev));
            ++counted;
            bad += std::fabs(ratio - ev) > (s.format->dxgi == DXGI_FORMAT_R11G11B10_FLOAT ? 0.06 : 0.02);
        }
    }
    Check(counted > 100 && bad == 0, "composite, %s, %s: Output / frame is %+.2f EV over %zu mid-tone pixels (worst "
                                     "off by %.3f EV)",
          s.format->name, what, double(ev), counted, worst);
}

// A model that pushes the brightest pixels to the top of its range (largest channel 1, hue kept), as one trained on
// 8-bit pictures does with a bright sky. With HighlightRestore 1 they come back brighter by the plain ratio (under
// 0.2 EV here) plus at most the restore up to 3 EV of compression (under 0.5 EV at Shoulder 0.70), never by the
// MaxGainEV an inverse taken all the way to 1 gave them.
void CheckClippedWhite(Scene& s, const std::vector<Pixel>& proxy)
{
    Setup u;
    u.knobs.highlight = 1.0f;
    const float heavy = u.knobs.shoulder + ref::kHeavyU * (1.0f - u.knobs.shoulder);
    const std::vector<Pixel> clipped = MakeModel(proxy, [heavy](ref::F3 p, size_t) {
        return ref::Max3(p) > heavy ? p / ref::Max3(p) : p;
    });
    const std::vector<uint8_t> output = RunComposite(s, u, Encode(clipped));
    CheckComposite(s, u, proxy, clipped, output, "model pushes the heavy highlights to 1, HighlightRestore 1");
    size_t counted = 0;
    double worst = -100.0;
    for (UINT y = 0; y < s.height; ++y)
    {
        for (UINT x = 0; x < s.width; ++x)
        {
            const ref::F3 p = ref::ProxyLinear(Rgb(proxy[size_t(y) * s.width + x]));
            const Pixel& f = s.Frame(x, y);
            if (ref::Max3(p) <= heavy || !std::isfinite(Luma(f)) || Luma(f) <= 0.0f)
                continue;
            const size_t i = size_t(y + s.baseY) * s.textureWidth + x + s.baseX;
            const Pixel o = Decode(*s.format, output.data() + i * s.format->bytes);
            worst = std::max(worst, std::log2(double(Luma(o)) / double(Luma(f))));
            ++counted;
        }
    }
    Check(counted > 100 && worst < 0.9,
          "composite, %s: highlights the model pushed to 1 come back at most %+.2f EV brighter over %zu pixels "
          "(MaxGainEV 1)",
          s.format->name, worst, counted);
}

// The card in the model's input does not depend on the white point; in the Output it is the white point times its
// value.
void CheckCard(Scene& s)
{
    Setup low, high;
    low.card = high.card = true;
    high.whiteScale = low.whiteScale * 8.0f;
    const std::vector<Pixel> a = DecodeAll(kHalf, EncodeAndReadProxy(s, low));
    const std::vector<Pixel> b = DecodeAll(kHalf, EncodeAndReadProxy(s, high));
    size_t inside = 0, bad = 0, outsideSame = 0;
    for (UINT y = 0; y < s.height; ++y)
    {
        for (UINT x = 0; x < s.width; ++x)
        {
            const size_t i = size_t(y) * s.width + x;
            if (ref::InCard(x, y, low.cardRect))
            {
                ++inside;
                bad += !ClosePixel(kHalf, a[i], b[i]);
            }
            else
                outsideSame += std::memcmp(&a[i], &b[i], sizeof(Pixel)) == 0 && ref::Max3(Rgb(a[i])) > 0.01f;
        }
    }
    Check(inside == size_t(low.cardRect.w) * low.cardRect.h && bad == 0 && outsideSame < 50,
          "card, %s: the model's input inside the card is the same at two white points 3 EV apart (%zu pixels, %zu "
          "differ), the scene around it is not",
          s.format->name, inside, bad);
    CheckEncode(s, high, "card, white point x8");

    // With a model that returns its input, the Output inside the card is exactly the card at the white point.
    const std::vector<uint8_t> proxyBytes = EncodeAndReadProxy(s, low);
    const std::vector<Pixel> proxy = DecodeAll(kHalf, proxyBytes);
    const std::vector<uint8_t> output = RunComposite(s, low, proxyBytes);
    CheckComposite(s, low, proxy, proxy, output, "card, model returns its input");
}

// ---------------------------------------------------------------------------------------------------------------
// The statistics (nr_stats.hlsl) against the CPU.

struct CpuStats
{
    uint32_t counts[NR_COUNTS] = {};
    uint32_t maxHist[NR_HIST_BINS] = {};
    uint32_t lumHist[NR_HIST_BINS] = {};
    uint32_t gainHist[NR_GAIN_BINS] = {};
    float largest = 0.0f;
};

int Bin(float value, float lowEv, float perEv, int bins)
{
    const float at = std::floor((std::log2(value) - lowEv) * perEv);
    return at < 0.0f ? -1 : at >= float(bins) ? bins : int(at);
}

CpuStats ExpectedStats(const Scene& s, const Setup& u, const std::vector<Pixel>& proxy, const std::vector<Pixel>& model)
{
    CpuStats c;
    const float white = WhiteOf(u);
    const float sh = u.knobs.shoulder;
    for (UINT y = 0; y < s.height; ++y)
    {
        for (UINT x = 0; x < s.width; ++x)
        {
            ++c.counts[NR_C_PIXELS];
            const Pixel& f = s.Frame(x, y);
            if (!Finite(f))
            {
                ++c.counts[NR_C_NONFINITE];
                continue;
            }
            if (f.r < 0.0f || f.g < 0.0f || f.b < 0.0f)
                ++c.counts[NR_C_NEGATIVE];
            const ref::F3 x3 = ref::Clean(Rgb(f)) / white;
            c.largest = std::max(c.largest, ref::Max3(ref::Clean(Rgb(f))));
            const float m = ref::Max3(x3);
            if (m <= 0.0f)
            {
                ++c.counts[NR_C_BLACK];
                ++c.counts[NR_C_DARK];
                continue;
            }
            const int bin = Bin(m, float(NR_HIST_EV_MIN), float(NR_HIST_PER_EV), NR_HIST_BINS);
            if (bin < 0)
                ++c.counts[NR_C_BELOW];
            else if (bin >= NR_HIST_BINS)
                ++c.counts[NR_C_ABOVE];
            else
                ++c.maxHist[bin];
            const float l = ref::Dot(x3, ref::kLuma);
            if (l > 0.0f)
            {
                const int lumBin = Bin(l, float(NR_HIST_EV_MIN), float(NR_HIST_PER_EV), NR_HIST_BINS);
                if (lumBin >= 0 && lumBin < NR_HIST_BINS)
                    ++c.lumHist[lumBin];
            }
            if (u.linear && m > sh)
                ++c.counts[NR_C_SHOULDER];
            if (u.linear && m > sh + ref::kHeavyT * (1.0f - sh))
                ++c.counts[NR_C_HEAVY];
            if (m < 0.5f / 255.0f / 12.92f)
                ++c.counts[NR_C_DARK];
        }
    }
    for (size_t i = 0; i < proxy.size(); ++i)
    {
        ++c.counts[NR_C_GAIN_PIXELS];
        const ref::F3 p = ref::ProxyLinear(Rgb(proxy[i]));
        const ref::F3 o = ref::ModelLinear(Rgb(model[i]), Rgb(proxy[i]));
        const float gain = ref::ModelGain(p, o, u.linear, u.knobs) * u.knobs.detail;
        if (gain < -u.knobs.maxGain)
            ++c.counts[NR_C_GAIN_LOW];
        else if (gain > u.knobs.maxGain)
            ++c.counts[NR_C_GAIN_HIGH];
        const float at = std::clamp(std::floor((gain - float(NR_GAIN_EV_MIN)) * float(NR_GAIN_PER_EV)), 0.0f,
                                    float(NR_GAIN_BINS - 1));
        ++c.gainHist[int(at)];
        const ref::F3 chroma = ref::ModelChroma(p, o);
        if (std::fabs(chroma.r) + std::fabs(chroma.g) + std::fabs(chroma.b) > 0.05f)
            ++c.counts[NR_C_COLOUR];
    }
    return c;
}

// Histograms as running totals, allowing `slack` pixels to sit in a neighbouring bin (log2 at a bin's edge).
bool SameHistogram(const uint32_t* got, const uint32_t* want, int bins, uint32_t slack)
{
    int64_t a = 0, b = 0;
    for (int i = 0; i < bins; ++i)
    {
        a += got[i];
        b += want[i];
        if (std::llabs(a - b) > int64_t(slack))
            return false;
    }
    return a == b;
}

bool Near(uint32_t got, uint32_t want, uint32_t slack) { return (got > want ? got - want : want - got) <= slack; }

float Bits(uint32_t bits)
{
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

void CheckStats(Scene& s, const Setup& u, const std::vector<uint8_t>& modelBytes)
{
    const UINT64 bytes = NR_S_WORDS * 4;
    ID3D12Resource* stats = MakeBuffer(bytes, D3D12_HEAP_TYPE_DEFAULT);
    Reset(s, u);
    RecordEncode(s, u);
    Upload(s.model, modelBytes, 8, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(stats, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(s.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(s.proxy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const unsigned input = Table(OutputView(s), {}, {}, ExposureView(s, u), {}, stats, bytes);
    const unsigned gain = Table({}, { s.model, DXGI_FORMAT_R16G16B16A16_FLOAT }, { s.proxy, DXGI_FORMAT_R16G16B16A16_FLOAT },
                                ExposureView(s, u), {}, stats, bytes);
    Dispatch(g.stats, input, Constants(s, u, NR_STATS_CLEAR), 1, 1);
    UavBarrier(stats);
    Dispatch(g.stats, input, Constants(s, u, NR_STATS_INPUT), Groups(s.width, 32), Groups(s.height, 32));
    UavBarrier(stats);
    Dispatch(g.stats, gain, Constants(s, u, NR_STATS_GAIN), Groups(s.width, 32), Groups(s.height, 32));
    Barrier(s.output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(s.proxy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const std::vector<uint32_t> w = ReadBuffer(stats, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    stats->Release();

    const std::vector<Pixel> proxy = DecodeAll(kHalf, EncodeAndReadProxy(s, u));
    const std::vector<Pixel> model = DecodeAll(kHalf, modelBytes);
    const CpuStats e = ExpectedStats(s, u, proxy, model);
    const uint32_t* c = w.data() + NR_S_COUNTS;
    Check(w[NR_S_TAG_FIRST] == u.frame && w[NR_S_TAG_LAST] == u.frame,
          "statistics, %s: stamped %u and %u at the two ends (frame %u)", s.format->name, w[NR_S_TAG_FIRST],
          w[NR_S_TAG_LAST], u.frame);
    const bool exactCounts = c[NR_C_PIXELS] == e.counts[NR_C_PIXELS] && c[NR_C_NONFINITE] == e.counts[NR_C_NONFINITE] &&
                             c[NR_C_NEGATIVE] == e.counts[NR_C_NEGATIVE] && c[NR_C_BLACK] == e.counts[NR_C_BLACK] &&
                             c[NR_C_GAIN_PIXELS] == e.counts[NR_C_GAIN_PIXELS];
    Check(exactCounts, "statistics, %s: pixels %u/%u, not finite %u/%u, negative %u/%u, black %u/%u, gain pixels %u/%u",
          s.format->name, c[NR_C_PIXELS], e.counts[NR_C_PIXELS], c[NR_C_NONFINITE], e.counts[NR_C_NONFINITE],
          c[NR_C_NEGATIVE], e.counts[NR_C_NEGATIVE], c[NR_C_BLACK], e.counts[NR_C_BLACK], c[NR_C_GAIN_PIXELS],
          e.counts[NR_C_GAIN_PIXELS]);
    const uint32_t slack = 3;
    const bool nearCounts = Near(c[NR_C_BELOW], e.counts[NR_C_BELOW], slack) &&
                            Near(c[NR_C_ABOVE], e.counts[NR_C_ABOVE], slack) &&
                            Near(c[NR_C_SHOULDER], e.counts[NR_C_SHOULDER], slack) &&
                            Near(c[NR_C_HEAVY], e.counts[NR_C_HEAVY], slack) &&
                            Near(c[NR_C_DARK], e.counts[NR_C_DARK], slack) &&
                            Near(c[NR_C_GAIN_LOW], e.counts[NR_C_GAIN_LOW], slack) &&
                            Near(c[NR_C_GAIN_HIGH], e.counts[NR_C_GAIN_HIGH], slack) &&
                            Near(c[NR_C_COLOUR], e.counts[NR_C_COLOUR], slack);
    Check(nearCounts,
          "statistics, %s: below %u/%u, above %u/%u, shoulder %u/%u, heavy %u/%u, dark %u/%u, gain low %u/%u, high "
          "%u/%u, colour %u/%u",
          s.format->name, c[NR_C_BELOW], e.counts[NR_C_BELOW], c[NR_C_ABOVE], e.counts[NR_C_ABOVE], c[NR_C_SHOULDER],
          e.counts[NR_C_SHOULDER], c[NR_C_HEAVY], e.counts[NR_C_HEAVY], c[NR_C_DARK], e.counts[NR_C_DARK],
          c[NR_C_GAIN_LOW], e.counts[NR_C_GAIN_LOW], c[NR_C_GAIN_HIGH], e.counts[NR_C_GAIN_HIGH], c[NR_C_COLOUR],
          e.counts[NR_C_COLOUR]);
    Check(SameHistogram(w.data() + NR_S_MAX_HIST, e.maxHist, NR_HIST_BINS, slack) &&
              SameHistogram(w.data() + NR_S_LUM_HIST, e.lumHist, NR_HIST_BINS, slack) &&
              SameHistogram(w.data() + NR_S_GAIN_HIST, e.gainHist, NR_GAIN_BINS, slack),
          "statistics, %s: the three histograms", s.format->name);
    Check(Bits(c[NR_C_WHITE]) == WhiteOf(u) && Bits(c[NR_C_EXPOSURE]) == (u.useExposure ? u.exposure : 0.0f) &&
              Bits(c[NR_C_MAX_BITS]) == e.largest,
          "statistics, %s: white point %g (want %g), exposure %g, largest value %g (want %g)", s.format->name,
          double(Bits(c[NR_C_WHITE])), double(WhiteOf(u)), double(Bits(c[NR_C_EXPOSURE])),
          double(Bits(c[NR_C_MAX_BITS])), double(e.largest));
}

// ---------------------------------------------------------------------------------------------------------------
// The scene meter (nr_stats.hlsl's METER and RESOLVE passes).

const float kSceneKey = 0.2f;
const float kSceneLow = 0.25f;
const float kSceneHigh = 0.75f;
const float kSceneSeconds = 0.5f;

// Whether the meter counts a pixel, and the log2 of its luminance if so.
bool Metered(const Pixel& p, float* ev)
{
    const float l = ref::Dot(ref::Clean(Rgb(p)), ref::kLuma);
    if (!Finite(p) || !(l > 0.0f))
        return false;
    *ev = std::log2(l);
    return true;
}

// The histogram the METER pass makes of the frame.
std::vector<uint32_t> MeterBins(const Scene& s)
{
    std::vector<uint32_t> bins(NR_METER_BINS);
    for (UINT y = 0; y < s.height; ++y)
    {
        for (UINT x = 0; x < s.width; ++x)
        {
            float ev = 0.0f;
            if (!Metered(s.Frame(x, y), &ev))
                continue;
            const float at = std::floor((ev - float(NR_METER_EV_MIN)) * float(NR_METER_PER_EV));
            ++bins[size_t(std::clamp(at, 0.0f, float(NR_METER_BINS - 1)))];
        }
    }
    return bins;
}

// What the RESOLVE pass aims for, from that histogram: the log2 of the exposure that puts the mean log2 luminance of
// the middle half of the pixels at the key.
double MeterTarget(const std::vector<uint32_t>& bins)
{
    double total = 0.0;
    for (const uint32_t b : bins)
        total += b;
    const double low = double(kSceneLow) * total;
    const double high = double(kSceneHigh) * total;
    double below = 0.0;
    double sum = 0.0;
    for (size_t k = 0; k < bins.size(); ++k)
    {
        const double inside = std::min(below + bins[k], high) - std::max(below, low);
        if (inside > 0.0)
            sum += inside * (NR_METER_EV_MIN + (double(k) + 0.5) / NR_METER_PER_EV);
        below += bins[k];
    }
    return std::log2(double(kSceneKey)) - sum / (high - low);
}

// The same from the sorted pixels themselves, without bins: what the histogram stands for.
double MeterTargetExact(const Scene& s)
{
    std::vector<double> evs;
    for (UINT y = 0; y < s.height; ++y)
    {
        for (UINT x = 0; x < s.width; ++x)
        {
            float ev = 0.0f;
            if (Metered(s.Frame(x, y), &ev))
                evs.push_back(ev);
        }
    }
    std::sort(evs.begin(), evs.end());
    const size_t from = size_t(double(kSceneLow) * double(evs.size()));
    const size_t to = size_t(double(kSceneHigh) * double(evs.size()));
    double sum = 0.0;
    for (size_t i = from; i < to; ++i)
        sum += evs[i];
    return std::log2(double(kSceneKey)) - sum / double(to - from);
}

void CheckMeter(Scene& s)
{
    const Setup u; // for Reset alone: the meter works in the frame's own units, whatever the white point
    const UINT64 bytes = NR_METER_WORDS * 4;
    ID3D12Resource* meter = MakeBuffer(bytes, D3D12_HEAP_TYPE_DEFAULT);
    ID3D12Resource* scene = MakeTexture(DXGI_FORMAT_R32_FLOAT, 1, 1, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_RESOURCE_STATES meterState = D3D12_RESOURCE_STATE_COMMON;
    std::vector<uint32_t> left; // the meter's buffer after the resolve
    // The scene texture holding `old`, the meter over the frame (unless `measure` is false), then the resolve; returns
    // what the resolve stored.
    auto run = [&](bool measure, float old, bool reset, float seconds) {
        Reset(s, u);
        std::vector<uint8_t> value(4);
        std::memcpy(value.data(), &old, 4);
        Upload(scene, value, 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Barrier(meter, meterState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        meterState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        Barrier(s.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const unsigned table = Table(OutputView(s), {}, {}, {}, { scene, DXGI_FORMAT_R32_FLOAT }, meter, bytes);
        if (measure)
            Dispatch(g.stats, table, Constants(s, u, NR_STATS_METER), Groups(s.width, 32), Groups(s.height, 32));
        UavBarrier(meter);
        NrConstants c = Constants(s, u, NR_STATS_RESOLVE);
        c.seconds = seconds;
        if (reset)
            c.flags |= NR_FLAG_SCENE_RESET;
        Dispatch(g.stats, table, c, 1, 1);
        Barrier(s.output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        left = ReadBuffer(meter, bytes, meterState);
        const std::vector<uint8_t> got = Read(scene, 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        float stored = 0.0f;
        std::memcpy(&stored, got.data(), 4);
        return stored;
    };
    auto empty = [&] { return std::all_of(left.begin(), left.end(), [](uint32_t w) { return w == 0; }); };

    const double want = MeterTarget(MeterBins(s));
    const double exact = MeterTargetExact(s);
    const double fresh = std::log2(double(run(true, 123.0f, true, 0.0f)));
    Check(std::fabs(fresh - want) < 0.01 && std::fabs(fresh - exact) < 0.0625,
          "scene meter, %s: afresh, exposure 2^%+.3f (the CPU from the same bins 2^%+.3f, from the sorted pixels "
          "2^%+.3f)",
          s.format->name, fresh, want, exact);
    Check(empty(), "scene meter, %s: the histogram is empty again after the resolve", s.format->name);

    // A quarter of a second after a white point 3 EV away: 1 - e^-0.5 = 39% of the way.
    const float old = float(std::exp2(want + 3.0));
    const double eased = std::log2(double(run(true, old, false, 0.25f)));
    const double wantEased = want + 3.0 * std::exp(-0.25 / double(kSceneSeconds));
    Check(std::fabs(eased - wantEased) < 0.01, "scene meter, %s: eased for 0.25 s from 2^%+.3f: 2^%+.3f (want 2^%+.3f)",
          s.format->name, want + 3.0, eased, wantEased);

    // Nothing to measure (the meter did not run, so the histogram is empty): the last value stays, even on a reset.
    const float kept = run(false, old, true, 0.0f);
    Check(kept == old && empty(), "scene meter, %s: with nothing to measure the last value stays (%g, want %g)",
          s.format->name, double(kept), double(old));

    // A stored value that is not a number is replaced by this frame's, without a reset.
    const double replaced = std::log2(double(run(true, NAN, false, 0.25f)));
    Check(std::fabs(replaced - want) < 0.01, "scene meter, %s: a stored NaN gives way to 2^%+.3f (want 2^%+.3f)",
          s.format->name, replaced, want);
    meter->Release();
    scene->Release();
}

// ---------------------------------------------------------------------------------------------------------------
// The frame dump (nr.hlsl's DUMP modes) and the preview.

void CheckDump(Scene& s, const Setup& u, const std::vector<uint8_t>& modelBytes)
{
    const UINT rw = s.width / NR_DUMP_SCALE, rh = s.height / NR_DUMP_SCALE;
    const UINT cw = std::min<UINT>(NR_DUMP_CROP, s.width), ch = std::min<UINT>(NR_DUMP_CROP, s.height);
    const UINT cx = (s.width - cw) / 2, cy = (s.height - ch) / 2;
    const UINT64 end = NR_DUMP_HEADER_BYTES + UINT64(NR_DUMP_PICTURES) * (UINT64(rw) * rh + UINT64(cw) * ch) * 8;
    const UINT64 bytes = end + 8;
    ID3D12Resource* dump = MakeBuffer(bytes, D3D12_HEAP_TYPE_DEFAULT);

    Reset(s, u);
    RecordEncode(s, u);
    Upload(s.model, modelBytes, 8, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(dump, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(s.proxy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(s.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const unsigned dumpTable = Table(OutputView(s), { s.model, DXGI_FORMAT_R16G16B16A16_FLOAT },
                                     { s.proxy, DXGI_FORMAT_R16G16B16A16_FLOAT }, ExposureView(s, u), {}, dump, bytes);
    const unsigned compositeTable = Table({}, { s.model, DXGI_FORMAT_R16G16B16A16_FLOAT },
                                          { s.proxy, DXGI_FORMAT_R16G16B16A16_FLOAT }, ExposureView(s, u), OutputView(s));
    auto dumpPasses = [&](uint32_t mode) {
        NrConstants c = Constants(s, u, mode);
        c.part = 0;
        c.outWidth = rw;
        c.outHeight = rh;
        Dispatch(g.nr, dumpTable, c, Groups(rw, 8), Groups(rh, 8));
        c.part = 1;
        c.outWidth = cw;
        c.outHeight = ch;
        Dispatch(g.nr, dumpTable, c, Groups(cw, 8), Groups(ch, 8));
    };
    dumpPasses(NR_MODE_DUMP_BEFORE);
    Barrier(s.output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Dispatch(g.nr, compositeTable, Constants(s, u, NR_MODE_COMPOSITE), Groups(s.width, 8), Groups(s.height, 8));
    Barrier(s.output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    dumpPasses(NR_MODE_DUMP_AFTER);
    Barrier(s.output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Barrier(s.proxy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const std::vector<uint32_t> w = ReadBuffer(dump, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    dump->Release();
    const std::vector<uint8_t> outputBytes = Read(s.output, s.format->bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const std::vector<Pixel> proxy = DecodeAll(kHalf, EncodeAndReadProxy(s, u));
    const std::vector<Pixel> model = DecodeAll(kHalf, modelBytes);

    Check(w[NR_D_TAG] == u.frame && w[end / 4] == u.frame && Bits(w[NR_D_WHITE]) == WhiteOf(u) &&
              Bits(w[NR_D_EXPOSURE]) == (u.useExposure ? u.exposure : 0.0f),
          "dump, %s: frame %u at both ends (%u, %u), white point %g, exposure %g", s.format->name, u.frame, w[NR_D_TAG],
          w[end / 4], double(Bits(w[NR_D_WHITE])), double(Bits(w[NR_D_EXPOSURE])));

    auto stored = [&](UINT part, UINT picture, UINT x, UINT y) {
        const UINT64 pixel = part == 0 ? UINT64(picture) * rw * rh + UINT64(y) * rw + x
                                       : UINT64(NR_DUMP_PICTURES) * rw * rh + UINT64(picture) * cw * ch + UINT64(y) * cw + x;
        const uint32_t* p = w.data() + (NR_DUMP_HEADER_BYTES + pixel * 8) / 4;
        return Pixel { HalfToFloat(uint16_t(p[0])), HalfToFloat(uint16_t(p[0] >> 16)), HalfToFloat(uint16_t(p[1])),
                       HalfToFloat(uint16_t(p[1] >> 16)) };
    };
    auto outputAt = [&](UINT x, UINT y) {
        const size_t i = size_t(y + s.baseY) * s.textureWidth + x + s.baseX;
        return Decode(*s.format, outputBytes.data() + i * s.format->bytes);
    };
    // The reduced pictures, everywhere; the crop, everywhere.
    size_t bad = 0;
    for (UINT y = 0; y < rh; ++y)
    {
        for (UINT x = 0; x < rw; ++x)
        {
            Pixel mean[4] = {};
            unsigned zebra = 0;
            for (UINT dy = 0; dy < NR_DUMP_SCALE; ++dy)
            {
                for (UINT dx = 0; dx < NR_DUMP_SCALE; ++dx)
                {
                    const UINT fx = x * NR_DUMP_SCALE + dx, fy = y * NR_DUMP_SCALE + dy;
                    const Pixel sources[4] = { s.Frame(fx, fy), proxy[size_t(fy) * s.width + fx],
                                               model[size_t(fy) * s.width + fx], outputAt(fx, fy) };
                    for (int k = 0; k < 4; ++k)
                    {
                        mean[k].r += sources[k].r;
                        mean[k].g += sources[k].g;
                        mean[k].b += sources[k].b;
                        mean[k].a += sources[k].a;
                    }
                    zebra = std::max(zebra, ref::ZebraClass(Rgb(sources[1]), u.linear, u.knobs.shoulder));
                }
            }
            for (int k = 0; k < 4; ++k)
            {
                const float n = float(NR_DUMP_SCALE * NR_DUMP_SCALE);
                Pixel want = { mean[k].r / n, mean[k].g / n, mean[k].b / n, mean[k].a / n };
                if (k == 1)
                    want.a = float(zebra);
                const Pixel got = stored(0, UINT(k), x, y);
                bad += !ClosePixel(kHalf, got, Quantise(kHalf, want)) || (k == 1 && got.a != want.a);
            }
        }
    }
    for (UINT y = 0; y < ch; ++y)
    {
        for (UINT x = 0; x < cw; ++x)
        {
            const UINT fx = cx + x, fy = cy + y;
            const Pixel p = proxy[size_t(fy) * s.width + fx];
            const Pixel wants[4] = { s.Frame(fx, fy),
                                     { p.r, p.g, p.b, float(ref::ZebraClass(Rgb(p), u.linear, u.knobs.shoulder)) },
                                     model[size_t(fy) * s.width + fx], outputAt(fx, fy) };
            for (int k = 0; k < 4; ++k)
            {
                const Pixel got = stored(1, UINT(k), x, y);
                bad += !ClosePixel(kHalf, got, Quantise(kHalf, wants[k])) || (k == 1 && got.a != wants[k].a);
            }
        }
    }
    Check(bad == 0, "dump, %s: the four %ux%u reduced pictures and the four %ux%u crops, zebra classes in the proxy's "
                    "alpha (%zu values wrong)",
          s.format->name, rw, rh, cw, ch, bad);
}

void CheckPreview(Scene& s, const Setup& u)
{
    const UINT scale = 4, pw = Groups(s.width, scale), ph = Groups(s.height, scale);
    const std::vector<uint8_t> proxyBytes = EncodeAndReadProxy(s, u);
    const std::vector<Pixel> proxy = DecodeAll(kHalf, proxyBytes);
    ID3D12Resource* preview = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, pw, ph, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Upload(s.model, proxyBytes, 8, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(s.proxy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const unsigned table = Table({}, { s.model, DXGI_FORMAT_R16G16B16A16_FLOAT }, { s.proxy, DXGI_FORMAT_R16G16B16A16_FLOAT },
                                 {}, { preview, DXGI_FORMAT_R16G16B16A16_FLOAT });
    NrConstants c = Constants(s, u, NR_MODE_PREVIEW);
    c.flags |= NR_FLAG_ZEBRA;
    c.scale = scale;
    c.outWidth = pw;
    c.outHeight = ph;
    Dispatch(g.nr, table, c, Groups(pw, 8), Groups(ph, 8));
    Barrier(s.proxy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const std::vector<Pixel> got = DecodeAll(kHalf, Read(preview, 8, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    preview->Release();

    size_t bad = 0, striped[3] = {};
    for (UINT y = 0; y < ph; ++y)
    {
        for (UINT x = 0; x < pw; ++x)
        {
            Pixel sum = {};
            unsigned marked = 0;
            for (UINT dy = 0; dy < scale; ++dy)
            {
                for (UINT dx = 0; dx < scale; ++dx)
                {
                    const UINT qx = std::min(x * scale + dx, s.width - 1), qy = std::min(y * scale + dy, s.height - 1);
                    const Pixel e = WithAlpha(ref::Saturate(Rgb(proxy[size_t(qy) * s.width + qx])), 1.0f);
                    sum.r += e.r;
                    sum.g += e.g;
                    sum.b += e.b;
                    marked = std::max(marked, ref::ZebraClass(Rgb(e), true, u.knobs.shoulder));
                }
            }
            const float n = float(scale * scale);
            Pixel want = { sum.r / n, sum.g / n, sum.b / n, 1.0f };
            if (marked != 0 && ((x + y) / 4) % 2 == 0)
            {
                want = marked == 2 ? Pixel { 1.0f, 0.15f, 0.15f, 1.0f } : Pixel { 0.95f, 0.3f, 0.95f, 1.0f };
                ++striped[marked];
            }
            bad += !ClosePixel(kHalf, got[size_t(y) * pw + x], Quantise(kHalf, want));
        }
    }
    Check(bad == 0 && striped[1] > 0 && striped[2] > 0,
          "preview, %s: %ux%u at 1/%u, zebra stripes on %zu shoulder and %zu heavy squares (%zu pixels wrong)",
          s.format->name, pw, ph, scale, striped[1], striped[2], bad);
}

// ---------------------------------------------------------------------------------------------------------------
// ModelScale (NR_FLAG_SCALED): the model on a smaller copy of the frame. Of each side and its model side here one is
// odd and the other even, so no frame pixel's centre falls exactly between two model texels, and the CPU and the GPU
// always pick the same four texels around it.

const Pixel kSentinel = { 0.25f, 0.5f, 0.75f, 1.0f };

// How far the scaled composite may part from the CPU's, beyond the format's own steps, as a share of the pixel's
// largest channel (CheckOutput). The GPU takes the guide's log2 and the bilinear weights to a few millionths, and the
// change follows the guide by slopes of up to a few.
constexpr float kScaledSpread = 1.0f / 65536.0f;

// The model's picture (mw x mh) out of a frame-sized texture's pixels, and back into one: the rest white, which a
// pass reading beyond the picture would show as a change.
std::vector<Pixel> Picture(const std::vector<Pixel>& texture, UINT width, UINT mw, UINT mh)
{
    std::vector<Pixel> out(size_t(mw) * mh);
    for (UINT y = 0; y < mh; ++y)
        std::copy_n(texture.begin() + ptrdiff_t(size_t(y) * width), mw, out.begin() + ptrdiff_t(size_t(y) * mw));
    return out;
}

std::vector<uint8_t> Framed(const Scene& s, const std::vector<Pixel>& picture, UINT mw, UINT mh)
{
    std::vector<Pixel> texture(size_t(s.width) * s.height, Pixel { 1.0f, 1.0f, 1.0f, 1.0f });
    for (UINT y = 0; y < mh; ++y)
        std::copy_n(picture.begin() + ptrdiff_t(size_t(y) * mw), mw, texture.begin() + ptrdiff_t(size_t(y) * s.width));
    return Encode(texture);
}

// The CPU's encode of model texel (qx, qy): the light of the frame pixels it spans, each weighted by how much of it
// the texel covers, sRGB-encoded.
Pixel ExpectedScaledProxy(const Scene& s, const Setup& u, UINT qx, UINT qy)
{
    const float stepX = float(s.width) / float(ModelW(s, u)), stepY = float(s.height) / float(ModelH(s, u));
    const float loX = float(qx) * stepX, loY = float(qy) * stepY;
    const float hiX = std::min(loX + stepX, float(s.width)), hiY = std::min(loY + stepY, float(s.height));
    const UINT endX = std::min(UINT(std::ceil(hiX)), s.width), endY = std::min(UINT(std::ceil(hiY)), s.height);
    const float white = WhiteOf(u);
    ref::F3 sum = { 0.0f, 0.0f, 0.0f };
    float weight = 0.0f;
    for (UINT y = UINT(loY); y < endY; ++y)
    {
        const float wy = std::min(hiY, float(y) + 1.0f) - std::max(loY, float(y));
        for (UINT x = UINT(loX); x < endX; ++x)
        {
            const float w = wy * (std::min(hiX, float(x) + 1.0f) - std::max(loX, float(x)));
            if (w <= 0.0f)
                continue;
            ref::F3 frame = Rgb(s.Frame(x, y));
            if (u.linear && u.card && ref::InCard(x, y, u.cardRect))
                frame = ref::Card(x, y, u.cardRect) * white;
            sum = sum + ref::ProxyLight(frame, white, u.linear, u.knobs.shoulder) * w;
            weight += w;
        }
    }
    ref::F3 mean = weight > 0.0f ? sum / weight : ref::F3 { 0.0f, 0.0f, 0.0f };
    mean = { ref::Min(mean.r, 1.0f), ref::Min(mean.g, 1.0f), ref::Min(mean.b, 1.0f) };
    return WithAlpha(ref::SrgbEncode(mean), 1.0f);
}

// The encode over the model's picture as the CPU has it, and the proxy beyond it left as it was.
void CheckScaledEncode(Scene& s, const Setup& u, const char* what)
{
    const UINT mw = ModelW(s, u), mh = ModelH(s, u);
    Reset(s, u);
    Upload(s.proxy, Encode(std::vector<Pixel>(size_t(s.width) * s.height, kSentinel)), 8,
           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    RecordEncode(s, u);
    const std::vector<Pixel> proxy = DecodeAll(kHalf, Read(s.proxy, 8, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    size_t bad = 0, outside = 0;
    UINT firstX = 0, firstY = 0;
    Pixel got, want;
    for (UINT y = 0; y < s.height; ++y)
    {
        for (UINT x = 0; x < s.width; ++x)
        {
            const Pixel& p = proxy[size_t(y) * s.width + x];
            if (x >= mw || y >= mh)
            {
                outside += p.r != kSentinel.r || p.g != kSentinel.g || p.b != kSentinel.b || p.a != kSentinel.a;
                continue;
            }
            const Pixel e = ExpectedScaledProxy(s, u, x, y);
            if ((!ClosePixel(kHalf, p, e) || p.a != 1.0f) && bad++ == 0)
                firstX = x, firstY = y, got = p, want = e;
        }
    }
    if (bad == 0)
        Check(outside == 0, "encode, %s, ModelScale %ux%u of %ux%u, %s: the area averages as the CPU, %zu texels "
                            "beyond the model's picture written",
              s.format->name, mw, mh, s.width, s.height, what, outside);
    else
        Check(false, "encode, %s, ModelScale %ux%u of %ux%u, %s: %zu texels differ; first (%u,%u) got %g %g %g %g, "
                     "want %g %g %g",
              s.format->name, mw, mh, s.width, s.height, what, bad, firstX, firstY, got.r, got.g, got.b, got.a, want.r,
              want.g, want.b);
}

// The fit's three textures as the GPU left them (frame-sized, as dxgi.dll makes them; the model's picture at their
// top left): the slope (gain, red, blue), the value at the texel's own guide and that guide, what the model changed.
struct Fit
{
    std::vector<Pixel> slope, value, raw;
};

// Encode, the model's output (a frame-sized texture, the model's picture at its top left), the fit and the
// composite, as dxgi.dll runs them with ModelScale. Returns the Output's bytes; the fit's textures into `fit`.
std::vector<uint8_t> RunScaled(Scene& s, const Setup& u, const std::vector<uint8_t>& modelBytes, Fit* fit)
{
    const UINT mw = ModelW(s, u), mh = ModelH(s, u);
    ID3D12Resource* textures[3];
    View views[3];
    for (int i = 0; i < 3; ++i)
    {
        textures[i] = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, s.width, s.height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        views[i] = { textures[i], DXGI_FORMAT_R16G16B16A16_FLOAT };
    }
    Reset(s, u);
    RecordEncode(s, u);
    Upload(s.model, modelBytes, 8, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(s.proxy, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const View model = { s.model, DXGI_FORMAT_R16G16B16A16_FLOAT };
    const View proxy = { s.proxy, DXGI_FORMAT_R16G16B16A16_FLOAT };
    Dispatch(g.fit, FitTable(model, proxy, views[0], views[1], views[2]), Constants(s, u, NR_MODE_COMPOSITE),
             Groups(mw, 8), Groups(mh, 8));
    for (ID3D12Resource* t : textures)
        Barrier(t, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const unsigned table =
        Table({}, model, proxy, ExposureView(s, u), OutputView(s), nullptr, 0, views[0], views[1], views[2]);
    Dispatch(g.nr, table, Constants(s, u, NR_MODE_COMPOSITE), Groups(s.width, 8), Groups(s.height, 8));
    Barrier(s.proxy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const std::vector<uint8_t> output = Read(s.output, s.format->bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (fit != nullptr)
    {
        std::vector<Pixel>* const into[3] = { &fit->slope, &fit->value, &fit->raw };
        for (int i = 0; i < 3; ++i)
            *into[i] = DecodeAll(kHalf, Read(textures[i], 8, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    }
    for (ID3D12Resource* t : textures)
        t->Release();
    return output;
}

// The CPU copy of nr_fit.hlsl over the model's picture (`proxy` and `model`, mw x mh), checked against the GPU's
// textures. The half floats the GPU stores round the CPU's floats either way, and a fit's numerator can cancel, hence
// an absolute allowance of 1/4096 (of an EV, or of a chromaticity) besides the format's own.
void CheckFit(const Setup& u, UINT width, UINT mw, UINT mh, const std::vector<Pixel>& proxy,
              const std::vector<Pixel>& model, const Fit& fit, const char* name, const char* what)
{
    const size_t count = size_t(mw) * mh;
    std::vector<float> guide(count);
    std::vector<ref::F3> target(count); // gain, red, blue
    for (size_t i = 0; i < count; ++i)
    {
        const ref::F3 p = ref::ProxyLinear(Rgb(proxy[i]));
        guide[i] = ref::Guide(p);
        target[i] = { 0.0f, 0.0f, 0.0f }; // a texel the model returned as it got it changed nothing
        if (model[i].r != proxy[i].r || model[i].g != proxy[i].g || model[i].b != proxy[i].b)
        {
            const ref::F3 o = ref::ModelLinear(Rgb(model[i]), Rgb(proxy[i]));
            const ref::F3 chroma = ref::ModelChroma(p, o);
            target[i] = { ref::ModelGain(p, o, u.linear, u.knobs), chroma.r, chroma.b };
        }
    }
    auto at = [&](int x, int y) {
        return size_t(std::clamp(y, 0, int(mh) - 1)) * mw + size_t(std::clamp(x, 0, int(mw) - 1));
    };
    std::vector<ref::F3> a(count), b(count);
    for (UINT y = 0; y < mh; ++y)
    {
        for (UINT x = 0; x < mw; ++x)
        {
            const float centre = guide[at(int(x), int(y))];
            float mean = 0.0f, square = 0.0f;
            ref::F3 t = { 0.0f, 0.0f, 0.0f }, product = { 0.0f, 0.0f, 0.0f };
            for (int dy = -1; dy <= 1; ++dy)
            {
                for (int dx = -1; dx <= 1; ++dx)
                {
                    const size_t k = at(int(x) + dx, int(y) + dy);
                    const float d = guide[k] - centre;
                    mean += d;
                    square += d * d;
                    t = t + target[k];
                    product = product + target[k] * d;
                }
            }
            mean /= 9.0f;
            square /= 9.0f;
            t = t / 9.0f;
            product = product / 9.0f;
            const float variance = std::max(square - mean * mean, 0.0f);
            const ref::F3 slope = (product - t * mean) / (variance + ref::kFitEps);
            a[size_t(y) * mw + x] = slope;
            b[size_t(y) * mw + x] = t - slope * (mean + centre);
        }
    }
    auto close = [](float got, float want) {
        return std::fabs(got - want) <= std::max(1.0f / 4096.0f, std::fabs(want) / 256.0f);
    };
    size_t bad = 0, changed = 0;
    UINT firstX = 0, firstY = 0;
    Pixel gotSlope, wantSlope, gotValue, wantValue;
    for (UINT y = 0; y < mh; ++y)
    {
        for (UINT x = 0; x < mw; ++x)
        {
            ref::F3 sa = { 0.0f, 0.0f, 0.0f }, sb = { 0.0f, 0.0f, 0.0f };
            for (int dy = -1; dy <= 1; ++dy)
            {
                for (int dx = -1; dx <= 1; ++dx)
                {
                    sa = sa + a[at(int(x) + dx, int(y) + dy)];
                    sb = sb + b[at(int(x) + dx, int(y) + dy)];
                }
            }
            sa = sa / 9.0f;
            sb = sb / 9.0f;
            const size_t i = size_t(y) * mw + x;
            const Pixel slope = { sa.r, sa.g, sa.b, 0.0f };
            const ref::F3 v = sb + sa * guide[i];
            const Pixel value = { v.r, v.g, v.b, guide[i] };
            const Pixel raw = { target[i].r, target[i].g, target[i].b, 0.0f };
            const size_t j = size_t(y) * width + x; // the textures are frame-sized
            const Pixel& gs = fit.slope[j];
            const Pixel& gv = fit.value[j];
            const Pixel& gr = fit.raw[j];
            changed += raw.r != 0.0f || raw.g != 0.0f || raw.b != 0.0f;
            const bool ok = close(gs.r, slope.r) && close(gs.g, slope.g) && close(gs.b, slope.b) &&
                            close(gv.r, value.r) && close(gv.g, value.g) && close(gv.b, value.b) &&
                            close(gv.a, value.a) && close(gr.r, raw.r) && close(gr.g, raw.g) && close(gr.b, raw.b);
            if (!ok && bad++ == 0)
                firstX = x, firstY = y, gotSlope = gs, wantSlope = slope, gotValue = gv, wantValue = value;
        }
    }
    if (bad == 0)
        Check(true, "fit, %s, ModelScale %ux%u, %s: slopes, values and changes of %zu texels as the CPU (%zu changed)",
              name, mw, mh, what, count, changed);
    else
        Check(false, "fit, %s, ModelScale %ux%u, %s: %zu texels wrong; first (%u,%u) slope got %g %g %g want %g %g %g, "
                     "value got %g %g %g %g want %g %g %g %g",
              name, mw, mh, what, bad, firstX, firstY, gotSlope.r, gotSlope.g, gotSlope.b, wantSlope.r, wantSlope.g,
              wantSlope.b, gotValue.r, gotValue.g, gotValue.b, gotValue.a, wantValue.r, wantValue.g, wantValue.b,
              wantValue.a);
}

// The four model texels around frame pixel (x, y) and their bilinear weights, as nr.hlsl's ScaledChange takes them.
struct Taps
{
    UINT x[4], y[4];
    float weight[4];
};

Taps TapsAt(const Scene& s, const Setup& u, UINT x, UINT y)
{
    const UINT mw = ModelW(s, u), mh = ModelH(s, u);
    const float stepX = float(s.width) / float(mw), stepY = float(s.height) / float(mh);
    const float ux = std::clamp((float(x) + 0.5f) / stepX - 0.5f, 0.0f, float(mw) - 1.0f);
    const float uy = std::clamp((float(y) + 0.5f) / stepY - 0.5f, 0.0f, float(mh) - 1.0f);
    const UINT x0 = UINT(ux), y0 = UINT(uy);
    const UINT x1 = std::min(x0 + 1, mw - 1), y1 = std::min(y0 + 1, mh - 1);
    const float fx = ux - float(x0), fy = uy - float(y0);
    return { { x0, x1, x0, x1 }, { y0, y0, y1, y1 },
             { (1.0f - fx) * (1.0f - fy), fx * (1.0f - fy), (1.0f - fx) * fy, fx * fy } };
}

// nr.hlsl's ScaledChange from the GPU's own fit, for frame pixel (x, y) whose guide is `guide`.
bool ScaledChange(const Scene& s, const Setup& u, const Fit& fit, UINT x, UINT y, float guide, float* change,
                  ref::F3* chroma)
{
    const Taps taps = TapsAt(s, u, x, y);
    ref::F3 sum = { 0.0f, 0.0f, 0.0f };
    ref::F3 low = { 1.0e30f, 1.0e30f, 1.0e30f }, high = { -1.0e30f, -1.0e30f, -1.0e30f };
    for (int i = 0; i < 4; ++i)
    {
        const size_t j = size_t(taps.y[i]) * s.width + taps.x[i];
        const Pixel& value = fit.value[j];
        const ref::F3 raw = Rgb(fit.raw[j]);
        sum = sum + (Rgb(fit.slope[j]) * (guide - value.a) + Rgb(value)) * taps.weight[i];
        low = { ref::Min(low.r, raw.r), ref::Min(low.g, raw.g), ref::Min(low.b, raw.b) };
        high = { ref::Max(high.r, raw.r), ref::Max(high.g, raw.g), ref::Max(high.b, raw.b) };
    }
    const ref::F3 held = { ref::Min(ref::Max(sum.r, low.r), high.r), ref::Min(ref::Max(sum.g, low.g), high.g),
                           ref::Min(ref::Max(sum.b, low.b), high.b) };
    *change = held.r;
    *chroma = ref::ChromaFromRedBlue(held.g, held.b);
    return low.r != 0.0f || low.g != 0.0f || low.b != 0.0f || high.r != 0.0f || high.g != 0.0f || high.b != 0.0f;
}

// What the composite with ModelScale must leave in texture pixel (tx, ty), from the GPU's own fit.
Pixel ExpectedScaledOutput(const Scene& s, const Setup& u, const Fit& fit, UINT tx, UINT ty, bool* exact)
{
    const size_t i = size_t(ty) * s.textureWidth + tx;
    *exact = true;
    if (tx < s.baseX || tx >= s.baseX + s.width || ty < s.baseY || ty >= s.baseY + s.height)
        return s.texture[i];
    const UINT x = tx - s.baseX, y = ty - s.baseY;
    Pixel frame = s.texture[i];
    const float white = WhiteOf(u);
    bool written = false;
    if (u.card && ref::InCard(x, y, u.cardRect))
    {
        const ref::F3 c = ref::Card(x, y, u.cardRect) * white;
        frame = { c.r, c.g, c.b, frame.a };
        written = true;
    }
    Pixel result = frame;
    float change = 0.0f;
    ref::F3 chroma;
    if (u.knobs.detail != 0.0f && Finite(frame) &&
        ScaledChange(s, u, fit, x, y, ref::Guide(ref::ProxyLight(Rgb(frame), white, u.linear, u.knobs.shoulder)), &change,
                     &chroma))
    {
        const ref::F3 r = ref::ApplyChange(Rgb(frame), change, chroma, u.linear, u.knobs);
        result = { r.r, r.g, r.b, frame.a };
        written = true;
    }
    *exact = !written;
    return written ? Quantise(*s.format, result) : result;
}

// Where the model brightened all four texels around a pixel by exactly `ev` (no clipping, well above the floor), the
// pixel must come out brighter by that much: a check that leans on neither CPU copy.
void CheckScaledRatio(const Scene& s, const Setup& u, const Fit& fit, const std::vector<uint8_t>& output, float ev,
                      const char* what)
{
    size_t counted = 0, bad = 0;
    double worst = 0.0;
    for (UINT y = 1; y < s.height; ++y)
    {
        for (UINT x = 0; x < s.width; ++x)
        {
            const Taps taps = TapsAt(s, u, x, y);
            bool uniform = true;
            for (int i = 0; i < 4; ++i)
                uniform &= std::fabs(fit.raw[size_t(taps.y[i]) * s.width + taps.x[i]].r - ev) < 0.002f;
            const Pixel& f = s.Frame(x, y);
            if (!uniform || !Finite(f))
                continue;
            const size_t i = size_t(y + s.baseY) * s.textureWidth + x + s.baseX;
            const Pixel o = Decode(*s.format, output.data() + i * s.format->bytes);
            // A display-encoded frame in linear light, where the gain is applied; its darks are too coarse in 8 bits
            // for a ratio.
            const bool linear = s.format->linear;
            const float before = linear ? Luma(f) : Luma(WithAlpha(ref::SrgbDecode(Rgb(f)), 1.0f));
            const float after = linear ? Luma(o) : Luma(WithAlpha(ref::SrgbDecode(Rgb(o)), 1.0f));
            if (before < (linear ? 1.0e-6f : 0.02f))
                continue;
            const double ratio = std::log2(double(after) / double(before));
            worst = std::max(worst, std::fabs(ratio - ev));
            ++counted;
            const double allowed = s.format->dxgi == DXGI_FORMAT_R11G11B10_FLOAT ? 0.06 : linear ? 0.02 : 0.1;
            bad += std::fabs(ratio - ev) > allowed;
        }
    }
    Check(counted > 100 && bad == 0,
          "composite, %s, %s: Output / frame is %+.2f EV over the %zu pixels whose four model texels all changed "
          "by that (worst off by %.3f EV)",
          s.format->name, what, double(ev), counted, worst);
}

// Everything ModelScale adds, on one frame: `base` with the model at mw x mh.
void TestScaled(Scene& s, const Setup& base, UINT mw, UINT mh)
{
    Setup u = base;
    u.modelWidth = mw;
    u.modelHeight = mh;
    char what[160];
    CheckScaledEncode(s, u, u.linear ? "white point 2.5" : "display-encoded");
    if (u.linear)
    {
        Setup exposure = u;
        exposure.whiteScale = 5.0f;
        exposure.useExposure = true;
        exposure.exposure = 2.0f;
        CheckScaledEncode(s, exposure, "pre-exposure 5 over an exposure texture of 2");
        Setup card = u;
        card.card = true;
        CheckScaledEncode(s, card, "the calibration card");
    }

    const std::vector<Pixel> proxy = Picture(DecodeAll(kHalf, EncodeAndReadProxy(s, u)), s.width, mw, mh);
    std::snprintf(what, sizeof what, "ModelScale %ux%u, the model returns its input", mw, mh);
    CheckUntouched(s, RunScaled(s, u, Framed(s, proxy, mw, mh), nullptr), what);

    const std::vector<Pixel> brighter = MakeModel(proxy, [](ref::F3 p, size_t) { return p * std::exp2(0.5f); });
    Setup off = u;
    off.knobs.detail = 0.0f;
    std::snprintf(what, sizeof what, "ModelScale %ux%u, DetailStrength 0", mw, mh);
    CheckUntouched(s, RunScaled(s, off, Framed(s, brighter, mw, mh), nullptr), what);

    Fit fit;
    std::vector<uint8_t> output = RunScaled(s, u, Framed(s, brighter, mw, mh), &fit);
    CheckFit(u, s.width, mw, mh, proxy, brighter, fit, s.format->name, "model +0.5 EV");
    std::snprintf(what, sizeof what, "ModelScale %ux%u, model +0.5 EV", mw, mh);
    CheckOutput(
        s, output, [&](UINT tx, UINT ty, bool* exact) { return ExpectedScaledOutput(s, u, fit, tx, ty, exact); }, what,
        kScaledSpread);
    CheckScaledRatio(s, u, fit, output, 0.5f, what);

    // Brighter and warmer by the column, every third texel left as it was: edges in the model's change for the fit to
    // follow and the hold to clamp, with the highlights handed back and the colour turned up.
    std::vector<Pixel> mixed = MakeModel(proxy, [mw](ref::F3 p, size_t i) {
        const float k = 1.0f + 0.25f * float(i % mw) / float(mw);
        return ref::F3 { p.r * 1.12f * k, p.g * 1.05f * k, p.b * 0.97f };
    });
    for (size_t i = 0; i < mixed.size(); i += 3)
        mixed[i] = proxy[i];
    Setup knobs = u;
    if (u.linear)
        knobs.knobs.highlight = 1.0f;
    knobs.knobs.colour = 1.5f;
    knobs.knobs.detail = 1.3f;
    output = RunScaled(s, knobs, Framed(s, mixed, mw, mh), &fit);
    CheckFit(knobs, s.width, mw, mh, proxy, mixed, fit, s.format->name, "mixed model");
    std::snprintf(what, sizeof what, "ModelScale %ux%u, mixed model, %sColourStrength 1.5, DetailStrength 1.3", mw, mh,
                  u.linear ? "HighlightRestore 1, " : "");
    CheckOutput(
        s, output, [&](UINT tx, UINT ty, bool* exact) { return ExpectedScaledOutput(s, knobs, fit, tx, ty, exact); },
        what, kScaledSpread);
}

// ---------------------------------------------------------------------------------------------------------------

void TestLinear(const Format& f, UINT border)
{
    if (!TypedLoad(f.dxgi))
    {
        Say("skip  %s: this GPU cannot load it through a UAV, so dxgi.dll stands down for it too", f.name);
        return;
    }
    Scene s = MakeScene(f, 203, 117, border);
    Setup base;

    CheckEncode(s, base, "white point 2.5, Shoulder 0.70");
    Setup exposure = base;
    exposure.whiteScale = 5.0f;
    exposure.useExposure = true;
    exposure.exposure = 2.0f;
    CheckEncode(s, exposure, "pre-exposure 5 over an exposure texture of 2");
    Setup soft = base;
    soft.knobs.shoulder = 0.5f;
    soft.whiteScale = 0.8f;
    CheckEncode(s, soft, "white point 0.8, Shoulder 0.50");

    const std::vector<uint8_t> proxyBytes = EncodeAndReadProxy(s, base);
    const std::vector<Pixel> proxy = DecodeAll(kHalf, proxyBytes);
    CheckUntouched(s, RunComposite(s, base, proxyBytes), "the model returns its input");

    const std::vector<Pixel> brighter = MakeModel(proxy, [](ref::F3 p, size_t) { return p * std::exp2(0.5f); });
    const std::vector<uint8_t> brighterBytes = Encode(brighter);
    Setup off = base;
    off.knobs.detail = 0.0f;
    CheckUntouched(s, RunComposite(s, off, brighterBytes), "DetailStrength 0");

    std::vector<uint8_t> output = RunComposite(s, base, brighterBytes);
    CheckComposite(s, base, proxy, brighter, output, "model +0.5 EV");
    CheckRatio(s, proxy, output, 0.5f, "model +0.5 EV");

    const std::vector<Pixel> much = MakeModel(proxy, [](ref::F3 p, size_t) { return p * 8.0f; });
    output = RunComposite(s, base, Encode(much));
    CheckComposite(s, base, proxy, much, output, "model +3 EV, MaxGainEV 1");
    CheckRatio(s, proxy, output, 1.0f, "model +3 EV, MaxGainEV 1");

    // Some pixels a little brighter and warmer, every third left exactly as the model got it, with the highlights
    // handed back.
    std::vector<Pixel> mixed =
        MakeModel(proxy, [](ref::F3 p, size_t) { return ref::F3 { p.r * 1.12f, p.g * 1.05f, p.b * 0.97f }; });
    for (size_t i = 0; i < mixed.size(); i += 3)
        mixed[i] = proxy[i];
    Setup highlight = base;
    highlight.knobs.highlight = 1.0f;
    highlight.knobs.colour = 1.5f;
    highlight.knobs.detail = 1.3f;
    CheckComposite(s, highlight, proxy, mixed, RunComposite(s, highlight, Encode(mixed)),
                   "mixed model, HighlightRestore 1, ColourStrength 1.5, DetailStrength 1.3");
    CheckClippedWhite(s, proxy);

    CheckCard(s);
    CheckStats(s, base, brighterBytes);
    Setup statsExposure = exposure;
    statsExposure.frame = 777;
    CheckStats(s, statsExposure, Encode(mixed));
    CheckMeter(s);
    CheckDump(s, base, brighterBytes);
    CheckPreview(s, base);
    TestScaled(s, base, 140, 78); // about 0.69 of each side
    Drop(s);
}

void TestDisplayEncoded(const Format& f)
{
    if (!TypedLoad(f.dxgi))
    {
        Say("skip  %s: this GPU cannot load it through a UAV, so dxgi.dll stands down for it too", f.name);
        return;
    }
    Scene s = MakeScene(f, 160, 90, 0);
    Setup base;
    base.linear = false;
    base.whiteScale = 1.0f;
    CheckEncode(s, base, "display-encoded: a copy");
    const std::vector<uint8_t> proxyBytes = EncodeAndReadProxy(s, base);
    const std::vector<Pixel> proxy = DecodeAll(kHalf, proxyBytes);
    CheckUntouched(s, RunComposite(s, base, proxyBytes), "the model returns its input");
    const std::vector<Pixel> brighter = MakeModel(proxy, [](ref::F3 p, size_t) { return p * std::exp2(0.5f); });
    Setup off = base;
    off.knobs.detail = 0.0f;
    CheckUntouched(s, RunComposite(s, off, Encode(brighter)), "DetailStrength 0");
    CheckComposite(s, base, proxy, brighter, RunComposite(s, base, Encode(brighter)), "model +0.5 EV");
    TestScaled(s, base, 107, 61); // about 0.67 of each side
    Drop(s);
}

// ---------------------------------------------------------------------------------------------------------------
// What the game's depth adds (DilateMotion, the sky sliders, Show sky). The passes read the game's depth through a view
// of its depth plane alone; whether such a view reads a depth-stencil texture made with a fully typed format (The Last
// of Us Part II's) the documentation does not settle, so every depth format dxgi.dll reads is tried here, drawn the
// way a game draws depth: as a depth-stencil texture.

struct DepthCase
{
    const char* name;
    DXGI_FORMAT resource; // the texture, as the game made it
    DXGI_FORMAT target;   // its depth-stencil view, to draw the pattern with
    DXGI_FORMAT view;     // what the passes read it as (nr_dx12.cpp, DepthView)
    bool stencil;
};

const DepthCase kDepthCases[] = {
    { "R32G8X24_TYPELESS (The Witcher 3)", DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
      DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, true },
    { "D32_FLOAT_S8X24_UINT (The Last of Us Part II)", DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
      DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, true },
    { "R32_TYPELESS", DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT, false },
    { "D32_FLOAT", DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT, false },
    { "R24G8_TYPELESS", DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS,
      true },
    { "D24_UNORM_S8_UINT", DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D24_UNORM_S8_UINT,
      DXGI_FORMAT_R24_UNORM_X8_TYPELESS, true },
    { "R16_TYPELESS", DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_R16_UNORM, false },
    { "D16_UNORM", DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_R16_UNORM, false },
};

// The pattern, in a 72x52 depth texture whose guide (the render subrect) is 64x48 at (5,3). Each rectangle is cleared
// over the ones before it, and the CPU reads the same list. Depths as a game without inverted depth writes them, 0
// nearest and 1 the far end; with inverted depth each is 1 minus that. 16- and 24-bit depth round them, which keeps
// their order and keeps the far end at the far end.
constexpr UINT kDepthWidth = 72, kDepthHeight = 52;
constexpr UINT kGuideX = 5, kGuideY = 3, kGuideWidth = 64, kGuideHeight = 48;

struct DepthRect
{
    float depth;
    D3D12_RECT rect; // in the texture
};

std::vector<DepthRect> DepthPattern()
{
    std::vector<DepthRect> p;
    auto add = [&p](float depth, LONG x0, LONG y0, LONG x1, LONG y1) { // in the guide's coordinates
        p.push_back({ depth, { x0 + LONG(kGuideX), y0 + LONG(kGuideY), x1 + LONG(kGuideX), y1 + LONG(kGuideY) } });
    };
    // Around the guide: nearer than anything in it.
    p.push_back({ 0.0625f, { 0, 0, LONG(kDepthWidth), LONG(kDepthHeight) } });
    add(1.0f, 0, 0, 64, 48);    // the sky
    add(0.75f, 0, 36, 64, 48);  // the ground, down to the guide's bottom edge
    add(0.25f, 20, 10, 36, 26); // a square in front
    add(0.5f, 44, 4, 45, 34);   // a bar one texel wide
    add(0.5f, 2, 30, 18, 31);   // and one a texel tall
    add(0.375f, 52, 6, 54, 30); // a bar two texels wide
    add(0.125f, 8, 8, 9, 9);    // single texels, the nearest in the guide
    add(0.125f, 60, 20, 61, 21);
    add(0.125f, 0, 0, 1, 1); // on the guide's corners and its right edge
    add(0.125f, 63, 47, 64, 48);
    add(0.125f, 63, 10, 64, 11);
    add(1.0f - 1.0e-4f, 10, 20, 11, 21); // all but at the far end: not sky
    return p;
}

float Stored(float depth, bool inverted) { return inverted ? 1.0f - depth : depth; }

// The depth the pattern leaves in texel (x, y) of the texture.
float PatternDepth(const std::vector<DepthRect>& pattern, UINT x, UINT y, bool inverted)
{
    for (size_t i = pattern.size(); i-- > 0;)
    {
        const D3D12_RECT& r = pattern[i].rect;
        if (LONG(x) >= r.left && LONG(x) < r.right && LONG(y) >= r.top && LONG(y) < r.bottom)
            return Stored(pattern[i].depth, inverted);
    }
    return Stored(1.0f, inverted); // never: the first rectangle covers the texture
}

// The CPU copy of nr.hlsl's IsSky, Closer and DepthTexel.
bool SkyDepth(float d, bool inverted) { return inverted ? d <= 1.0e-6f : d >= 1.0f - 1.0e-6f; }
bool CloserDepth(float a, float b, bool inverted) { return inverted ? a > b : a < b; }
UINT DepthTexel(UINT p, UINT size, UINT guide) { return std::min((p * 2 + 1) * guide / (size * 2), guide - 1); }

// The depth under texel (x, y) of a picture of width x height that shows the guide's view.
float DepthUnder(const std::vector<DepthRect>& pattern, UINT x, UINT y, UINT width, UINT height, bool inverted)
{
    return PatternDepth(pattern, kGuideX + DepthTexel(x, width, kGuideWidth),
                        kGuideY + DepthTexel(y, height, kGuideHeight), inverted);
}

// The depth texture with the pattern on it, in the NPSR state a game hands its depth over in; null when this GPU will
// not make this format a depth-stencil texture.
ID3D12Resource* MakeDepth(const DepthCase& d, bool inverted, const std::vector<DepthRect>& pattern,
                          ID3D12DescriptorHeap* targets)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = kDepthWidth;
    desc.Height = kDepthHeight;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = d.resource;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    ID3D12Resource* depth = nullptr;
    if (FAILED(g.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                                                 nullptr, IID_PPV_ARGS(&depth))))
        return nullptr;
    D3D12_DEPTH_STENCIL_VIEW_DESC view = {};
    view.Format = d.target;
    view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    const D3D12_CPU_DESCRIPTOR_HANDLE handle = targets->GetCPUDescriptorHandleForHeapStart();
    g.device->CreateDepthStencilView(depth, &view, handle);
    // The first rectangle is the whole texture: a whole clear, the stencil with it.
    g.list->ClearDepthStencilView(
        handle, d.stencil ? D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL : D3D12_CLEAR_FLAG_DEPTH,
        Stored(pattern[0].depth, inverted), 0, 0, nullptr);
    for (size_t i = 1; i < pattern.size(); ++i)
        g.list->ClearDepthStencilView(handle, D3D12_CLEAR_FLAG_DEPTH, Stored(pattern[i].depth, inverted), 0, 1,
                                      &pattern[i].rect);
    Barrier(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return depth;
}

// The guide's part of the constants, as nr_dx12.cpp fills it from the game's depth.
void Guide(NrConstants* c, bool inverted)
{
    c->guideWidth = kGuideWidth;
    c->guideHeight = kGuideHeight;
    c->depthBaseX = kGuideX;
    c->depthBaseY = kGuideY;
    if (inverted)
        c->flags |= NR_FLAG_DEPTH_INVERTED;
}

// The sky pass: the control mask, one texel per depth texel, (1, SkyTone, SkyStructure, 1) on the sky and 1 elsewhere.
void CheckSkyMask(ID3D12Resource* depth, const DepthCase& d, bool inverted, const std::vector<DepthRect>& pattern,
                  const char* what)
{
    ID3D12Resource* mask =
        MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, kGuideWidth, kGuideHeight, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    NrConstants c = {};
    c.mode = NR_MODE_SKY;
    c.outWidth = kGuideWidth;
    c.outHeight = kGuideHeight;
    c.skyTone = 0.25f;
    c.skyStructure = 1.5f;
    Guide(&c, inverted);
    const unsigned table = Table({ depth, d.view }, {}, {}, {}, { mask, DXGI_FORMAT_R16G16B16A16_FLOAT });
    Dispatch(g.nr, table, c, Groups(kGuideWidth, 8), Groups(kGuideHeight, 8));
    const std::vector<Pixel> got = DecodeAll(kHalf, Read(mask, 8, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    mask->Release();
    size_t sky = 0, bad = 0;
    UINT firstX = 0, firstY = 0;
    Pixel first, want;
    for (UINT y = 0; y < kGuideHeight; ++y)
    {
        for (UINT x = 0; x < kGuideWidth; ++x)
        {
            const bool isSky = SkyDepth(PatternDepth(pattern, kGuideX + x, kGuideY + y, inverted), inverted);
            sky += isSky;
            const Pixel e = { 1.0f, isSky ? 0.25f : 1.0f, isSky ? 1.5f : 1.0f, 1.0f };
            const Pixel& o = got[size_t(y) * kGuideWidth + x];
            if ((o.r != e.r || o.g != e.g || o.b != e.b || o.a != e.a) && bad++ == 0)
                firstX = x, firstY = y, first = o, want = e;
        }
    }
    if (bad == 0)
        Check(sky > 0 && sky < size_t(kGuideWidth) * kGuideHeight,
              "depth, %s: the sky mask as the CPU (%zu of %u texels sky)", what, sky, kGuideWidth * kGuideHeight);
    else
        Check(false, "depth, %s: the sky mask, %zu texels wrong; first (%u,%u) got %g %g %g %g, want %g %g %g %g", what,
              bad, firstX, firstY, first.r, first.g, first.b, first.a, want.r, want.g, want.b, want.a);
}

// The dilate pass, for motion vectors of width x height at (baseX, baseY) in a larger texture: each texel's own motion
// is (x + 0.25, -(y + 0.5)), so which texel a dilated one came from shows, and (1000, 1000) lies around them.
void CheckDilate(ID3D12Resource* depth, const DepthCase& d, bool inverted, const std::vector<DepthRect>& pattern,
                 DXGI_FORMAT format, UINT width, UINT height, UINT baseX, UINT baseY, const char* what)
{
    const bool wide = format == DXGI_FORMAT_R32G32_FLOAT;
    const UINT bytes = wide ? 8 : 4;
    const UINT textureWidth = width + 2 * baseX, textureHeight = height + 2 * baseY;
    auto store = [wide](uint8_t* at, float x, float y) {
        if (wide)
        {
            std::memcpy(at, &x, 4);
            std::memcpy(at + 4, &y, 4);
        }
        else
        {
            const uint16_t h[2] = { FloatToHalf(x), FloatToHalf(y) };
            std::memcpy(at, h, 4);
        }
    };
    auto load = [wide](const uint8_t* at, float* x, float* y) {
        if (wide)
        {
            std::memcpy(x, at, 4);
            std::memcpy(y, at + 4, 4);
        }
        else
        {
            uint16_t h[2];
            std::memcpy(h, at, 4);
            *x = HalfToFloat(h[0]);
            *y = HalfToFloat(h[1]);
        }
    };
    std::vector<uint8_t> data(size_t(textureWidth) * textureHeight * bytes);
    for (UINT ty = 0; ty < textureHeight; ++ty)
    {
        for (UINT tx = 0; tx < textureWidth; ++tx)
        {
            const bool inside = tx >= baseX && tx < baseX + width && ty >= baseY && ty < baseY + height;
            store(data.data() + (size_t(ty) * textureWidth + tx) * bytes, inside ? float(tx - baseX) + 0.25f : 1000.0f,
                  inside ? -(float(ty - baseY) + 0.5f) : 1000.0f);
        }
    }
    ID3D12Resource* motion =
        MakeTexture(format, textureWidth, textureHeight, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Upload(motion, data, bytes, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* dilated = MakeTexture(format, width, height, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    NrConstants c = {};
    c.mode = NR_MODE_DILATE;
    c.outWidth = width;
    c.outHeight = height;
    c.motionBaseX = baseX;
    c.motionBaseY = baseY;
    Guide(&c, inverted);
    const unsigned table = Table({ depth, d.view }, { motion, format }, {}, {}, { dilated, format });
    Dispatch(g.nr, table, c, Groups(width, 8), Groups(height, 8));
    const std::vector<uint8_t> got = Read(dilated, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    motion->Release();
    dilated->Release();

    size_t moved = 0, bad = 0;
    UINT firstX = 0, firstY = 0;
    float gotX = 0.0f, gotY = 0.0f, wantX = 0.0f, wantY = 0.0f;
    for (UINT y = 0; y < height; ++y)
    {
        for (UINT x = 0; x < width; ++x)
        {
            // Itself, then left, right, up, down, each clamped to the subrect; a tie keeps the one before.
            const UINT taps[5][2] = { { x, y },
                                      { x > 0 ? x - 1 : 0, y },
                                      { std::min(x + 1, width - 1), y },
                                      { x, y > 0 ? y - 1 : 0 },
                                      { x, std::min(y + 1, height - 1) } };
            unsigned best = 0;
            float bestDepth = DepthUnder(pattern, x, y, width, height, inverted);
            for (unsigned i = 1; i < 5; ++i)
            {
                const float depthHere = DepthUnder(pattern, taps[i][0], taps[i][1], width, height, inverted);
                if (CloserDepth(depthHere, bestDepth, inverted))
                {
                    best = i;
                    bestDepth = depthHere;
                }
            }
            moved += best != 0;
            const float ex = float(taps[best][0]) + 0.25f, ey = -(float(taps[best][1]) + 0.5f);
            float ox, oy;
            load(got.data() + (size_t(y) * width + x) * bytes, &ox, &oy);
            if ((ox != ex || oy != ey) && bad++ == 0)
                firstX = x, firstY = y, gotX = ox, gotY = oy, wantX = ex, wantY = ey;
        }
    }
    const char* formatName = wide ? "R32G32_FLOAT" : "R16G16_FLOAT";
    if (bad == 0)
        Check(moved > 0,
              "depth, %s: motion vectors dilated as the CPU (%s, %ux%u at %u,%u; %zu texels take a "
              "neighbour's)",
              what, formatName, width, height, baseX, baseY, moved);
    else
        Check(false,
              "depth, %s: motion vectors dilated (%s, %ux%u at %u,%u), %zu texels wrong; first (%u,%u) got %g %g, "
              "want %g %g",
              what, formatName, width, height, baseX, baseY, bad, firstX, firstY, gotX, gotY, wantX, wantY);
}

// Show sky: the composite stripes the sky, as bright as the white point, and leaves every other pixel as it was
// (DetailStrength 0, so the stripes are all it does). A frame of twice the guide's size, inside a larger texture.
void CheckShowSky(ID3D12Resource* depth, const DepthCase& d, bool inverted, const std::vector<DepthRect>& pattern,
                  const char* what)
{
    Scene s = MakeScene(kHalf, 2 * kGuideWidth, 2 * kGuideHeight, 4);
    Setup u;
    u.knobs.detail = 0.0f;
    Reset(s, u);
    NrConstants c = Constants(s, u, NR_MODE_COMPOSITE);
    Guide(&c, inverted);
    c.flags |= NR_FLAG_SHOW_SKY;
    const unsigned table = Table({ depth, d.view }, {}, {}, ExposureView(s, u), OutputView(s));
    Dispatch(g.nr, table, c, Groups(s.width, 8), Groups(s.height, 8));
    const std::vector<uint8_t> output = Read(s.output, s.format->bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    const float white = WhiteOf(u);
    size_t striped = 0, bad = 0;
    UINT firstX = 0, firstY = 0;
    Pixel got, want;
    for (UINT ty = 0; ty < s.textureHeight; ++ty)
    {
        for (UINT tx = 0; tx < s.textureWidth; ++tx)
        {
            const size_t i = size_t(ty) * s.textureWidth + tx;
            const bool inside = tx >= s.baseX && tx < s.baseX + s.width && ty >= s.baseY && ty < s.baseY + s.height;
            const UINT x = tx - s.baseX, y = ty - s.baseY;
            const bool stripe = inside && ((x + y) / 8) % 2 == 0 &&
                                SkyDepth(DepthUnder(pattern, x, y, s.width, s.height, inverted), inverted);
            const Pixel o = Decode(kHalf, output.data() + i * 8);
            bool ok;
            Pixel e = s.texture[i];
            if (stripe)
            {
                ++striped;
                e = Quantise(kHalf, { 0.95f * white, 0.3f * white, 0.95f * white, s.texture[i].a });
                ok = ClosePixel(kHalf, o, e) && o.a == e.a;
            }
            else
                ok = std::memcmp(output.data() + i * 8, s.bytes.data() + i * 8, 8) == 0;
            if (!ok && bad++ == 0)
                firstX = tx, firstY = ty, got = o, want = e;
        }
    }
    if (bad == 0)
        Check(striped > 0, "depth, %s: Show sky stripes %zu pixels of a %ux%u frame, the rest bit for bit as they were",
              what, striped, s.width, s.height);
    else
        Check(false, "depth, %s: Show sky, %zu pixels wrong; first (%u,%u) got %g %g %g %g, want %g %g %g %g", what,
              bad, firstX, firstY, got.r, got.g, got.b, got.a, want.r, want.g, want.b, want.a);
    Drop(s);
}

void TestDepth()
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 options = {};
    const bool said = SUCCEEDED(g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &options, sizeof options));
    Say("depth: this GPU %s a fully typed format in another format of its family",
        !said                                      ? "does not say whether it views"
        : options.CastingFullyTypedFormatSupported ? "views"
                                                   : "does not view");
    D3D12_DESCRIPTOR_HEAP_DESC heap = {};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    heap.NumDescriptors = 1;
    ID3D12DescriptorHeap* targets = nullptr;
    Must(g.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&targets)), "CreateDescriptorHeap(DSV)");
    const std::vector<DepthRect> pattern = DepthPattern();
    for (const DepthCase& d : kDepthCases)
    {
        for (const bool inverted : { false, true })
        {
            char what[96];
            std::snprintf(what, sizeof what, "%s%s", d.name, inverted ? ", inverted" : "");
            ID3D12Resource* depth = MakeDepth(d, inverted, pattern, targets);
            if (depth == nullptr)
            {
                Check(false, "depth, %s: this GPU will not make it a depth-stencil texture", what);
                continue;
            }
            CheckSkyMask(depth, d, inverted, pattern, what);
            // At the render resolution, as both games give them; at the display resolution, as others may.
            CheckDilate(depth, d, inverted, pattern, DXGI_FORMAT_R16G16_FLOAT, kGuideWidth, kGuideHeight, 3, 1, what);
            CheckDilate(depth, d, inverted, pattern, DXGI_FORMAT_R16G16_FLOAT, 2 * kGuideWidth, 2 * kGuideHeight, 1, 1,
                        what);
            if (&d == &kDepthCases[0])
                CheckDilate(depth, d, inverted, pattern, DXGI_FORMAT_R32G32_FLOAT, kGuideWidth, kGuideHeight, 2, 2,
                            what);
            CheckShowSky(depth, d, inverted, pattern, what);
            depth->Release();
        }
    }
    targets->Release();
}
// What the NR pass binds again (src/list_state.cpp): unknown until the list is reset, then whatever the game bound
// last, nothing after a Reset or ClearState, our own bindings undone by the restore, and each list its own.
bool SameState(const ListState& a, const ListState& b)
{
    if (a.known != b.known || a.heapCount != b.heapCount || a.heaps[0] != b.heaps[0] || a.heaps[1] != b.heaps[1] ||
        a.pipeline != b.pipeline || a.stateObject != b.stateObject || a.computeRoot != b.computeRoot ||
        a.constantCount != b.constantCount)
        return false;
    for (unsigned i = 0; i < kListStateArgs; ++i)
    {
        if (a.args[i].kind != b.args[i].kind || (a.args[i].kind != 0 && a.args[i].value != b.args[i].value))
            return false;
    }
    for (unsigned i = 0; i < a.constantCount; ++i)
    {
        if (a.constants[i].parameter != b.constants[i].parameter || a.constants[i].offset != b.constants[i].offset ||
            a.constants[i].value != b.constants[i].value)
            return false;
    }
    return true;
}

bool Empty(const ListState& s, ID3D12PipelineState* pipeline)
{
    ListState empty = {};
    empty.known = true;
    empty.pipeline = pipeline;
    return SameState(s, empty);
}

void TestListState()
{
    auto makeHeap = [](D3D12_DESCRIPTOR_HEAP_TYPE type) {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = type;
        desc.NumDescriptors = 4;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ID3D12DescriptorHeap* heap = nullptr;
        Must(g.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)), "CreateDescriptorHeap");
        return heap;
    };
    ID3D12DescriptorHeap* gameHeap = makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ID3D12DescriptorHeap* gameSampler = makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    ID3D12DescriptorHeap* ourHeap = makeHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    // The game's root signature: four constants, a CBV, a table; ours is shadertest's (as dxgi.dll's: two parameters).
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER parameters[3] = {};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = 4;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameters[1].Descriptor.ShaderRegister = 1; // b0 holds the constants
    parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[2].DescriptorTable.NumDescriptorRanges = 1;
    parameters[2].DescriptorTable.pDescriptorRanges = &range;
    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 3;
    desc.pParameters = parameters;
    ID3DBlob* blob = nullptr;
    ID3DBlob* errors = nullptr;
    Must(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors), "the game's root signature");
    ID3D12RootSignature* gameRoot = nullptr;
    Must(g.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&gameRoot)),
         "CreateRootSignature");
    blob->Release();
    if (errors != nullptr)
        errors->Release();

    ListState s = {};
    Check(!ListStateCapture(g.list, &s), "list state: a list first seen is unknown");
    g.list->SetDescriptorHeaps(1, &gameHeap);
    Check(!ListStateCapture(g.list, &s), "list state: and stays unknown until it is reset");
    Submit(); // Close, Reset with no pipeline
    Check(ListStateCapture(g.list, &s) && Empty(s, nullptr), "list state: nothing bound after a Reset");

    // The game binds its compute state.
    ID3D12DescriptorHeap* gameHeaps[] = { gameHeap, gameSampler };
    g.list->SetDescriptorHeaps(2, gameHeaps);
    g.list->SetComputeRootSignature(gameRoot);
    const uint32_t constants[] = { 7, 8 };
    g.list->SetComputeRoot32BitConstants(0, 2, constants, 1);
    g.list->SetComputeRoot32BitConstant(0, 9, 3);
    g.list->SetComputeRoot32BitConstant(0, 10, 1); // replaces the 7
    g.list->SetComputeRootConstantBufferView(1, 0x10000);
    g.list->SetComputeRootDescriptorTable(2, gameHeap->GetGPUDescriptorHandleForHeapStart());
    g.list->SetPipelineState(g.nr);
    ListState game = {};
    const bool known = ListStateCapture(g.list, &game);
    Check(known && game.heapCount == 2 && game.heaps[0] == gameHeap && game.heaps[1] == gameSampler &&
              game.computeRoot == gameRoot && game.pipeline == g.nr && game.args[1].value == 0x10000 &&
              game.args[2].value == gameHeap->GetGPUDescriptorHandleForHeapStart().ptr && game.constantCount == 3,
          "list state: the game's heaps, root signature, CBV, table, three constants and pipeline");
    bool constantsRight = game.constantCount == 3;
    for (unsigned i = 0; i < game.constantCount; ++i)
    {
        const ListStateConstant& c = game.constants[i];
        const uint32_t want = c.offset == 1 ? 10 : c.offset == 2 ? 8 : c.offset == 3 ? 9 : 0;
        constantsRight = constantsRight && c.parameter == 0 && c.value == want;
    }
    Check(constantsRight, "list state: constants 10, 8, 9 at offsets 1, 2, 3");

    // The NR pass binds its own, then the game's again.
    g.list->SetDescriptorHeaps(1, &ourHeap);
    g.list->SetComputeRootSignature(g.root);
    g.list->SetPipelineState(g.stats);
    Check(ListStateCapture(g.list, &s) && s.computeRoot == g.root && s.args[1].kind == 0 && s.args[2].kind == 0 &&
              s.constantCount == 0 && s.heapCount == 1 && s.heaps[0] == ourHeap && s.pipeline == g.stats,
          "list state: ours while the pass runs, and a new root signature leaves no argument bound");
    ListStateRestore(g.list, game);
    Check(ListStateCapture(g.list, &s) && SameState(s, game), "list state: the game's own again after the pass");

    g.list->ClearState(g.nr);
    Check(ListStateCapture(g.list, &s) && Empty(s, g.nr), "list state: nothing but ClearState's pipeline after it");
    Submit();
    Check(ListStateCapture(g.list, &s) && Empty(s, nullptr), "list state: nothing bound after the next Reset");

    ID3D12CommandAllocator* otherAllocator = nullptr;
    Must(g.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&otherAllocator)),
         "CreateCommandAllocator");
    ID3D12GraphicsCommandList* other = nullptr;
    Must(g.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, otherAllocator, nullptr, IID_PPV_ARGS(&other)),
         "CreateCommandList");
    Check(!ListStateCapture(other, &s), "list state: a second list starts unknown");
    Must(other->Close(), "Close");
    Must(other->Reset(otherAllocator, nullptr), "Reset");
    other->SetDescriptorHeaps(1, &ourHeap);
    Check(ListStateCapture(other, &s) && s.heapCount == 1 && s.heaps[0] == ourHeap && ListStateCapture(g.list, &s) &&
              Empty(s, nullptr),
          "list state: each list its own");
    other->Close();
    other->Release();
    otherAllocator->Release();
    gameRoot->Release();
    gameHeap->Release();
    gameSampler->Release();
    ourHeap->Release();
}
} // namespace

int main()
{
    Init();
    TestListState();
    D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
    g.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof options);
    Say("shadertest: nr_cso %zu bytes, nr_stats_cso %zu bytes, nr_fit_cso %zu bytes, typed UAV loads of the "
        "additional formats: %s",
        sizeof nr_cso, sizeof nr_stats_cso, sizeof nr_fit_cso, options.TypedUAVLoadAdditionalFormats ? "yes" : "no");
    TestLinear(kHalf, 4); // inside a larger texture, as a game with an output subrect
    TestLinear(kSmall, 0);
    TestDisplayEncoded(kUnorm);
    TestDepth(); // last: a view this GPU cannot read may take the device down with it
    Say("shadertest: %d checks, %d failed", g_checks, g_failed);
    if (g_failed == 0)
        std::puts("PASS");
    else
        std::printf("FAIL (%d)\n", g_failed);
    return g_failed;
}
