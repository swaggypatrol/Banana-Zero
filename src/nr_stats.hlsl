// Statistics of the Neural Rendering pass (StatsLog, and the menu's readouts): how the frame's brightness lies
// relative to the white point, how much of it the encode compresses, and how much the model changed. Accumulated in
// group shared memory, merged into a small buffer (u1) that nr_dx12.cpp copies into a readback ring; the counts and
// the layout are in nr_shared.h.
//
// Also the scene meter, which runs before the encode on the frames whose white point is the scene's: a histogram of
// the frame's luminance into its own buffer (u1 then), and a one-group pass that works the scene white point out of
// it into a 1x1 texture (u0) that the encode reads as its exposure texture. Nothing goes back to the CPU.
//
// Each thread of the statistics looks at a 2x2 square of pixels, so a 16x16 group covers 32x32 and merges its
// histogram once; each thread of the meter looks at one pixel of a NR_METER_STEP square (nr_shared.h).
//
// Precompiled: the build turns this file into nr_stats_shader.h (array nr_stats_cso).

#include "nr_common.hlsli"

RWTexture2D<float> gScene : register(u0); // meter resolve: the scene white point, stored as an exposure (1 / W)
RWByteAddressBuffer gStats : register(u1); // the statistics buffer, or for the meter its histogram

groupshared uint sHist[NR_HIST_BINS * 2];
groupshared uint sCount[NR_COUNTS];
groupshared uint sMeter[NR_METER_BINS];
// The frame-wide sums of each wave of the group, in floats, and how many waves have put theirs in: the group adds them
// up and rounds once into the buffer's fixed point (AddToGroup). 64 waves at most: 256 threads in waves of 4.
static const uint kMaxWaves = 64;
groupshared float sWave[NR_GLOBALS * kMaxWaves];
groupshared uint sWaveKey[kMaxWaves]; // each slot's wave by its first thread's index in the group
groupshared uint sWaves;

// Into the group's frame-wide sums: across the wave in floats, into the wave's own slot. The merge after the group's
// barrier adds the slots up, still in floats, and rounds once per group: rounding per wave, in v0.3.1, made the fit's
// sums of a frame of little grey mostly rounding. Every thread of the group calls it.
void AddToGroup(float global[NR_GLOBALS], uint gi)
{
    uint slot = 0;
    if (WaveIsFirstLane())
        InterlockedAdd(sWaves, 1u, slot);
    slot = WaveReadLaneFirst(slot);
    const uint key = WaveReadLaneFirst(gi);
    if (WaveIsFirstLane())
        sWaveKey[slot] = key;
    [unroll] for (uint k = 0; k < NR_GLOBALS; ++k)
    {
        const float sum = WaveActiveSum(global[k]);
        if (WaveIsFirstLane())
            sWave[k * kMaxWaves + slot] = sum;
    }
}

// A pixel's share of the white balance's first sums (nr.hlsl's CastTint): its greyness, and that times its tint and its
// own chroma. The chroma of a pixel greyness weighs is within its own +-log2 of (smallest / largest), so weight times
// chroma stays under 1 (the spread pass sums its squares and products). The cast pass sums these alone, the gain
// pass beside its own, in the same order, so the two give the same words.
void AddGrey(inout float global[NR_GLOBALS], float3 p, float3 o)
{
    const float grey = Greyness(p, o);
    if (grey > 0.0)
    {
        const float2 tint = Tint(p, o);
        const float2 c = GreyChroma(p);
        global[NR_G_GREY] += grey;
        global[NR_G_TINT_R] += grey * tint.x;
        global[NR_G_TINT_B] += grey * tint.y;
        global[NR_G_CHROMA_R] += grey * c.x;
        global[NR_G_CHROMA_B] += grey * c.y;
    }
}

// The scene white point: the frame's middle brightness, the mean log2 luminance of the middle half of its pixels (the
// 25th to the 75th percentile, which moves smoothly as a bright sky comes into view or leaves it, where a median
// would jump), is put kSceneKey below the white point. 0.2 (-2.32 EV) is where the middle half of Witcher 3's own
// finished pictures lies, measured on four in-game screenshots (-2.17 to -2.54 EV), and the
// middle of section 2's 2-3 EV. WhiteEV moves it further, as with the other sources.
static const float kSceneKey = 0.2;
static const float kSceneLow = 0.25;
static const float kSceneHigh = 0.75;
// How the white point follows the scene (Follow): the first kSceneSlackEv of any difference is not followed at all,
// and the rest at one of two speeds, 63% of it after this many seconds, 95% after three times as long. A scene that
// got brighter is followed fast enough to spare its highlights the shoulder; one that got darker slowly, so a scene
// that keeps changing (a road through trees, the sky in and out of view) holds the white point near its brightest
// rather than swinging it both ways, and the model's history sees a picture that changes seldom and smoothly. With
// one speed of 0.5 s and no slack, Forza Horizon 6's white point moved by up to 3 EV within two seconds of driving,
// and the picture breathed.
static const float kSceneBrighterSeconds = 1.0;
static const float kSceneDarkerSeconds = 4.0;
static const float kSceneSlackEv = 0.25;

// The next log2 exposure from the last and this frame's target, `seconds` later. The slack is a dead band that moves
// with the value, so it needs no memory: inside it nothing moves, and a new level is settled kSceneSlackEv short of.
float Follow(float last, float target, float seconds)
{
    const float difference = target - last;
    const float beyond = difference - clamp(difference, -kSceneSlackEv, kSceneSlackEv);
    const float time = beyond < 0.0 ? kSceneBrighterSeconds : kSceneDarkerSeconds; // a lower exposure: brighter
    return last + beyond * (1.0 - exp(-max(seconds, 0.0) / time));
}

// The pixel the meter takes of square `cell` (NR_METER_STEP a side): a different one in each square, the same every
// frame. With the same one in every square, a pattern in the frame that repeats in step with the squares would be
// metered by only a part of itself (stripes three rows high in six colours came out 0.1 EV off).
uint2 MeterPixel(uint2 cell)
{
    uint h = cell.x * 0x9E3779B1u ^ cell.y * 0x85EBCA6Bu;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    return cell * NR_METER_STEP + uint2(h % NR_METER_STEP, (h >> 8) % NR_METER_STEP);
}

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
        const uint2 q = MeterPixel(id.xy);
        if (q.x < g.width && q.y < g.height)
        {
            const float3 c = gFrame.Load(int3(q + uint2(g.baseX, g.baseY), 0)).rgb;
            const float y = dot(Clean(c), kLuma);
            if (all(isfinite(c)) && y > 0.0)
            {
                const float at = clamp(floor((log2(y) - float(NR_METER_EV_MIN)) * float(NR_METER_PER_EV)), 0.0,
                                       float(NR_METER_BINS - 1));
                InterlockedAdd(sMeter[uint(at)], 1u);
            }
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
            // Held inside the slack: the stored value itself, not exp2 of its log2, so it does not creep frame by frame.
            if (!haveOld || (g.flags & NR_FLAG_SCENE_RESET) != 0)
                exposure = exp2(target);
            else
            {
                const float last = log2(old);
                const float next = Follow(last, target, g.seconds);
                exposure = next == last ? old : exp2(next);
            }
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
    if (gi == 0)
        sWaves = 0;
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
    else if (g.mode == NR_STATS_CAST)
    {
        // The white balance's first sums alone (AddGrey), for the composite of a frame without statistics; a frame
        // with them reads the gain pass's, which are the same.
        float global[NR_GLOBALS];
        [unroll] for (uint z = 0; z < NR_GLOBALS; ++z)
            global[z] = 0.0;
        [unroll] for (uint n = 0; n < 4; ++n)
        {
            const uint2 q = id.xy * 2 + uint2(n & 1, n >> 1);
            if (q.x >= g.width || q.y >= g.height)
                continue;
            const float3 pe = gProxy.Load(int3(q, 0)).rgb;
            AddGrey(global, ProxyLinear(pe), ModelLinear(gModel.Load(int3(q, 0)).rgb, pe));
        }
        AddToGroup(global, gi);
    }
    else if (g.mode == NR_STATS_GAIN)
    {
        float global[NR_GLOBALS];
        [unroll] for (uint z = 0; z < NR_GLOBALS; ++z)
            global[z] = 0.0;
        [unroll] for (uint n = 0; n < 4; ++n)
        {
            const uint2 q = id.xy * 2 + uint2(n & 1, n >> 1);
            if (q.x >= g.width || q.y >= g.height)
                continue;
            ++count[NR_C_GAIN_PIXELS];
            const float3 pe = gProxy.Load(int3(q, 0)).rgb;
            const float3 p = ProxyLinear(pe);
            const float3 o = ModelLinear(gModel.Load(int3(q, 0)).rgb, pe);
            const float change = ModelGain(p, o);
            const float gain = change * g.detail;
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
            // The brightness by the proxy's luminance, the white balance by greyness; each weight at most 1 and each
            // value held to +-2, so that no sum can leave an int32 at 4K (the spread pass's are bounded by these).
            const float weight = dot(p, kLuma);
            global[NR_G_WEIGHT] += weight;
            global[NR_G_GAIN] += weight * clamp(change, -2.0, 2.0);
            AddGrey(global, p, o);
            global[NR_G_IN_R] += p.r;
            global[NR_G_IN_G] += p.g;
            global[NR_G_IN_B] += p.b;
        }
        AddToGroup(global, gi);
    }
    else if (g.mode == NR_STATS_SPREAD)
    {
        // The fit's second sums (nr.hlsl's CastTint): the greyish pixels' chroma and tint each less its mean, the mean
        // being the sums the gain or cast pass left in this buffer over their greyness, worked out as CastTint does.
        // Around 0 instead, as v0.3.1 summed them, a frame of one saturated colour gave chroma squares of about 10 whose
        // rounding at every wave added up beyond the ridge, and the fit took several EV of white balance off it.
        // No sum of these, whole or in part, can leave an int32 at 4K: weight x (chroma - mean)^2 summed over any pixels is
        // at most weight x chroma^2 summed over all (under 0.3 a pixel), weight x (tint - mean)^2 likewise under 4, and a
        // product of the two under half their sum.
        float global[NR_GLOBALS];
        [unroll] for (uint z = 0; z < NR_GLOBALS; ++z)
            global[z] = 0.0;
        const float total = float(asint(gStats.Load((NR_S_GLOBAL + NR_G_GREY) * 4)));
        if (total > 0.0)
        {
            const float2 meanC = float2(float(asint(gStats.Load((NR_S_GLOBAL + NR_G_CHROMA_R) * 4))),
                                        float(asint(gStats.Load((NR_S_GLOBAL + NR_G_CHROMA_B) * 4)))) / total;
            const float2 meanT = float2(float(asint(gStats.Load((NR_S_GLOBAL + NR_G_TINT_R) * 4))),
                                        float(asint(gStats.Load((NR_S_GLOBAL + NR_G_TINT_B) * 4)))) / total;
            [unroll] for (uint n = 0; n < 4; ++n)
            {
                const uint2 q = id.xy * 2 + uint2(n & 1, n >> 1);
                if (q.x >= g.width || q.y >= g.height)
                    continue;
                const float3 pe = gProxy.Load(int3(q, 0)).rgb;
                const float3 p = ProxyLinear(pe);
                const float3 o = ModelLinear(gModel.Load(int3(q, 0)).rgb, pe);
                const float grey = Greyness(p, o);
                if (grey > 0.0)
                {
                    const float2 c = GreyChroma(p) - meanC;
                    const float2 t = Tint(p, o) - meanT;
                    global[NR_G_RR] += grey * c.x * c.x;
                    global[NR_G_RB] += grey * c.x * c.y;
                    global[NR_G_BB] += grey * c.y * c.y;
                    global[NR_G_R_TINT_R] += grey * c.x * t.x;
                    global[NR_G_B_TINT_R] += grey * c.y * t.x;
                    global[NR_G_R_TINT_B] += grey * c.x * t.y;
                    global[NR_G_B_TINT_B] += grey * c.y * t.y;
                }
            }
        }
        AddToGroup(global, gi);
    }

    [unroll] for (uint w = 0; w < NR_COUNTS; ++w)
    {
        if (count[w] != 0)
            InterlockedAdd(sCount[w], count[w]);
    }
    if (largest != 0)
        InterlockedMax(sCount[NR_C_MAX_BITS], largest);
    GroupMemoryBarrierWithGroupSync();

    // Merge: every non-empty bin and count once per group (the cast and spread passes: their sums alone).
    if (g.mode != NR_STATS_CAST && g.mode != NR_STATS_SPREAD)
    {
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
    }
    if (g.mode != NR_STATS_INPUT && gi < NR_GLOBALS)
    {
        // The slots in the order of their waves' first threads, not of the waves' arrival, so that the float sum, and
        // its rounding, is the same every run (in arrival order a frame's white balance changed by a step of the
        // fixed point now and then, and the picture by an fp16 step).
        float sum = 0.0;
        int previous = -1;
        for (uint r = 0; r < sWaves; ++r)
        {
            uint next = 0;
            uint smallest = 0xFFFFFFFF;
            for (uint w = 0; w < sWaves; ++w)
            {
                if (int(sWaveKey[w]) > previous && sWaveKey[w] < smallest)
                {
                    smallest = sWaveKey[w];
                    next = w;
                }
            }
            sum += sWave[gi * kMaxWaves + next];
            previous = int(smallest);
        }
        const int fixed = int(round(sum * NR_G_SCALE));
        if (fixed != 0)
            gStats.InterlockedAdd((NR_S_GLOBAL + gi) * 4, uint(fixed));
    }

    // The values the counts are relative to, once per dispatch.
    if (g.mode == NR_STATS_INPUT && all(id.xy == 0))
    {
        const float exposure = (g.flags & NR_FLAG_EXPOSURE) != 0 ? gExposure.Load(int3(0, 0, 0)).x : 0.0;
        gStats.Store((NR_S_COUNTS + NR_C_EXPOSURE) * 4, asuint(exposure));
        gStats.Store((NR_S_COUNTS + NR_C_WHITE) * 4, asuint(linearHdr ? WhitePoint() : 1.0));
    }
}
