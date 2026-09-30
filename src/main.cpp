// Process entry. DllMain runs under the loader lock, so it does only three things: load the real System32\dxgi.dll,
// set up the export forwarding, and register the DLL load notification that patches the NGX core when it loads
// (ngx_hook.cpp). Everything else happens later, inside the game's own DLSS calls.

#include <windows.h>

#include <cwchar>
#include <iterator>

#include "build_hash.h"
#include "dxgi_exports.h"
#include "log.h"
#include "ngx_hook.h"

// Where our exports go: stub i in dxgi_stubs.asm jumps through g_dxgiReal[i], the real DLL's kDxgiExports[i].
extern "C"
{
    void* g_dxgiReal[std::size(kDxgiExports)];
}

namespace
{
// Stands in for an export the real dxgi.dll no longer has, so the stub fails instead of jumping to nothing. The list
// is taken from one Windows build and an update can drop an export.
HRESULT MissingExport() { return E_NOTIMPL; }

// Loads the real dxgi.dll by its full path (by name alone the loader would hand back this DLL) and points every
// stub at it. False if it cannot be loaded: no game could run then.
bool ForwardExports()
{
    constexpr wchar_t kName[] = L"\\dxgi.dll";
    wchar_t path[MAX_PATH];
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    if (length == 0 || length + std::size(kName) > MAX_PATH)
    {
        Log("GetSystemDirectory failed (%lu)", GetLastError());
        return false;
    }
    wmemcpy(path + length, kName, std::size(kName));

    const HMODULE real = LoadLibraryW(path);
    if (real == nullptr)
    {
        Log("could not load %ls (%lu)", path, GetLastError());
        return false;
    }

    size_t forwarded = 0;
    for (size_t i = 0; i < std::size(kDxgiExports); ++i)
    {
        const DxgiExport& entry = kDxgiExports[i];
        const FARPROC address =
            GetProcAddress(real, entry.name != nullptr ? entry.name : MAKEINTRESOURCEA(entry.ordinal));
        if (address != nullptr)
        {
            g_dxgiReal[i] = reinterpret_cast<void*>(address);
            ++forwarded;
        }
        else
        {
            g_dxgiReal[i] = reinterpret_cast<void*>(&MissingExport);
            Log("%s @%u is not in the real dxgi.dll, it returns E_NOTIMPL",
                entry.name != nullptr ? entry.name : "(no name)", entry.ordinal);
        }
    }
    Log("loaded %ls, forwarding %zu of %zu exports", path, forwarded, std::size(kDxgiExports));
    return true;
}
} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_DETACH)
    {
        NgxHookStop();
        return TRUE;
    }
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;

    // Beside this DLL, in the game's directory.
    constexpr wchar_t kLogName[] = L"dlssnr.log";
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    wchar_t* slash = length > 0 && length < MAX_PATH ? wcsrchr(path, L'\\') : nullptr;
    if (slash != nullptr && size_t(path + MAX_PATH - (slash + 1)) >= std::size(kLogName))
    {
        wmemcpy(slash + 1, kLogName, std::size(kLogName));
        LogOpen(path);
    }

    SYSTEMTIME now;
    GetLocalTime(&now);
    Log("dlssnr %s, built %s %s, started %04u-%02u-%02u %02u:%02u:%02u", BUILD_HASH, __DATE__, __TIME__, now.wYear,
        now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);

    if (!ForwardExports())
        return FALSE;
    NgxHookStart();
    return TRUE;
}
