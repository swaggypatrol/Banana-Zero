// No GPU/model execution: parameter types, retained defaults, version profiles and feature rebuild decisions.
#include <cstdio>
#include <string>
#include <unordered_map>

#include "model_config.h"
#include "nvsdk_ngx_params.h"

namespace
{
int checks = 0, failures = 0;
void Check(bool ok, const char* name)
{
    ++checks;
    failures += !ok;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", name);
}
struct Value { char type; double number; };
struct Parameters : NVSDK_NGX_Parameter
{
    std::unordered_map<std::string, Value> values;
    void Put(const char* key, char type, double v) { values[key] = { type, v }; }
    void Set(const char* k, unsigned long long v) override { Put(k, 'q', double(v)); }
    void Set(const char* k, float v) override { Put(k, 'f', v); }
    void Set(const char* k, double v) override { Put(k, 'd', v); }
    void Set(const char* k, unsigned v) override { Put(k, 'u', v); }
    void Set(const char* k, int v) override { Put(k, 'i', v); }
    void Set(const char* k, ID3D11Resource*) override { Put(k, 'a', 0); }
    void Set(const char* k, ID3D12Resource*) override { Put(k, 'b', 0); }
    void Set(const char* k, void*) override { Put(k, 'p', 0); }
#define GET(T) NVSDK_NGX_Result Get(const char*, T*) const override { return NVSDK_NGX_Result_FAIL_UnsupportedParameter; }
    GET(unsigned long long) GET(float) GET(double) GET(unsigned) GET(int)
    GET(ID3D11Resource*) GET(ID3D12Resource*) GET(void*)
#undef GET
    void Reset() override { values.clear(); }
    bool Is(const char* k, char type, double number) const
    {
        const auto i = values.find(k);
        return i != values.end() && i->second.type == type && i->second.number == number;
    }
};
}

int wmain(int argc, wchar_t** argv)
{
    const ModelCapabilities old { 310, 8, 0, 0 }, fresh { 310, 9, 16, 0 }, unknown { 310, 10, 0, 0 };
    Check(old.Known() && !old.MaskBlend() && old.DefaultPreset() == 1, "310.8 legacy capabilities");
    Check(fresh.Known() && fresh.MaskBlend() && fresh.DefaultPreset() == 2, "310.9 capabilities");
    Check(!unknown.Known() && !unknown.MaskBlend() && unknown.DefaultPreset() == 0, "unknown version is conservative");
    Check(!ModelCapabilities { 310, 9, 16, 1 }.Known(), "do not extrapolate to another revision");
    Check(!ModelCapabilities { 310, 9, 16, 1 }.MultiPreset() && !ModelCapabilities { 310, 9, 16, 1 }.MaskBlend(),
          "another revision without content evidence gets no new controls");
    // A later build whose content still carries both weight sets and the key name: controls on, profile unverified.
    ModelCapabilities later { 310, 9, 20, 0 };
    later.probed = later.weightsHt = later.weightsCcSmall = later.maskBlendKey = true;
    Check(!later.Known() && later.MultiPreset() && later.MaskBlend() && later.DefaultPreset() == 0,
          "unknown build with both weights and the key: controls from content, default unknown");
    Check(later.ResolvePreset(1) == 1 && later.ResolvePreset(2) == 2 && later.ResolvePreset(0) == 0 &&
              later.ResolvePreset(3) == 0, "unknown build resolves only explicit 1/2");
    ModelCapabilities htOnly { 310, 9, 20, 0 };
    htOnly.probed = htOnly.weightsHt = true;
    Check(!htOnly.MultiPreset() && !htOnly.MaskBlend(), "one weight set and no key: no new controls");
    ModelCapabilities legacyContent = old;
    legacyContent.probed = legacyContent.weightsHt = legacyContent.weightsCcSmall = legacyContent.maskBlendKey = true;
    Check(!legacyContent.MultiPreset() && !legacyContent.MaskBlend(), "the 310.8 profile is never widened by content");
    Check(std::string(ModelPresetName(1)) == "HT" && std::string(ModelPresetName(2)) == "CC Small FP8" &&
              std::string(ModelPresetName(0)) == "unknown", "preset names");
    for (unsigned p : { 0u, 1u, 2u, 3u, 0xffffffffu })
    {
        Check(old.ResolvePreset(p) == 1, "legacy fallback is always 1");
        Check(fresh.ResolvePreset(p) == ((p & 0x7fffffff) == 1 ? 1u : 2u), "new shipping fallback is 2");
    }
    ModelSettings m;
    Parameters params;
    params.Set("Sentinel", 99);
    m.preset = { true, 1 };
    ModelFillCreate(&params, 1920, 1080, m);
    Check(params.Is("DLSSNR.Hint.Render.Preset", 'i', 1), "preset is a creation int");
    m.preset = { true, 2 };
    ModelFillCreate(&params, 1920, 1080, m);
    Check(params.Is("DLSSNR.Hint.Render.Preset", 'i', 2), "preset 2 overwrites preset 1");
    m.preset.set = false;
    ModelFillCreate(&params, 1920, 1080, m);
    Check(params.Is("DLSSNR.Hint.Render.Preset", 'i', 0), "default explicitly clears a retained preset");
    Check(params.Is("Sentinel", 'i', 99), "never reset the parameter block or erase callbacks");
    Check(params.Is("DLSSNR.Width", 'u', 1920) && params.Is("DLSSNR.Height", 'u', 1080), "creation dimensions unchanged");

    const ModelSkyMask noSky, sky { true, 0.5f };
    params.values.clear();
    m = {};
    m.maskBlendMode = { true, 3 };
    ModelFillTunables(&params, fresh, m, noSky);
    Check(params.Is("DLSSNR.MaskBlendMode", 'i', 3), "mask blend uses the int overload");
    Check(!params.values.contains("DLSSNR.Hint.Render.Preset"), "never switch presets in the evaluate block");
    m.maskBlendMode.set = false;
    ModelFillTunables(&params, fresh, m, noSky);
    Check(params.Is("DLSSNR.MaskBlendMode", 'i', 0), "Auto with no sky mask explicitly overwrites retained mode 3 with 0");
    Check(params.Is("DLSSNR.UseAutoMask", 'i', 0) && params.Is("DLSSNR.LocalToneStrength", 'f', 1), "legacy per-frame defaults preserved");
    m.maskBlendMode = { true, 0xffffffffu };
    ModelFillTunables(&params, fresh, m, noSky);
    Check(params.Is("DLSSNR.MaskBlendMode", 'i', 0), "invalid blend falls back to 0, not 3");
    for (const auto& caps : { old, unknown })
    {
        Parameters p;
        ModelFillTunables(&p, caps, m, sky);
        Check(!p.values.contains("DLSSNR.MaskBlendMode"), "no new parameter for legacy/unknown profiles");
    }
    for (unsigned mode = 0; mode <= 3; ++mode)
    {
        m.maskBlendMode = { true, mode };
        Check(ModelExternalExcludesAuto(fresh, m, sky) == (mode == 0), "new external mask gating follows mode");
        Check(ModelExternalExcludesAuto(old, m, sky), "legacy external mask always excludes automatic mask");
        Check(!ModelExternalExcludesAuto(fresh, m, noSky), "without an external mask automatic mask is allowed");
    }
    m = {};
    const auto initial = ModelCreateKey(fresh, 1920, 1080, m);
    m.style = { true, 2 }; m.localTone = { true, 0.5f }; m.maskBlendMode = { true, 2 };
    Check(initial == ModelCreateKey(fresh, 1920, 1080, m), "per-evaluate sliders do not change the creation key");
    m.preset = { true, 2 };
    Check(ModelCreateKey(fresh, 1920, 1080, m) == initial,
          "310.9.16.0: default and explicit 2 select the same weights, so no rebuild");
    for (unsigned p : { 1u, 2u, 7u })
    {
        m.preset = { true, p };
        Check(ModelCreateKey(old, 1920, 1080, m) == ModelCreateKey(old, 1920, 1080, ModelSettings {}),
              "310.8: every request selects HT, so no rebuild");
    }
    m.preset = { true, 2 };
    Check(ModelCreateKey(later, 1920, 1080, m) != ModelCreateKey(later, 1920, 1080, ModelSettings {}),
          "unknown build: the raw request is the key");
    m.preset = { true, 1 };
    const auto presetHt = ModelCreateKey(fresh, 1920, 1080, m);
    Check(initial != presetHt, "preset changes the creation key");
    Check(ModelPlanRebuild(initial, presetHt, 29, 30) == ModelRebuild::Keep, "pending preset keeps the old model running");
    Check(ModelPlanRebuild(initial, presetHt, 30, 30) == ModelRebuild::Recreate, "preset rebuilds at the throttle boundary");
    Check(ModelPlanRebuild(presetHt, presetHt, 100, 30) == ModelRebuild::Keep, "stable model never rebuilds");
    const auto resized = ModelCreateKey(fresh, 1280, 720, m);
    Check(ModelPlanRebuild(presetHt, resized, 29, 30) == ModelRebuild::WaitForGeometry, "new geometry waits, never evaluates the wrong-sized model");
    Check(ModelPlanRebuild(presetHt, resized, 30, 30) == ModelRebuild::Recreate, "geometry rebuilds when due");
    m.preset.set = false;
    Check(ModelCreateKey(fresh, 1920, 1080, m) == initial, "restoring default changes creation key back");
    m.autoMask = { true, 1 };
    m.maskBlendMode = { true, 2 };
    Check(ModelMaskBlendActive(fresh, m, sky), "blend acts with an external mask, auto mask and nonzero mode");
    Check(!ModelMaskBlendActive(fresh, m, noSky), "blend needs the external mask");
    Check(!ModelMaskBlendActive(old, m, sky), "blend never acts on 310.8");
    m.autoMask = { true, 0 };
    Check(!ModelMaskBlendActive(fresh, m, sky), "blend needs the auto mask's strengths");
    m.autoMask = { true, 1 };
    m.maskBlendMode = { true, 0 };
    Check(!ModelMaskBlendActive(fresh, m, sky), "mode 0 never blends");

    // Auto (not set): the mode that keeps what the sky and skin sliders ask for.
    auto resolve = [&](float skyStructure, float skin, unsigned automatic, bool present = true) {
        ModelSettings a;
        a.autoMask = { true, automatic };
        if (skin >= 0.0f)
            a.skinStructure = { true, skin };
        return ModelMaskBlendResolve(fresh, a, ModelSkyMask { present, skyStructure });
    };
    Check(resolve(0.6f, 0.5f, 1).mode == 2 && resolve(0.6f, 0.5f, 1).why == ModelBlendWhy::BothDown,
          "Auto: sky and skin both down keep the lower");
    Check(resolve(0.6f, -1.0f, 1).mode == 2, "Auto: sky down, skin following local structure: the lower");
    Check(resolve(1.0f, 0.5f, 1).mode == 2, "Auto: only skin down (sky tone moved): the lower");
    Check(resolve(1.3f, 1.2f, 1).mode == 3 && resolve(1.3f, 1.2f, 1).why == ModelBlendWhy::BothUp,
          "Auto: both up keep the higher");
    Check(resolve(1.3f, -1.0f, 1).mode == 3, "Auto: only sky up: the higher");
    Check(resolve(0.6f, 1.2f, 1).mode == 1 && resolve(1.3f, 0.5f, 1).mode == 1 &&
              resolve(0.6f, 1.2f, 1).why == ModelBlendWhy::Opposite, "Auto: opposite ways average");
    Check(resolve(1.0f, -1.0f, 1).mode == 2 && resolve(1.0f, -1.0f, 1).why == ModelBlendWhy::Untouched,
          "Auto: nothing moved still combines, so the auto mask stays on");
    Check(resolve(0.6f, 0.5f, 0).mode == 0 && resolve(0.6f, 0.5f, 0).why == ModelBlendWhy::NoAutoMask,
          "Auto: auto mask off sends 0 (identical picture, nrprobe T11)");
    Check(resolve(0.6f, 0.5f, 1, false).mode == 0 && resolve(0.6f, 0.5f, 1, false).why == ModelBlendWhy::NoSky,
          "Auto: no sky mask sends 0");
    {
        ModelSettings a;
        a.autoMask = { true, 1 };
        a.localStructure = { true, 0.8f };
        a.skinStructure = { true, 0.9f }; // above local structure: up, even though below 1
        Check(ModelMaskBlendResolve(fresh, a, ModelSkyMask { true, 0.6f }).mode == 1,
              "Auto: skin direction is relative to Local structure");
        Check(!ModelExternalExcludesAuto(fresh, ModelSettings {}, sky),
              "Auto never turns the auto mask off, so it can be switched on with a sky mask");
        Check(ModelMaskBlendResolve(old, a, sky).why == ModelBlendWhy::Unsupported, "Auto on 310.8: unsupported");
        a.maskBlendMode = { true, 3 };
        Check(ModelMaskBlendResolve(fresh, a, ModelSkyMask { true, 0.6f }).mode == 3 &&
                  ModelMaskBlendResolve(fresh, a, ModelSkyMask { true, 0.6f }).why == ModelBlendWhy::Manual,
              "a mode set by hand wins over Auto");
        Parameters p;
        a.maskBlendMode = {};
        ModelFillTunables(&p, fresh, a, ModelSkyMask { true, 0.6f });
        Check(p.Is("DLSSNR.MaskBlendMode", 'i', 1), "Auto's mode is what reaches the model");
    }
    Check(!ModelInspectFile(L"does-not-exist.dll").Known(), "unreadable file has no experimental capabilities");
    if (argc == 3)
    {
        const ModelCapabilities a = ModelInspectFile(argv[1]), b = ModelInspectFile(argv[2]);
        Check(a.Legacy(), "old fixture version read without loading DLL code");
        Check(b.KnownMulti() && b.MultiPreset(), "new fixture version read without loading DLL code");
        Check(a.probed && a.weightsHt && !a.weightsCcSmall && !a.maskBlendKey,
              "old fixture content: WEIGHTS_HT only, no MaskBlendMode key");
        Check(b.probed && b.weightsHt && b.weightsCcSmall && b.maskBlendKey,
              "new fixture content: both weight sets and the MaskBlendMode key");
    }
    else if (argc != 1)
        Check(false, "usage: modeltest [old-dll new-dll]");
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
