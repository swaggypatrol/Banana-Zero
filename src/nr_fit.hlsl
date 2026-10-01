// The fit, for when the model works on a smaller copy of the frame (ModelScale below 100%, NR_FLAG_SCALED): what the
// model changed, at its own size, as local linear functions of a guide, so that the composite can bring the change
// back to the frame's size along the frame's own edges rather than along the model's coarser texels. A fast guided
// filter (He and Sun, 2015): over each 3x3 window of model texels, the brightness change and the colour change are
// fitted as a x guide + b, the guide being the log2 luminance of the model's input (nr_common.hlsli, Guide); each fit
// is averaged with its neighbours' over 3x3 again; the composite (nr.hlsl) blends the fits of the four texels around
// a frame pixel bilinearly and applies them to that pixel's own guide, then holds the result within what the model
// did at those four texels.
//
// Chosen on 4K screenshots of both games with a stand-in for the model at 67%: on Witcher 3's trees against the sky
// it came within 36.3-36.9 dB of the change made at full size, where bilinear came within 33.9 and joint bilateral
// upsampling within 35.4; without the hold, a few pixels at hard edges went badly wrong. kFitEps and the 3x3 windows
// are what did best there.
//
// One group per 8x8 model texels: the inputs of the 12x12 texels around them and the fits of the 10x10 inside that go
// through group shared memory. Beyond the picture's edges the windows repeat its edge texels (the inputs, and the fits
// for the averaging).
//
// Precompiled: the build turns this file into nr_fit_shader.h (array nr_fit_cso).

#include "nr_common.hlsli"

RWTexture2D<float4> gSlope : register(u0); // per model texel, how its change follows the guide: gain, red, blue
RWTexture2D<float4> gValue : register(u1); // the change at the texel's own guide (gain, red, blue), and that guide
RWTexture2D<float4> gRaw : register(u2);   // what the model itself changed at the texel: gain, red, blue

// Where the guide varies less than this (in EV squared) across a window, the fit leans towards the window's mean
// change rather than following the guide: a seventh of a stop of local contrast.
static const float kFitEps = 0.02;

#define FIT_TILE 8
#define FIT_IN (FIT_TILE + 4) // the inputs: the tile and two texels around it
#define FIT_AB (FIT_TILE + 2) // the fits: the tile and one texel around it

groupshared float4 sIn[FIT_IN * FIT_IN]; // guide, gain, red, blue
groupshared float3 sA[FIT_AB * FIT_AB];  // the fits' slopes: gain, red, blue
groupshared float3 sB[FIT_AB * FIT_AB];  // and their intercepts

[numthreads(FIT_TILE, FIT_TILE, 1)]
void CSMain(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    const bool linearHdr = (g.flags & NR_FLAG_LINEAR_HDR) != 0;
    const int2 last = int2(g.modelWidth, g.modelHeight) - 1;
    const int2 origin = int2(group.xy) * FIT_TILE - 2; // the model texel sIn[0] holds

    // The inputs: each texel's guide and the model's change there, the edge texels repeated beyond the picture. Where
    // the model returned a texel exactly as it got it, the change is exactly 0, as in the full-size composite, and not
    // whatever rounding leaves of the formulas: the GPU need not round o / yo - p / yp to 0 for o equal to p.
    for (uint i = gi; i < FIT_IN * FIT_IN; i += FIT_TILE * FIT_TILE)
    {
        const int2 q = clamp(origin + int2(i % FIT_IN, i / FIT_IN), 0, last);
        const float3 pe = gProxy.Load(int3(q, 0)).rgb;
        const float3 me = gModel.Load(int3(q, 0)).rgb;
        const float3 p = ProxyLinear(pe);
        float4 input = float4(Guide(p), 0.0, 0.0, 0.0);
        if (any(me != pe))
        {
            const float3 o = ModelLinear(me, pe);
            const float3 chroma = ModelChroma(p, o);
            input.yzw = float3(ModelGain(p, o, linearHdr), chroma.r, chroma.b);
        }
        sIn[i] = input;
    }
    GroupMemoryBarrierWithGroupSync();

    // The fits over 3x3 windows, for the tile and one texel around it; a texel beyond the picture takes the fit of the
    // edge texel it repeats. The guide is taken relative to the window's centre, so that the variance of a nearly flat
    // window does not drown in the guide's own size.
    for (uint j = gi; j < FIT_AB * FIT_AB; j += FIT_TILE * FIT_TILE)
    {
        const int2 at = clamp(origin + 1 + int2(j % FIT_AB, j / FIT_AB), 0, last) - origin; // where it is in sIn
        const float centre = sIn[at.y * FIT_IN + at.x].x;
        float mean = 0.0, square = 0.0;
        float3 target = float3(0.0, 0.0, 0.0), product = float3(0.0, 0.0, 0.0);
        [unroll] for (int dy = -1; dy <= 1; ++dy)
        {
            [unroll] for (int dx = -1; dx <= 1; ++dx)
            {
                const float4 v = sIn[(at.y + dy) * FIT_IN + at.x + dx];
                const float d = v.x - centre;
                mean += d;
                square += d * d;
                target += v.yzw;
                product += d * v.yzw;
            }
        }
        mean /= 9.0;
        square /= 9.0;
        target /= 9.0;
        product /= 9.0;
        const float variance = max(square - mean * mean, 0.0);
        const float3 a = (product - mean * target) / (variance + kFitEps);
        sA[j] = a;
        sB[j] = target - a * (mean + centre);
    }
    GroupMemoryBarrierWithGroupSync();

    // Each texel's fit averaged with its neighbours', kept as its slope and the value it gives at the texel's own guide
    // (which the half floats hold more precisely than an intercept far from the guides in use), with the guide.
    const int2 q = int2(group.xy) * FIT_TILE + int2(thread.xy);
    if (any(q > last))
        return;
    float3 a = float3(0.0, 0.0, 0.0), b = float3(0.0, 0.0, 0.0);
    [unroll] for (int dy = -1; dy <= 1; ++dy)
    {
        [unroll] for (int dx = -1; dx <= 1; ++dx)
        {
            const uint k = (thread.y + 1 + dy) * FIT_AB + thread.x + 1 + dx;
            a += sA[k];
            b += sB[k];
        }
    }
    a /= 9.0;
    b /= 9.0;
    const float4 own = sIn[(thread.y + 2) * FIT_IN + thread.x + 2];
    gSlope[q] = float4(a, 0.0);
    gValue[q] = float4(b + a * own.x, own.x);
    gRaw[q] = float4(own.yzw, 0.0);
}
