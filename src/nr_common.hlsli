// What nr.hlsl and nr_stats.hlsl both compute: the white point, the encode's shoulder and its inverse, sRGB, and the
// brightness change the composite applies. One copy, so that the statistics measure exactly what the passes do.

#include "nr_shared.h"

ConstantBuffer<NrConstants> g : register(b0);
Texture2D<float4> gFrame : register(t0);    // the game's Output (see nr_shared.h)
Texture2D<float4> gModel : register(t1);    // the model's output
Texture2D<float4> gProxy : register(t2);    // the model's input
Texture2D<float4> gExposure : register(t3); // the game's exposure texture, 1x1

static const float3 kLuma = float3(0.2126, 0.7152, 0.0722); // luminance of linear Rec. 709 RGB

// Added to both luminances before the model's change is taken as a ratio: in the near-black the model's changes are
// small in absolute terms but huge as ratios, and would otherwise all hit MaxGainEV. 1/4096 of the white point is
// below one 8-bit step of the sRGB-encoded proxy.
static const float kGainFloor = 1.0 / 4096.0;
// Below this luminance (proxy units) the model's colour change is ignored: its hue there is noise.
static const float kChromaFloor = 1.0 / 16384.0;
// How close to 1 the inverse shoulder is taken: 1 itself is infinitely far above the white point.
static const float kMaxU = 1.0 - 1.0 / 4096.0;
// The largest value the composite writes where the frame had less: the top of R11G11B10_FLOAT, just under the top of
// R16G16B16A16_FLOAT, so that no gain turns a finite pixel into infinity when the Output stores it.
static const float kMaxOutput = 65024.0;
// The curve's slope is below 1/8 (more than 3 EV of compression) above s + kHeavyT (1 - s): (1 + t)^2 > 8.
static const float kHeavyT = 1.8284271;
// The same point on the output side of the curve: s + kHeavyU (1 - s), where u = t / (1 + t).
static const float kHeavyU = 0.64644661;

float Max3(float3 v) { return max(v.r, max(v.g, v.b)); }

// A channel the model can be given: NaN is 0, below zero is 0, infinity is a very large number.
float Clean1(float v) { return isnan(v) ? 0.0 : clamp(v, 0.0, 1.0e30); }
float3 Clean(float3 c) { return float3(Clean1(c.r), Clean1(c.g), Clean1(c.b)); }

float SrgbEncode1(float v) { return v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055; }
float3 SrgbEncode(float3 v) { return float3(SrgbEncode1(v.r), SrgbEncode1(v.g), SrgbEncode1(v.b)); }
float SrgbDecode1(float e) { return e <= 0.04045 ? e / 12.92 : pow((e + 0.055) / 1.055, 2.4); }
float3 SrgbDecode(float3 e) { return float3(SrgbDecode1(e.r), SrgbDecode1(e.g), SrgbDecode1(e.b)); }

// The white point in the frame's own units: what the encode maps to 1 before the shoulder. g.whiteScale carries the
// pre-exposure (when the white point comes from the game) and 2^WhiteEV; the exposure texture, when the game gives
// one, divides it here on the GPU, so its value never has to come back to the CPU.
float WhitePoint()
{
    float white = g.whiteScale;
    if ((g.flags & NR_FLAG_EXPOSURE) != 0)
    {
        const float exposure = gExposure.Load(int3(0, 0, 0)).x;
        if (exposure > 1.0e-20 && exposure < 1.0e20) // NaN fails both
            white /= exposure;
    }
    return white > 1.0e-20 && white < 1.0e20 ? white : 1.0;
}

// The encode's curve on m, the largest channel of frame / white point: m itself up to s (Shoulder), then
// s + (1 - s) t / (1 + t) with t = (m - s) / (1 - s). Continuous with slope 1 at s, approaching 1 as m grows, and
// with a closed-form inverse. Applied to the largest channel and then to all three alike, so the hue stays.
float Shoulder(float m, float s)
{
    if (m <= s)
        return m;
    const float t = (m - s) / (1.0 - s);
    return s + (1.0 - s) * (t / (1.0 + t));
}

float ShoulderInverse(float y, float s)
{
    if (y <= s)
        return y;
    const float u = min((y - s) / (1.0 - s), kMaxU);
    return s + (1.0 - s) * (u / (1.0 - u));
}

// What undoing the shoulder multiplies a proxy pixel by, given its largest channel y: 1 below the shoulder.
float InverseScale(float y, float s) { return y > s ? ShoulderInverse(y, s) / max(y, 1.0e-20) : 1.0; }

// The same, held where the curve compresses by 3 EV (kHeavyU): HighlightRestore undoes the shoulder only up to
// there. Above it the inverse is so steep that one 8-bit step of the model's output is worth several EV (8 W encodes
// to 254, and 255 stands for over 1000 W), so a model that clips a bright sky to white would come back as MaxGainEV
// of extra light, and one that dims it a step as MaxGainEV less (the M4 playtest).
float RestoreScale(float y, float s) { return InverseScale(min(y, s + kHeavyU * (1.0 - s)), s); }

// The frame as the model's input (linear, before sRGB): white point, then the shoulder on the largest channel.
float3 EncodeLinear(float3 frame, float white, float s)
{
    const float3 x = Clean(frame) / white;
    const float m = Max3(x);
    return m > s ? x * (Shoulder(m, s) / m) : x;
}

// The model's input and output as linear light in the proxy's units. A NaN from the model counts as no change.
float3 ProxyLinear(float3 encoded) { return SrgbDecode(saturate(encoded)); }
float3 ModelLinear(float3 encoded, float3 proxyEncoded)
{
    const float3 e = float3(isnan(encoded.r) ? proxyEncoded.r : encoded.r, isnan(encoded.g) ? proxyEncoded.g : encoded.g,
                            isnan(encoded.b) ? proxyEncoded.b : encoded.b);
    return SrgbDecode(clamp(e, 0.0, 2.0));
}

// The brightness change the composite applies, in EV, before DetailStrength and MaxGainEV: the model's luminance
// over the proxy's, plus, above the shoulder, HighlightRestore's share of what undoing the curve (up to 3 EV of
// compression) adds to it. Exactly 0 when the model returned its input.
float ModelGain(float3 p, float3 o, bool linearHdr)
{
    const float yp = dot(p, kLuma);
    const float yo = dot(o, kLuma);
    float gain = yo == yp ? 0.0 : log2((yo + kGainFloor) / (yp + kGainFloor));
    if (linearHdr && g.highlight != 0.0)
    {
        const float mp = Max3(p);
        const float mo = Max3(o);
        if (mp > g.shoulder || mo > g.shoulder)
            gain += g.highlight * (log2(RestoreScale(mo, g.shoulder)) - log2(RestoreScale(mp, g.shoulder)));
    }
    return gain;
}

// The model's colour change: its chromaticity (colour over luminance) minus the proxy's. 0 in the near-black.
float3 ModelChroma(float3 p, float3 o)
{
    const float yp = dot(p, kLuma);
    const float yo = dot(o, kLuma);
    return yp > kChromaFloor && yo > kChromaFloor ? o / yo - p / yp : float3(0.0, 0.0, 0.0);
}
