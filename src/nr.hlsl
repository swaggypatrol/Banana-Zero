// The Neural Rendering pass around the model: the encode (the game's Output -> the
// model's input), the composite (the model's output -> the game's Output, in place, plus the badge, the split
// screen's divider and the sky's stripes), the preview
// picture, the frame dump, and what the game's depth adds to the model's inputs (motion vectors dilated by it, the
// control mask that sets the sky apart). One shader; g.mode picks the pass. The statistics are in nr_stats.hlsl, the
// fit that brings a smaller model's change back to the frame's size (ModelScale) in nr_fit.hlsl, the formulas they
// share in nr_common.hlsli.
//
// Precompiled: the build turns this file into nr_shader.h (array nr_cso).

#include "nr_common.hlsli"

Texture2D<float4> gSlope : register(t4); // ModelScale: the fit's three textures (nr_fit.hlsl), the model's picture
                                         // at their top left
Texture2D<float4> gValue : register(t5);
Texture2D<float4> gRaw : register(t6);
RWTexture2D<float4> gTarget : register(u0); // encode: the proxy. composite: the game's Output. preview: the picture
RWByteAddressBuffer gDump : register(u1);   // the frame dump

// ---------------------------------------------------------------------------------------------------------------
// The calibration card: a test chart drawn from formulas, in units of the white point, so that it goes
// through the encode, the model and the composite like the scene around it. Three rows: a grey scale from 8 EV below
// the white point to 4 above, with a banana a third of an EV brighter than its cell in each cell above white; mid
// grey (0.18), white (1) and a bright dot (+6 EV) on black; red, green and blue at white and 2 EV above.

bool InCard(uint2 q) { return (g.flags & NR_FLAG_CARD) != 0 && q.x - g.cardX < g.cardW && q.y - g.cardY < g.cardH; }

float3 Card(uint2 q)
{
    const float2 p = float2(q - uint2(g.cardX, g.cardY)) + 0.5;
    const float w = float(g.cardW);
    const float h = float(g.cardH);
    const float gap = max(2.0, floor(w / 240.0));
    const float row1 = floor(h * 0.40);
    const float row2 = floor(h * 0.70);
    float x0, x1, y0, y1; // the cell p is in
    float3 value;
    if (p.y < row1)
    {
        const float cell = w / 13.0;
        const float i = min(floor(p.x / cell), 12.0);
        x0 = i * cell;
        x1 = x0 + cell;
        y0 = 0.0;
        y1 = row1;
        const float ev = i - 8.0;
        value = exp2(ev) * float3(1.0, 1.0, 1.0);
        if (ev >= 1.0)
        {
            // The banana: inside one circle and outside a smaller one set off towards the top right.
            const float2 centre = float2(x0 + x1, y0 + y1) * 0.5;
            const float r = 0.30 * min(cell, row1);
            const float2 d1 = p - centre;
            const float2 d2 = p - (centre + float2(0.30, -0.30) * r);
            if (dot(d1, d1) < r * r && dot(d2, d2) >= 0.64 * r * r)
                value *= exp2(1.0 / 3.0);
        }
    }
    else if (p.y < row2)
    {
        const float cell = w / 3.0;
        const float i = min(floor(p.x / cell), 2.0);
        x0 = i * cell;
        x1 = x0 + cell;
        y0 = row1;
        y1 = row2;
        if (i < 0.5)
            value = float3(0.18, 0.18, 0.18);
        else if (i < 1.5)
            value = float3(1.0, 1.0, 1.0);
        else
        {
            const float2 d = p - float2(x0 + x1, y0 + y1) * 0.5;
            const float r = 0.08 * h;
            value = (dot(d, d) < r * r ? 64.0 : 0.0) * float3(1.0, 1.0, 1.0);
        }
    }
    else
    {
        const float cell = w / 6.0;
        const float i = min(floor(p.x / cell), 5.0);
        x0 = i * cell;
        x1 = x0 + cell;
        y0 = row2;
        y1 = h;
        const float level = i < 2.5 ? 1.0 : 4.0;
        const float k = i < 2.5 ? i : i - 3.0;
        value = level * float3(k < 0.5 ? 1.0 : 0.0, k > 0.5 && k < 1.5 ? 1.0 : 0.0, k > 1.5 ? 1.0 : 0.0);
    }
    if (p.x < x0 + gap || p.x >= x1 - gap || p.y < y0 + gap || p.y >= y1 - gap)
        value = float3(0.0, 0.0, 0.0);
    return value;
}

// ---------------------------------------------------------------------------------------------------------------
// What the game's depth adds (DilateMotion, the sky sliders, Show sky). The depth comes through t0, read through a
// view of its depth plane alone (its first channel), the motion vectors through t1, each at its own subrect. Both are
// at the render resolution in the two tested games; the passes do not count on it.

// How close to the far end of the depth range a pixel must be to count as sky: where games draw the sky, or what they
// clear the depth to and leave there. Anything real is much nearer: with the near plane at 10 cm, 1e-6 is 100 km away.
static const float kSkyDepth = 1.0e-6;

bool DepthInverted() { return (g.flags & NR_FLAG_DEPTH_INVERTED) != 0; }

float GuideDepth(uint2 q) { return gFrame.Load(int3(q + uint2(g.depthBaseX, g.depthBaseY), 0)).x; }

bool IsSky(float d) { return DepthInverted() ? d <= kSkyDepth : d >= 1.0 - kSkyDepth; }

// Whether depth a is closer to the camera than depth b. Every comparison with a NaN is false: a NaN is never taken,
// and a texel whose own depth is NaN keeps its own motion.
bool Closer(float a, float b) { return DepthInverted() ? a > b : a < b; }

// The depth texel under texel p of a picture of `size` that shows the same view (the motion vectors, the frame): the
// one its centre falls in.
uint2 DepthTexel(uint2 p, uint2 size)
{
    const uint2 guide = uint2(g.guideWidth, g.guideHeight);
    return min((p * 2 + 1) * guide / (size * 2), guide - 1);
}

// ---------------------------------------------------------------------------------------------------------------
// The composite: the model's brightness change as a gain on the frame, its colour change added at the frame's
// luminance. `frame` is the game's value, in its own units; the result is in the same units. `change` is the
// brightness change in EV before DetailStrength and MaxGainEV (ModelGain), `chroma` the colour change (ModelChroma).

float3 ApplyChange(float3 frame, float change, float3 chroma, bool linearHdr)
{
    const float gain = clamp(change * g.detail, -g.maxGain, g.maxGain);
    const float3 c = linearHdr ? frame : SrgbDecode(saturate(frame));
    const float3 scaled = c * exp2(gain);
    float3 result = scaled + (g.detail * g.colour * dot(max(scaled, 0.0), kLuma)) * chroma;
    // A colour change may not push a channel below zero, or further below than the frame itself had it; nothing may
    // go above what the Output can store, unless the frame was already there.
    result = clamp(result, min(scaled, 0.0), max(c, kMaxOutput));
    return linearHdr ? result : SrgbEncode(saturate(result));
}

float3 Composite(float3 frame, float3 proxyEncoded, float3 modelEncoded, bool linearHdr)
{
    const float3 p = ProxyLinear(proxyEncoded);
    const float3 o = ModelLinear(modelEncoded, proxyEncoded);
    return ApplyChange(frame, ModelGain(p, o, linearHdr), ModelChroma(p, o), linearHdr);
}

// ---------------------------------------------------------------------------------------------------------------
// ModelScale (NR_FLAG_SCALED): the model works on a smaller copy of the frame, modelWidth x modelHeight at the top left
// of the proxy and of its output; each of its texels spans stepX x stepY frame pixels.

// The light the model's input holds for a frame value, before sRGB (what ProxyLinear gives back): the white point and
// the shoulder for linear HDR, the decoded value for a display-encoded frame. `frame` has the card in it already.
float3 ProxyLight(float3 frame, float white, bool linearHdr)
{
    return linearHdr ? min(EncodeLinear(frame, white, g.shoulder), 1.0) : SrgbDecode(saturate(Clean(frame)));
}

// The model's input at texel q: the mean of that light over the frame pixels the texel spans, each weighted by how
// much of it the texel covers, sRGB-encoded as the full-size encode stores it.
float3 EncodeScaled(uint2 q, bool linearHdr)
{
    const float2 step = float2(g.stepX, g.stepY);
    const float2 frameSize = float2(g.width, g.height);
    const float2 lo = float2(q) * step;
    const float2 hi = min(lo + step, frameSize);
    const uint2 first = uint2(lo);
    const uint2 end = min(uint2(ceil(hi)), uint2(g.width, g.height));
    const float white = linearHdr ? WhitePoint() : 1.0;
    float3 sum = float3(0.0, 0.0, 0.0);
    float weight = 0.0;
    [loop] for (uint y = first.y; y < end.y; ++y)
    {
        const float wy = min(hi.y, float(y) + 1.0) - max(lo.y, float(y));
        [loop] for (uint x = first.x; x < end.x; ++x)
        {
            const float w = wy * (min(hi.x, float(x) + 1.0) - max(lo.x, float(x)));
            if (w <= 0.0)
                continue;
            const uint2 pixel = uint2(x, y);
            float3 frame = gFrame.Load(int3(pixel + uint2(g.baseX, g.baseY), 0)).rgb;
            if (linearHdr && InCard(pixel))
                frame = Card(pixel) * white;
            sum += w * ProxyLight(frame, white, linearHdr);
            weight += w;
        }
    }
    return SrgbEncode(weight > 0.0 ? min(sum / weight, 1.0) : float3(0.0, 0.0, 0.0));
}

// The model texel frame pixel q falls in; q itself while the model works at the frame's size.
uint2 ModelTexel(uint2 q)
{
    if (!Scaled())
        return q;
    return min(uint2((float2(q) + 0.5) / float2(g.stepX, g.stepY)), uint2(g.modelWidth, g.modelHeight) - 1);
}

// The model's change at frame pixel id, whose own guide is `guide`: the fits (nr_fit.hlsl) of the four model texels
// around the pixel's centre, blended bilinearly and applied to that guide, then held within the least and the most the
// model itself changed at those four, so that no edge the frame has and the model's picture lacks can make more of a
// change than the model made. False when the model changed nothing at any of the four: the pixel stays as it was.
bool ScaledChange(uint2 id, float guide, out float change, out float3 chroma)
{
    const float2 last = float2(g.modelWidth, g.modelHeight) - 1.0;
    const float2 u = clamp((float2(id) + 0.5) / float2(g.stepX, g.stepY) - 0.5, 0.0, last);
    const uint2 q0 = uint2(u);
    const uint2 q1 = min(q0 + 1, uint2(last));
    const float2 f = u - float2(q0);
    const uint2 taps[4] = { q0, uint2(q1.x, q0.y), uint2(q0.x, q1.y), q1 };
    const float weights[4] = { (1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y };
    float3 sum = float3(0.0, 0.0, 0.0);
    float3 low = float3(1.0e30, 1.0e30, 1.0e30), high = float3(-1.0e30, -1.0e30, -1.0e30);
    [unroll] for (uint i = 0; i < 4; ++i)
    {
        const float3 slope = gSlope.Load(int3(taps[i], 0)).xyz;
        const float4 value = gValue.Load(int3(taps[i], 0));
        const float3 raw = gRaw.Load(int3(taps[i], 0)).xyz;
        sum += weights[i] * (slope * (guide - value.w) + value.xyz);
        low = min(low, raw);
        high = max(high, raw);
    }
    const float3 held = clamp(sum, low, high);
    change = held.x;
    chroma = ChromaFromRedBlue(held.y, held.z);
    return any(low != 0.0) || any(high != 0.0);
}

// ---------------------------------------------------------------------------------------------------------------
// The zebra classes of a proxy pixel: 1 when its largest channel went into the shoulder, 2 when it was
// compressed by more than 3 EV (the curve's slope below 1/8), 0 elsewhere and whenever the frame is not linear HDR.
// The curve is the identity below the shoulder and rises monotonically above it, so the proxy's own largest channel
// tells where the frame's was.

uint ZebraClass(float3 proxyEncoded, bool linearHdr)
{
    if (!linearHdr)
        return 0;
    const float m = Max3(SrgbDecode(saturate(proxyEncoded)));
    if (m > g.shoulder + kHeavyU * (1.0 - g.shoulder))
        return 2u;
    return m > g.shoulder ? 1u : 0u;
}

// ---------------------------------------------------------------------------------------------------------------
// The frame dump (nr_shared.h): four halves per pixel, two words.

uint2 Pack(float4 v) { return uint2(f32tof16(v.r) | (f32tof16(v.g) << 16), f32tof16(v.b) | (f32tof16(v.a) << 16)); }

uint2 ReducedSize() { return uint2(g.width, g.height) / NR_DUMP_SCALE; }
uint2 CropSize() { return min(uint2(NR_DUMP_CROP, NR_DUMP_CROP), uint2(g.width, g.height)); }
uint2 CropOrigin() { return (uint2(g.width, g.height) - CropSize()) / 2; }

// Where the closing word goes: after the header and the eight pictures.
uint DumpEnd()
{
    const uint2 reduced = ReducedSize();
    const uint2 crop = CropSize();
    return NR_DUMP_HEADER_BYTES + NR_DUMP_PICTURES * (reduced.x * reduced.y + crop.x * crop.y) * 8;
}

void DumpStore(uint picture, uint2 id, float4 value)
{
    const uint2 reduced = ReducedSize();
    const uint2 crop = CropSize();
    uint pixel;
    if (g.part == 0)
        pixel = picture * reduced.x * reduced.y + id.y * reduced.x + id.x;
    else
        pixel = NR_DUMP_PICTURES * reduced.x * reduced.y + picture * crop.x * crop.y + id.y * crop.x + id.x;
    gDump.Store2(NR_DUMP_HEADER_BYTES + pixel * 8, Pack(value));
}

// The value of `source` for dump pixel `id`: the mean of its NR_DUMP_SCALE square in the reduced picture, the pixel
// itself in the crop from the centre. `base` offsets into the game's Output.
float4 DumpRead(Texture2D<float4> source, uint2 id, uint2 base)
{
    if (g.part != 0)
        return source.Load(int3(id + CropOrigin() + base, 0));
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    [loop] for (uint y = 0; y < NR_DUMP_SCALE; ++y)
    {
        [loop] for (uint x = 0; x < NR_DUMP_SCALE; ++x)
            sum += source.Load(int3(id * NR_DUMP_SCALE + uint2(x, y) + base, 0));
    }
    return sum / float(NR_DUMP_SCALE * NR_DUMP_SCALE);
}

// The same for the model's output, which may be smaller than the frame (ModelScale): each frame pixel reads the
// model texel it falls in.
float4 DumpReadModel(uint2 id)
{
    if (g.part != 0)
        return gModel.Load(int3(ModelTexel(id + CropOrigin()), 0));
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    [loop] for (uint y = 0; y < NR_DUMP_SCALE; ++y)
    {
        [loop] for (uint x = 0; x < NR_DUMP_SCALE; ++x)
            sum += gModel.Load(int3(ModelTexel(id * NR_DUMP_SCALE + uint2(x, y)), 0));
    }
    return sum / float(NR_DUMP_SCALE * NR_DUMP_SCALE);
}

// The proxy for the dump, its alpha replaced by the zebra class (the largest in the square for the reduced picture),
// read like the model's output.
float4 DumpProxy(uint2 id, bool linearHdr)
{
    if (g.part != 0)
    {
        const float3 e = gProxy.Load(int3(ModelTexel(id + CropOrigin()), 0)).rgb;
        return float4(e, float(ZebraClass(e, linearHdr)));
    }
    float3 sum = float3(0.0, 0.0, 0.0);
    uint zebra = 0;
    [loop] for (uint y = 0; y < NR_DUMP_SCALE; ++y)
    {
        [loop] for (uint x = 0; x < NR_DUMP_SCALE; ++x)
        {
            const float3 e = gProxy.Load(int3(ModelTexel(id * NR_DUMP_SCALE + uint2(x, y)), 0)).rgb;
            sum += e;
            zebra = max(zebra, ZebraClass(e, linearHdr));
        }
    }
    return float4(sum / float(NR_DUMP_SCALE * NR_DUMP_SCALE), float(zebra));
}

// ---------------------------------------------------------------------------------------------------------------

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint2 base = uint2(g.baseX, g.baseY);
    const bool linearHdr = (g.flags & NR_FLAG_LINEAR_HDR) != 0;

    if (g.mode == NR_MODE_DILATE)
    {
        // Each motion vector texel takes the motion of the nearest, by depth, of itself and its four neighbours (as
        // nrprobe's T7 did): along the edge of something in front, the texels just outside it move with it, so the
        // display pixels its edge covers between two texels follow it rather than what lies behind.
        if (id.x >= g.outWidth || id.y >= g.outHeight)
            return;
        const uint2 size = uint2(g.outWidth, g.outHeight);
        const uint2 taps[4] = { uint2(id.x > 0 ? id.x - 1 : 0, id.y), uint2(min(id.x + 1, size.x - 1), id.y),
                                uint2(id.x, id.y > 0 ? id.y - 1 : 0), uint2(id.x, min(id.y + 1, size.y - 1)) };
        uint2 best = id.xy;
        float bestDepth = GuideDepth(DepthTexel(best, size));
        [unroll] for (uint i = 0; i < 4; ++i)
        {
            const float d = GuideDepth(DepthTexel(taps[i], size));
            if (Closer(d, bestDepth))
            {
                best = taps[i];
                bestDepth = d;
            }
        }
        gTarget[id.xy] = float4(gModel.Load(int3(best + uint2(g.motionBaseX, g.motionBaseY), 0)).xy, 0.0, 0.0);
        return;
    }
    if (g.mode == NR_MODE_SKY)
    {
        // The control mask, one texel per depth texel: .x times Intensity (the model's final blend), .y times
        // LocalTone, .z times LocalStructure; .w unused (the teardown, nrprobe's T8). The sky gets the sky sliders,
        // everything else 1, which is what no mask at all does.
        if (id.x >= g.outWidth || id.y >= g.outHeight)
            return;
        const bool sky = IsSky(GuideDepth(id.xy));
        gTarget[id.xy] = float4(1.0, sky ? g.skyTone : 1.0, sky ? g.skyStructure : 1.0, 1.0);
        return;
    }

    if (g.mode == NR_MODE_PREVIEW || g.mode == NR_MODE_DUMP_BEFORE || g.mode == NR_MODE_DUMP_AFTER)
    {
        if (id.x >= g.outWidth || id.y >= g.outHeight)
            return;
        if (g.mode == NR_MODE_DUMP_BEFORE)
        {
            DumpStore(0, id.xy, DumpRead(gFrame, id.xy, base));
            DumpStore(1, id.xy, DumpProxy(id.xy, linearHdr));
            DumpStore(2, id.xy, DumpReadModel(id.xy));
            if (g.part == 0 && all(id.xy == 0))
            {
                const float exposure = (g.flags & NR_FLAG_EXPOSURE) != 0 ? gExposure.Load(int3(0, 0, 0)).x : 0.0;
                gDump.Store3(NR_D_TAG * 4, uint3(g.frame, asuint(linearHdr ? WhitePoint() : 1.0), asuint(exposure)));
            }
            return;
        }
        if (g.mode == NR_MODE_DUMP_AFTER)
        {
            DumpStore(3, id.xy, DumpRead(gFrame, id.xy, base));
            if (g.part == 0 && all(id.xy == 0))
                gDump.Store(DumpEnd(), g.frame);
            return;
        }

        // The preview: the mean of a g.scale square of the proxy or the model's output, as the SDR
        // picture it is. The zebra stripes mark squares where any one pixel went into the shoulder (magenta) or was
        // compressed by more than 3 EV (red), so a single bright reflection is not averaged away.
        const bool showOutput = (g.flags & NR_FLAG_PREVIEW_OUTPUT) != 0;
        const bool zebra = (g.flags & NR_FLAG_ZEBRA) != 0 && !showOutput && linearHdr;
        float3 sum = float3(0.0, 0.0, 0.0);
        uint marked = 0;
        [loop] for (uint y = 0; y < g.scale; ++y)
        {
            [loop] for (uint x = 0; x < g.scale; ++x)
            {
                const uint2 q = ModelTexel(min(id.xy * g.scale + uint2(x, y), uint2(g.width - 1, g.height - 1)));
                const float3 e = saturate(showOutput ? gModel.Load(int3(q, 0)).rgb : gProxy.Load(int3(q, 0)).rgb);
                sum += e;
                if (zebra)
                    marked = max(marked, ZebraClass(e, true));
            }
        }
        float3 colour = sum / float(g.scale * g.scale);
        if (marked != 0 && ((id.x + id.y) / 4) % 2 == 0)
            colour = marked == 2 ? float3(1.0, 0.15, 0.15) : float3(0.95, 0.3, 0.95);
        gTarget[id.xy] = float4(colour, 1.0);
        return;
    }

    if (g.mode == NR_MODE_ENCODE && Scaled())
    {
        // The dispatch covers the model's picture.
        if (id.x >= g.modelWidth || id.y >= g.modelHeight)
            return;
        gTarget[id.xy] = float4(EncodeScaled(id.xy, linearHdr), 1.0);
        return;
    }

    if (id.x >= g.width || id.y >= g.height)
        return;
    const uint2 pos = id.xy + base;

    if (g.mode == NR_MODE_ENCODE)
    {
        const float4 frame = gFrame.Load(int3(pos, 0));
        float3 proxy;
        if (linearHdr)
        {
            const float white = WhitePoint();
            const float3 c = InCard(id.xy) ? Card(id.xy) * white : frame.rgb;
            proxy = SrgbEncode(min(EncodeLinear(c, white, g.shoulder), 1.0));
        }
        else
            proxy = saturate(Clean(frame.rgb)); // display-encoded already: the model gets it as it is
        gTarget[id.xy] = float4(proxy, 1.0);
        return;
    }

    // The composite, in place: this pixel of the game's Output is read, then written, by this thread alone (a typed
    // UAV load; nr_dx12.cpp checks the format supports it).
    float4 frame = gTarget[pos];
    const float white = linearHdr ? WhitePoint() : 1.0;
    bool write = false;
    if (InCard(id.xy))
    {
        frame.rgb = Card(id.xy) * white;
        write = true;
    }
    float4 result = frame;
    // The split screen: left of the divider the frame stays as DLSS made it (the card still shows, it is
    // part of the frozen picture); the divider itself is two pixels, one dark, one white, so it shows on anything.
    const bool left = g.splitX != 0 && id.x < g.splitX;
    if (g.splitX != 0 && id.x + 2 >= g.splitX && id.x < g.splitX + 2)
    {
        result.rgb = id.x < g.splitX ? float3(0.0, 0.0, 0.0) : float3(white, white, white);
        gTarget[pos] = result;
        return;
    }
    // DetailStrength 0 leaves the pixel exactly as DLSS made it (it is not even written), and so do a model output
    // identical to its input (around the pixel, with ModelScale) and a frame value that is not a number already.
    if (!left && g.detail != 0.0 && all(isfinite(frame.rgb)))
    {
        if (Scaled())
        {
            float change;
            float3 chroma;
            if (ScaledChange(id.xy, Guide(ProxyLight(frame.rgb, white, linearHdr)), change, chroma))
            {
                result.rgb = ApplyChange(frame.rgb, change, chroma, linearHdr);
                write = true;
            }
        }
        else
        {
            const float3 proxyEncoded = gProxy.Load(int3(id.xy, 0)).rgb;
            const float3 modelEncoded = gModel.Load(int3(id.xy, 0)).rgb;
            if (any(modelEncoded != proxyEncoded))
            {
                result.rgb = Composite(frame.rgb, proxyEncoded, modelEncoded, linearHdr);
                write = true;
            }
        }
    }

    // Show sky: purple stripes over what counts as sky, on both sides of the split screen, as bright as the white point.
    if ((g.flags & NR_FLAG_SHOW_SKY) != 0 && ((id.x + id.y) / 8) % 2 == 0 && !InCard(id.xy))
    {
        if (IsSky(GuideDepth(DepthTexel(id.xy, uint2(g.width, g.height)))))
        {
            result.rgb = float3(0.95, 0.3, 0.95) * white;
            write = true;
        }
    }

    // The badge: a green square in the bottom-right corner for a few seconds after NR starts, so that
    // NR being on is visible at a glance. Inset by half its side from both edges; as bright as the white point.
    if (g.badgeSize != 0)
    {
        const uint inset = g.badgeSize / 2;
        const uint2 corner = uint2(g.width - inset - g.badgeSize, g.height - inset - g.badgeSize);
        if (id.x >= corner.x && id.x < corner.x + g.badgeSize && id.y >= corner.y && id.y < corner.y + g.badgeSize)
        {
            result.rgb = float3(0.0, white, 0.0);
            write = true;
        }
    }
    if (write)
        gTarget[pos] = result;
}
