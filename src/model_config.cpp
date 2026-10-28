#include "model_config.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <string_view>
#include <vector>

#include "nvsdk_ngx_params.h"

namespace
{
// The file mapped as an image resource: sections at their RVAs, nothing executed, no DllMain, no imports resolved.
// Looks for the two named weight resources and for the MaskBlendMode key name in the small read-only sections
// (.rdata is about 230 KB; .rsrc, which holds the 260 MB of weights, is never scanned or paged in).
void ProbeContent(const wchar_t* path, ModelCapabilities* caps)
{
    const HMODULE module =
        LoadLibraryExW(path, nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (module == nullptr)
        return;
    caps->probed = true;
    caps->weightsHt = FindResourceW(module, L"WEIGHTS_HT", MAKEINTRESOURCEW(10)) != nullptr; // RT_RCDATA
    caps->weightsCcSmall = FindResourceW(module, L"WEIGHTS_CC_SMALL_FP8", MAKEINTRESOURCEW(10)) != nullptr;

    // A data-file module's handle carries flag bits in its low two bits; the mapping starts at the aligned address.
    const auto* base = reinterpret_cast<const unsigned char*>(reinterpret_cast<ULONG_PTR>(module) & ~ULONG_PTR(3));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic == IMAGE_DOS_SIGNATURE && dos->e_lfanew > 0 && dos->e_lfanew < 0x1000)
    {
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature == IMAGE_NT_SIGNATURE && nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        {
            const DWORD image = nt->OptionalHeader.SizeOfImage;
            const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
            constexpr std::string_view key("DLSSNR.MaskBlendMode", sizeof "DLSSNR.MaskBlendMode"); // with its NUL
            constexpr DWORD kMaxScan = 16u << 20;
            for (WORD i = 0; i < nt->FileHeader.NumberOfSections && !caps->maskBlendKey; ++i, ++section)
            {
                const DWORD flags = section->Characteristics;
                const DWORD size = (std::min)(section->Misc.VirtualSize, section->SizeOfRawData);
                if ((flags & IMAGE_SCN_CNT_INITIALIZED_DATA) == 0 || (flags & IMAGE_SCN_MEM_EXECUTE) != 0 ||
                    (flags & IMAGE_SCN_MEM_WRITE) != 0 || size == 0 || size > kMaxScan ||
                    std::memcmp(section->Name, ".rsrc", 5) == 0 || section->VirtualAddress >= image ||
                    size > image - section->VirtualAddress)
                    continue;
                const std::string_view data(reinterpret_cast<const char*>(base + section->VirtualAddress), size);
                caps->maskBlendKey = data.find(key) != std::string_view::npos;
            }
        }
    }
    FreeLibrary(module);
}
} // namespace

ModelCapabilities ModelInspectFile(const wchar_t* path)
{
    ModelCapabilities caps;
    if (path == nullptr || *path == L'\0')
        return caps;
    ProbeContent(path, &caps);
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &ignored);
    if (size == 0)
        return caps;
    std::vector<unsigned char> bytes(size);
    if (!GetFileVersionInfoW(path, 0, size, bytes.data()))
        return caps;
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(bytes.data(), L"\\", reinterpret_cast<void**>(&info), &length) ||
        info == nullptr || length < sizeof *info || info->dwSignature != 0xfeef04bd)
        return caps;
    caps.major = HIWORD(info->dwFileVersionMS);
    caps.minor = LOWORD(info->dwFileVersionMS);
    caps.build = HIWORD(info->dwFileVersionLS);
    caps.revision = LOWORD(info->dwFileVersionLS);
    return caps;
}

void ModelFillCreate(NVSDK_NGX_Parameter* p, unsigned width, unsigned height, const ModelSettings& m)
{
    p->Set("DLSSNR.Enabled", 1u);
    p->Set("DLSSNR.Width", width);
    p->Set("DLSSNR.Height", height);
    p->Set(NVSDK_NGX_Parameter_CreationNodeMask, 1u);
    p->Set(NVSDK_NGX_Parameter_VisibilityNodeMask, 1u);
    p->Set("DLSSNR.Hint.Render.Preset", static_cast<int>(ModelRequestedPreset(m)));
}

void ModelFillTunables(NVSDK_NGX_Parameter* p, const ModelCapabilities& caps, const ModelSettings& m,
                       const ModelSkyMask& sky)
{
    p->Set("DLSSNR.Intensity", m.intensity.set ? m.intensity.value : 1.0f);
    p->Set("DLSSNR.Style", m.style.set ? m.style.value : 0u);
    p->Set("DLSSNR.LocalStructureStrength", m.localStructure.set ? m.localStructure.value : 1.0f);
    p->Set("DLSSNR.LocalToneStrength", m.localTone.set ? m.localTone.value : 1.0f);
    p->Set("DLSSNR.SkinStructureStrength", m.skinStructure.set ? m.skinStructure.value : -1.0f);
    p->Set("DLSSNR.UseAutoMask", m.autoMask.set && m.autoMask.value != 0 ? 1 : 0);
    if (caps.MaskBlend())
        p->Set("DLSSNR.MaskBlendMode", ModelMaskBlendMode(caps, m, sky)); // the DLL reads Get(int*), not float/ULL
}
