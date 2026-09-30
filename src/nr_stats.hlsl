// Statistics of the Neural Rendering pass (StatsLog, and the menu's readouts): how the frame's brightness lies
// relative to the white point, how much of it the encode compresses, and how much the model changed. Accumulated in
// group shared memory, merged into a small buffer (u1) that nr_dx12.cpp copies into a readback ring; the counts and
// the layout are in nr_shared.h.
//
// Also the scene meter, which runs every frame before the encode: a histogram of the frame's
// luminance into its own buffer (u1 then), and a one-group pass that works the scene white point out of it into a
// 1x1 texture (u0) that the encode reads as its exposure texture. Nothing goes back to the CPU.
//
// Each thread looks at a 2x2 square of pixels, so a 16x16 group covers 32x32 and merges its histogram once.
//
// Precompiled: the build turns this file into nr_stats_shader.h (array nr_stats_cso).

#include "nr_common.hlsli"

RWTexture2D<float> gScene : register(u0); // meter resolve: the scene white point, stored as an exposure (1 / W)
RWByteAddressBuffer gStats : register(u1); // the statistics buffer, or for the meter its histogram

groupshared uint sHist[NR_HIST_BINS * 2];
groupshared uint sCount[NR_COUNTS];
groupshared uint sMeter[NR_METER_BINS];

// The scene white point: the frame's middle brightness, the mean log2 luminance of the middle half of its pixels (the
// 25th to the 75th percentile, which moves smoothly as a bright sky comes into view or leaves it, where a median
// would jump), is put kSceneKey below the white point. 0.2 (-2.32 EV) is where the middle half of Witcher 3's own
// finished pictures lies, measured on four in-game screenshots (-2.17 to -2.54 EV), and the
// middle of section 2's 2-3 EV. WhiteEV moves it further, as with the other sources.
static const float kSceneKey = 0.2;
static const float kSceneLow = 0.25;
static const float kSceneHigh = 0.75;
// How fast the white point follows the scene: a change is 63% followed after this many seconds, 95% after three
// times as long. The model's history sees a picture that brightens or darkens smoothly, not in steps.
static const float kSceneSeconds = 0.5;

uint HistBin(float value, float lowEv, float perEv, uint bins, inout uint below, inout uint above)
{
    const float at = floor((log2(value) - lowEv) * perEv);
    if (at < 0.0)
    {
        ++below;
        return 0xFFFFFFFF;
    }
    if (at >= float(bins))
    {
        ++above;
        return 0xFFFFFFFF;
    }
    return uint(at);
}

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
    if (g.mode == NR_STATS_METER)
    {
        for (uint i = gi; i < NR_METER_BINS; i += 256)
            sMeter[i] = 0;
        GroupMemoryBarrierWithGroupSync();
        [unroll] for (uint n = 0; n < 4; ++n)
        {
            const uint2 q = id.xy * 2 + uint2(n & 1, n >> 1);
            if (q.x >= g.width || q.y >= g.height)
                continue;
            const float3 c = gFrame.Load(int3(q + uint2(g.baseX, g.baseY), 0)).rgb;
            const float y = dot(Clean(c), kLuma);
            if (!all(isfinite(c)) || y <= 0.0)
                continue;
            const float at = clamp(floor((log2(y) - float(NR_METER_EV_MIN)) * float(NR_METER_PER_EV)), 0.0,
                                   float(NR_METER_BINS - 1));
            InterlockedAdd(sMeter[uint(at)], 1u);
        }
        GroupMemoryBarrierWithGroupSync();
        for (uint b = gi; b < NR_METER_BINS; b += 256)
        {
            if (sMeter[b] != 0)
                gStats.InterlockedAdd(b * 4, sMeter[b]);
        }
        return;
    }

    if (g.mode == NR_STATS_RESOLVE)
    {
        // One group: every thread fetches its share of the bins, one thread works the white point out, and the
        // buffer is emptied for the next frame once every bin has been fetched.
        for (uint i = gi; i < NR_METER_BINS; i += 256)
            sMeter[i] = gStats.Load(i * 4);
        GroupMemoryBarrierWithGroupSync();
        for (uint z = gi; z < NR_METER_BINS; z += 256)
            gStats.Store(z * 4, 0u);
        if (gi != 0)
            return;
        float total = 0.0;
        for (uint b = 0; b < NR_METER_BINS; ++b)
            total += float(sMeter[b]);
        const float old = gScene[uint2(0, 0)];
        const bool haveOld = old > 1.0e-30 && old < 1.0e30; // NaN fails both
        float exposure = haveOld ? old : 1.0;
        if (total >= 1.0)
        {
            // The mean of each bin's centre over the part of it that lies in the middle half.
            const float low = kSceneLow * total;
            const float high = kSceneHigh * total;
            float below = 0.0;
            float sum = 0.0;
            for (uint k = 0; k < NR_METER_BINS; ++k)
            {
                const float count = float(sMeter[k]);
                const float inside = min(below + count, high) - max(below, low);
                if (inside > 0.0)
                    sum += inside * (float(NR_METER_EV_MIN) + (float(k) + 0.5) / float(NR_METER_PER_EV));
                below += count;
            }
            const float target = log2(kSceneKey) - sum / (high - low); // log2 of the exposure: key over the middle
            float next = target;
            if (haveOld && (g.flags & NR_FLAG_SCENE_RESET) == 0)
                next = lerp(log2(old), target, 1.0 - exp(-max(g.seconds, 0.0) / kSceneSeconds));
            exposure = exp2(next);
        }
        gScene[uint2(0, 0)] = exposure;
        return;
    }

    if (g.mode == NR_STATS_CLEAR)
    {
        // One group: zeroes the words and stamps the frame number into both ends, which the CPU side compares to
        // tell a finished copy from one still being written.
        for (uint i = gi; i < NR_S_WORDS; i += 256)
            gStats.Store(i * 4, i == NR_S_TAG_FIRST || i == NR_S_TAG_LAST ? g.frame : 0u);
        return;
    }

    for (uint i = gi; i < NR_HIST_BINS * 2; i += 256)
        sHist[i] = 0;
    if (gi < NR_COUNTS)
        sCount[gi] = 0;
    GroupMemoryBarrierWithGroupSync();

    const bool linearHdr = (g.flags & NR_FLAG_LINEAR_HDR) != 0;
    uint count[NR_COUNTS];
    [unroll] for (uint k = 0; k < NR_COUNTS; ++k)
        count[k] = 0;
    uint largest = 0; // float bits of the largest finite channel

    if (g.mode == NR_STATS_INPUT)
    {
        const float white = linearHdr ? WhitePoint() : 1.0;
        const float s = g.shoulder;
        const float heavyAt = s + kHeavyT * (1.0 - s);
        [unroll] for (uint n = 0; n < 4; ++n)
        {
            const uint2 q = id.xy * 2 + uint2(n & 1, n >> 1);
            if (q.x >= g.width || q.y >= g.height)
                continue;
            ++count[NR_C_PIXELS];
            const float3 c = gFrame.Load(int3(q + uint2(g.baseX, g.baseY), 0)).rgb;
            if (!all(isfinite(c)))
            {
                ++count[NR_C_NONFINITE];
                continue;
            }
            if (any(c < 0.0))
                ++count[NR_C_NEGATIVE];
            const float3 x = Clean(c) / white;
            largest = max(largest, asuint(Max3(Clean(c))));
            const float m = Max3(x);
            if (m <= 0.0)
            {
                ++count[NR_C_BLACK];
                ++count[NR_C_DARK];
                continue;
            }
            const uint bin = HistBin(m, float(NR_HIST_EV_MIN), float(NR_HIST_PER_EV), NR_HIST_BINS, count[NR_C_BELOW],
                                     count[NR_C_ABOVE]);
            if (bin != 0xFFFFFFFF)
                InterlockedAdd(sHist[bin], 1u);
            const float y = dot(x, kLuma);
            if (y > 0.0)
            {
                uint unusedBelow = 0, unusedAbove = 0;
                const uint lumBin = HistBin(y, float(NR_HIST_EV_MIN), float(NR_HIST_PER_EV), NR_HIST_BINS, unusedBelow,
                                            unusedAbove);
                if (lumBin != 0xFFFFFFFF)
                    InterlockedAdd(sHist[NR_HIST_BINS + lumBin], 1u);
            }
            if (linearHdr && m > s)
                ++count[NR_C_SHOULDER];
            if (linearHdr && m > heavyAt)
                ++count[NR_C_HEAVY];
            // 0 once the proxy is stored in 8 bits: its sRGB value below half a step. Below the shoulder the proxy's
            // linear value is m itself, and sRGB is linear (x 12.92) down there.
            if (m < 0.5 / 255.0 / 12.92)
                ++count[NR_C_DARK];
        }
    }
    else if (g.mode == NR_STATS_GAIN)
    {
        [unroll] for (uint n = 0; n < 4; ++n)
        {
            const uint2 q = id.xy * 2 + uint2(n & 1, n >> 1);
            if (q.x >= g.width || q.y >= g.height)
                continue;
            ++count[NR_C_GAIN_PIXELS];
            const float3 pe = gProxy.Load(int3(q, 0)).rgb;
            const float3 p = ProxyLinear(pe);
            const float3 o = ModelLinear(gModel.Load(int3(q, 0)).rgb, pe);
            const float gain = ModelGain(p, o, linearHdr) * g.detail;
            if (gain < -g.maxGain)
                ++count[NR_C_GAIN_LOW];
            else if (gain > g.maxGain)
                ++count[NR_C_GAIN_HIGH];
            const float at = clamp(floor((gain - float(NR_GAIN_EV_MIN)) * float(NR_GAIN_PER_EV)), 0.0,
                                   float(NR_GAIN_BINS - 1));
            InterlockedAdd(sHist[uint(at)], 1u);
            const float3 chroma = ModelChroma(p, o);
            if (abs(chroma.r) + abs(chroma.g) + abs(chroma.b) > 0.05)
                ++count[NR_C_COLOUR];
        }
    }

    [unroll] for (uint w = 0; w < NR_COUNTS; ++w)
    {
        if (count[w] != 0)
            InterlockedAdd(sCount[w], count[w]);
    }
    if (largest != 0)
        InterlockedMax(sCount[NR_C_MAX_BITS], largest);
    GroupMemoryBarrierWithGroupSync();

    // Merge: every non-empty bin and count once per group.
    const uint histWords = g.mode == NR_STATS_INPUT ? NR_HIST_BINS * 2 : NR_GAIN_BINS;
    const uint histStart = g.mode == NR_STATS_INPUT ? NR_S_MAX_HIST : NR_S_GAIN_HIST;
    for (uint b = gi; b < histWords; b += 256)
    {
        if (sHist[b] != 0)
            gStats.InterlockedAdd((histStart + b) * 4, sHist[b]);
    }
    if (gi < NR_COUNTS && gi != NR_C_MAX_BITS && sCount[gi] != 0)
        gStats.InterlockedAdd((NR_S_COUNTS + gi) * 4, sCount[gi]);
    if (gi == NR_C_MAX_BITS && sCount[gi] != 0)
        gStats.InterlockedMax((NR_S_COUNTS + gi) * 4, sCount[gi]);

    // The values the counts are relative to, once per dispatch.
    if (g.mode == NR_STATS_INPUT && all(id.xy == 0))
    {
        const float exposure = (g.flags & NR_FLAG_EXPOSURE) != 0 ? gExposure.Load(int3(0, 0, 0)).x : 0.0;
        gStats.Store((NR_S_COUNTS + NR_C_EXPOSURE) * 4, asuint(exposure));
        gStats.Store((NR_S_COUNTS + NR_C_WHITE) * 4, asuint(linearHdr ? WhitePoint() : 1.0));
    }
}
