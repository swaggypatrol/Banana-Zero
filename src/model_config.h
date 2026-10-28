#pragma once

#include "settings.h"

#include <cstdint>

struct NVSDK_NGX_Parameter;

// What a model DLL offers, from its file alone (no NVIDIA code is run). Two sources:
//  - the PE fixed file version: 310.8.0.0 and 310.9.16.0 are the two builds taken apart and tested ("known");
//  - the file's content: the named RT_RCDATA weight resources (WEIGHTS_HT, WEIGHTS_CC_SMALL_FP8) and the per-frame key
//    string "DLSSNR.MaskBlendMode" in its read-only data. A later build that still carries them (say 310.9.20) gets the
//    controls too, marked unverified, instead of silently losing them because its version number moved.
// Neither is a signature or trust check.
struct ModelCapabilities
{
    unsigned major = 0, minor = 0, build = 0, revision = 0;
    bool probed = false;         // the content below was inspected
    bool weightsHt = false;      // RT_RCDATA "WEIGHTS_HT"
    bool weightsCcSmall = false; // RT_RCDATA "WEIGHTS_CC_SMALL_FP8"
    bool maskBlendKey = false;   // the string "DLSSNR.MaskBlendMode" in a read-only section

    bool Legacy() const { return major == 310 && minor == 8 && build == 0 && revision == 0; }
    bool KnownMulti() const { return major == 310 && minor == 9 && build == 16 && revision == 0; }
    bool Known() const { return Legacy() || KnownMulti(); }
    // Two weight sets: the tested 310.9.16.0, or an unknown build carrying both weight resources.
    bool MultiPreset() const { return KnownMulti() || (!Legacy() && weightsHt && weightsCcSmall); }
    // The new per-frame key: the tested 310.9.16.0, or an unknown build whose data contains the key name.
    bool MaskBlend() const { return KnownMulti() || (!Legacy() && maskBlendKey); }
    // The preset the DLL falls back to for 0 / absent / unlisted requests; 0 when not known.
    unsigned DefaultPreset() const { return Legacy() ? 1u : KnownMulti() ? 2u : 0u; }
    // The preset a creation with this request ends up with; 0 when not known.
    unsigned ResolvePreset(unsigned requested) const
    {
        requested &= 0x7fffffffu; // the model masks this bit at creation
        if (Legacy())
            return 1u;
        if (KnownMulti())
            return requested == 1 ? 1u : 2u;
        if (MultiPreset() && (requested == 1 || requested == 2))
            return requested; // same descriptor numbering as 310.9.16.0 assumed, shown as unverified
        return 0u;
    }
};

ModelCapabilities ModelInspectFile(const wchar_t* path);

// Display name of a resolved preset (ResolvePreset); "unknown" for 0.
inline const char* ModelPresetName(unsigned resolved)
{
    return resolved == 1 ? "HT" : resolved == 2 ? "CC Small FP8" : "unknown";
}

inline unsigned ModelRequestedPreset(const ModelSettings& m)
{
    return m.preset.set ? m.preset.value & 0x7fffffffu : 0u;
}

// The preset as part of the creation key. For a known build two requests that select the same weights (310.9.16.0:
// 0 and 2; 310.8: everything) are one key, so switching between them neither rebuilds the feature nor spends one of
// the 64 creations. For other builds the raw request is the key.
inline unsigned ModelPresetKey(const ModelCapabilities& caps, unsigned requested)
{
    requested &= 0x7fffffffu;
    return caps.Known() ? caps.ResolvePreset(requested) : requested;
}

struct ModelCreationKey
{
    unsigned width, height, preset;
    bool operator==(const ModelCreationKey&) const = default;
};

inline ModelCreationKey ModelCreateKey(const ModelCapabilities& caps, unsigned width, unsigned height,
                                       const ModelSettings& m)
{
    return { width, height, ModelPresetKey(caps, ModelRequestedPreset(m)) };
}

enum class ModelRebuild { Keep, WaitForGeometry, Recreate };

inline ModelRebuild ModelPlanRebuild(const ModelCreationKey& active, const ModelCreationKey& requested,
                                     uint64_t sinceCreate, uint64_t delay)
{
    if (active == requested)
        return ModelRebuild::Keep;
    if (sinceCreate >= delay)
        return ModelRebuild::Recreate;
    return active.width != requested.width || active.height != requested.height
               ? ModelRebuild::WaitForGeometry : ModelRebuild::Keep;
}

// The sky mask as the model gets it: whether there is one this frame, and the factor its B channel puts on Local
// structure over the sky (Banana-Zero's mask: 1 off the sky, SkyStructure on it).
struct ModelSkyMask
{
    bool present = false;
    float structure = 1.0f;
};

// MaskBlendMode (310.9.16.0 pre-block kernels; docs/DLSSNR_COMPATIBILITY.md): with a sky mask, the auto mask on and a
// nonzero mode, the structure strength the network gets for skin and for the rest is the sky mask's (B x Local
// structure) combined with the auto mask's own (Skin structure, Local structure): 1 averaged, 2 the lower, 3 the
// higher. 0 skips that, and a sky mask then turns the auto mask off (the old rule).
//
// Not set means Auto: the mode that keeps what the sliders ask for. Off the sky the sky mask gives Local structure, so
// skin keeps Skin structure only with min when it is below Local structure and with max when above; on the sky the
// rest keeps Sky structure only with min when it is below 1 and with max when above. Both down (or untouched): min,
// both up: max, opposite ways: no mode keeps both, average gives each half. Without the auto mask or without a sky
// mask nothing is combined and Auto sends 0 (the same picture as any mode, nrprobe T11).
enum class ModelBlendWhy
{
    Unsupported, // the DLL has no MaskBlendMode: the old rule
    Manual,      // set by hand
    NoSky,       // Auto: no sky mask
    NoAutoMask,  // Auto: the auto mask is off
    Untouched,   // Auto: neither structure moved; combined so the auto mask stays on
    BothDown,    // Auto: min keeps both
    BothUp,      // Auto: max keeps both
    Opposite,    // Auto: average, half of each
};

struct ModelBlendChoice
{
    int mode;
    ModelBlendWhy why;
};

inline bool ModelAutoMaskOn(const ModelSettings& m) { return m.autoMask.set && m.autoMask.value != 0; }

inline ModelBlendChoice ModelMaskBlendResolve(const ModelCapabilities& caps, const ModelSettings& m,
                                              const ModelSkyMask& sky)
{
    if (!caps.MaskBlend())
        return { 0, ModelBlendWhy::Unsupported };
    if (m.maskBlendMode.set)
        return { m.maskBlendMode.value <= 3 ? static_cast<int>(m.maskBlendMode.value) : 0, ModelBlendWhy::Manual };
    if (!sky.present)
        return { 0, ModelBlendWhy::NoSky };
    if (!ModelAutoMaskOn(m))
        return { 0, ModelBlendWhy::NoAutoMask };
    constexpr float kEpsilon = 1.0e-4f;
    const float local = m.localStructure.set ? m.localStructure.value : 1.0f;
    const float skin = m.skinStructure.set && m.skinStructure.value >= 0.0f ? m.skinStructure.value : local;
    const int skyWay = sky.structure < 1.0f - kEpsilon ? -1 : sky.structure > 1.0f + kEpsilon ? 1 : 0;
    const int skinWay = skin < local - kEpsilon ? -1 : skin > local + kEpsilon ? 1 : 0;
    if (skyWay == 0 && skinWay == 0)
        return { 2, ModelBlendWhy::Untouched };
    if (skyWay <= 0 && skinWay <= 0)
        return { 2, ModelBlendWhy::BothDown };
    if (skyWay >= 0 && skinWay >= 0)
        return { 3, ModelBlendWhy::BothUp };
    return { 1, ModelBlendWhy::Opposite };
}

inline int ModelMaskBlendMode(const ModelCapabilities& caps, const ModelSettings& m, const ModelSkyMask& sky)
{
    return ModelMaskBlendResolve(caps, m, sky).mode;
}

inline const char* ModelBlendName(int mode)
{
    return mode == 1 ? "Average" : mode == 2 ? "Keep the lower" : mode == 3 ? "Keep the higher" : "Sky mask only";
}

// Whether the sky mask turns the model's automatic mask off: always on DLLs without MaskBlendMode, and with mode 0
// chosen by hand. Auto never does: it picks a combining mode as soon as the auto mask is on.
inline bool ModelExternalExcludesAuto(const ModelCapabilities& caps, const ModelSettings& m, const ModelSkyMask& sky)
{
    if (!sky.present)
        return false;
    if (!caps.MaskBlend())
        return true;
    return m.maskBlendMode.set && (m.maskBlendMode.value == 0 || m.maskBlendMode.value > 3);
}

// Whether the mode changes anything this frame: the GPU branch needs the sky mask, the auto mask and a nonzero mode.
inline bool ModelMaskBlendActive(const ModelCapabilities& caps, const ModelSettings& m, const ModelSkyMask& sky)
{
    return sky.present && ModelAutoMaskOn(m) && ModelMaskBlendMode(caps, m, sky) != 0;
}

// Only these creation keys select a model. Per-frame controls must never trigger a feature rebuild. Every owned
// key is written, including preset 0 and blend mode 0 on reset, because NGX parameter blocks retain old values.
void ModelFillCreate(NVSDK_NGX_Parameter* p, unsigned width, unsigned height, const ModelSettings& m);
void ModelFillTunables(NVSDK_NGX_Parameter* p, const ModelCapabilities& caps, const ModelSettings& m,
                       const ModelSkyMask& sky);
