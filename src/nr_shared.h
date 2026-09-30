#pragma once

// What the Neural Rendering shaders (nr.hlsl, nr_stats.hlsl) and the C++ side (nr_dx12.cpp) share, defined once so
// the two cannot drift: HLSL includes this file from beside it, C++ includes it directly. The constants go to the
// shaders as root constants (SetComputeRoot32BitConstants), so there is no constant buffer and no ring of them.
//
// Every dispatch sees the same descriptor table: t0-t3, then u0-u1. Which pass uses which slot:
//
//   pass              t0 frame   t1 model out  t2 proxy   t3 exposure   u0             u1 buffer
//   meter             read       -             -          -             -              meter
//   meter resolve     -          -             -          -             scene exposure meter
//   encode            read       -             -          read          proxy          -
//   composite         -          read          read       read          game Output    -
//   preview           -          read          read       -             preview        -
//   dump (before)     read       read          read       read          -              dump
//   dump (after)      read       -             -          -             -              dump
//   stats clear       -          -             -          -             -              stats
//   stats input       read       -             -          read          -              stats
//   stats gain        -          read          read       -             -              stats
//
// "frame" is the game's Output, read through a shader resource view while it is in the NPSR state; the composite
// reads and writes it through u0 instead, in place. "exposure" is the game's exposure texture, or with the scene
// white point our own 1x1 texture that the meter's resolve pass writes.

#ifdef __cplusplus
#include <cstdint>
#define NR_UINT uint32_t
#define NR_FLOAT float
#else
#define NR_UINT uint
#define NR_FLOAT float
#endif

// nr.hlsl
#define NR_MODE_ENCODE 0      // the game's Output -> the model's input (the proxy)
#define NR_MODE_COMPOSITE 1   // the model's output -> the game's Output, in place
#define NR_MODE_PREVIEW 2     // the proxy or the model's output -> the small preview picture
#define NR_MODE_DUMP_BEFORE 3 // frame dump, before the composite: the frame, the proxy, the model's output
#define NR_MODE_DUMP_AFTER 4  // frame dump, after the composite: the result

// nr_stats.hlsl
#define NR_STATS_CLEAR 0 // zero the statistics buffer and stamp the frame number into both ends
#define NR_STATS_INPUT 1 // histograms of the frame relative to the white point, and the counts
#define NR_STATS_GAIN 2  // histogram of the brightness change the model made
#define NR_STATS_METER 3   // the scene meter: histogram of the frame's luminance in its own units
#define NR_STATS_RESOLVE 4 // the scene white point from that histogram, eased towards over time; empties the histogram

// NrConstants.flags
#define NR_FLAG_LINEAR_HDR 0x01     // the frame is linear HDR: white point, shoulder, sRGB. Else it is display-encoded
                                    // already and the encode is a copy.
#define NR_FLAG_EXPOSURE 0x02       // the white point is divided by the exposure texture's value (t3): the game's,
                                    // or ours from the scene meter
#define NR_FLAG_CARD 0x04           // the calibration card replaces the frame inside its rectangle
#define NR_FLAG_ZEBRA 0x10          // the preview marks the shoulder and heavy compression
#define NR_FLAG_PREVIEW_OUTPUT 0x20 // the preview shows the model's output instead of its input
#define NR_FLAG_SCENE_RESET 0x40    // the scene white point jumps to this frame's instead of easing towards it

struct NrConstants
{
    NR_UINT mode;      // NR_MODE_* or NR_STATS_*
    NR_UINT width;     // the frame: the game's output size
    NR_UINT height;
    NR_UINT baseX;     // where the frame starts inside the game's Output texture (its output subrect)
    NR_UINT baseY;
    NR_UINT badgeSize; // side of the badge square in the bottom-right corner, in pixels; 0 = no badge
    NR_UINT flags;     // NR_FLAG_*
    NR_UINT frame;     // our frame counter + 1 (never 0): stamps the statistics and the dump

    NR_FLOAT whiteScale; // the white point before the exposure texture: pre-exposure (or 1) times 2^WhiteEV
    NR_FLOAT shoulder;   // Shoulder: where the encode's compression starts, as a fraction of the white point
    NR_FLOAT detail;     // DetailStrength
    NR_FLOAT colour;     // ColourStrength
    NR_FLOAT maxGain;    // MaxGainEV
    NR_FLOAT highlight;  // HighlightRestore

    NR_UINT cardX;     // the calibration card's rectangle inside the frame; cardW 0 = no card
    NR_UINT cardY;
    NR_UINT cardW;
    NR_UINT cardH;

    NR_UINT part;        // dump: 0 the reduced whole frame, 1 the full-resolution crop from the centre
    NR_UINT scale;       // preview and dump: frame pixels per picture pixel along each axis
    NR_UINT outWidth;    // preview and dump: the picture's size (the dispatch covers it)
    NR_UINT outHeight;

    NR_FLOAT seconds;  // meter resolve: seconds since the frame metered before this one
    NR_UINT splitX;    // the split screen (Compare): the composite leaves pixels left of this x as DLSS
                       // made them and draws the divider there; 0 = no split (M3)
};

#ifdef __cplusplus
static_assert(sizeof(NrConstants) == 96, "NrConstants is passed as root constants: 4-byte scalars, 16-byte multiple");
#define NR_CONSTANTS_DWORDS (uint32_t(sizeof(NrConstants) / 4))
#endif

// The scene meter's buffer: a histogram of the frame's luminance in its own units, 1/8 EV bins from
// -24 to +24 EV; values outside go into the end bins, black and non-finite pixels are left out. The resolve pass
// empties it after reading it, so every frame starts from zero (the buffer is created zeroed).
#define NR_METER_EV_MIN (-24)
#define NR_METER_PER_EV 8
#define NR_METER_BINS 384
#define NR_METER_WORDS NR_METER_BINS

// The statistics buffer (the menu's readouts, and M4's measurements): 32-bit words.
#define NR_HIST_EV_MIN (-16)   // the frame histograms: 1/8 EV bins, -16 to +8 EV relative to the white point
#define NR_HIST_PER_EV 8
#define NR_HIST_BINS 192
#define NR_GAIN_EV_MIN (-4)    // the gain histogram: 1/16 EV bins, -4 to +4 EV
#define NR_GAIN_PER_EV 16
#define NR_GAIN_BINS 128

#define NR_S_TAG_FIRST 0                                // frame + 1 (NrConstants.frame), written by the clear pass
#define NR_S_MAX_HIST 1                                 // largest channel / white point
#define NR_S_LUM_HIST (NR_S_MAX_HIST + NR_HIST_BINS)    // luminance / white point
#define NR_S_GAIN_HIST (NR_S_LUM_HIST + NR_HIST_BINS)   // log2 of the model's luminance / the proxy's
#define NR_S_COUNTS (NR_S_GAIN_HIST + NR_GAIN_BINS)
#define NR_C_PIXELS 0      // pixels the input pass looked at
#define NR_C_NONFINITE 1   // a channel is NaN or infinite
#define NR_C_NEGATIVE 2    // a channel is below zero
#define NR_C_BLACK 3       // largest channel <= 0: no place in the histogram
#define NR_C_BELOW 4       // below the histogram's range
#define NR_C_ABOVE 5       // above it
#define NR_C_SHOULDER 6    // largest channel / white point above Shoulder: compressed by the encode
#define NR_C_HEAVY 7       // compressed by more than 3 EV (the curve's slope below 1/8)
#define NR_C_DARK 8        // the proxy's largest channel below 0.5/255: 0 in 8 bits
#define NR_C_GAIN_PIXELS 9 // pixels the gain pass looked at
#define NR_C_GAIN_LOW 10   // the model's change was below -MaxGainEV, so the composite clamps it
#define NR_C_GAIN_HIGH 11  // above +MaxGainEV
#define NR_C_MAX_BITS 12   // largest finite channel value in the frame, as float bits
#define NR_C_EXPOSURE 13   // the exposure texture's value, as float bits (0 when none)
#define NR_C_WHITE 14      // the white point used, as float bits
#define NR_C_COLOUR 15     // pixels whose chromaticity the model changed by more than 0.05 (sum over R, G, B)
#define NR_COUNTS 16
#define NR_S_TAG_LAST (NR_S_COUNTS + NR_COUNTS)         // frame + 1 again
#define NR_S_WORDS (NR_S_TAG_LAST + 1)

// The frame dump (tools/bzdump.py reads it): NR_DUMP_HEADER_BYTES of words, then four pictures of the frame reduced
// NR_DUMP_SCALE times along each axis, then the same four as a full-resolution crop from the centre, each pixel four
// halves (two words), then a closing word. The pictures: the frame before the composite, the proxy (its alpha: 1 where
// a pixel went into the shoulder, 2 where it was compressed by more than 3 EV, 0 elsewhere; the largest in the square
// for the reduced picture), the model's output, the frame after the composite. The file adds a header of its own in
// front (nr_dx12.cpp, tools/bzdump.py).
#define NR_DUMP_SCALE 4
#define NR_DUMP_CROP 512
#define NR_DUMP_PICTURES 4
#define NR_DUMP_HEADER_BYTES 64
#define NR_D_TAG 0      // frame + 1, written by the pass before the composite; the closing word is frame + 1 again,
                        // written by the pass after it, so both ends match only once the whole copy has landed
#define NR_D_WHITE 1    // the white point used, as float bits
#define NR_D_EXPOSURE 2 // the exposure texture's value, as float bits (0 when none)
