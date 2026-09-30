// dxgitest: checks the built dxgi.dll the way a game meets it. It is built into the same folder as dxgi.dll and
// imports dxgi.dll by name, so the loader picks ours from beside the exe before System32's, as in a game directory.
//
//   build\Release\dxgitest.exe
//
// Checks that
//   - this exe's dxgi.dll import was resolved to our DLL, and ours loaded the real System32\dxgi.dll;
//   - ours exports exactly the real one's names and ordinals;
//   - every one of our exports, as built, jumps to the real export of the same name (or ordinal);
//   - CreateDXGIFactory, CreateDXGIFactory1 and CreateDXGIFactory2 through ours hand back the real DXGI's factory,
//     and its first adapter creates a D3D12 device;
//   - dlssnr.log says how many exports are forwarded, and that it is watching for the NGX core to load.
// Prints every check; the exit code is the number that failed. Writes nothing but ours' own dlssnr.log (and
// dlssnr.prev.log, the run before's).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

// This exe's import slot for dxgi.dll's CreateDXGIFactory1, which the loader fills. It is the one static reference to
// dxgi.dll here: every call below goes through GetProcAddress, and without a reference the linker drops the import
// and no dxgi.dll is loaded at start-up.
extern "C" void* __imp_CreateDXGIFactory1;

namespace
{
int g_failures = 0;

void Check(bool ok, const char* format, ...)
{
    std::fputs(ok ? "  ok    " : "  FAIL  ", stdout);
    va_list args;
    va_start(args, format);
    std::vprintf(format, args);
    va_end(args);
    std::fputc('\n', stdout);
    if (!ok)
        ++g_failures;
}

std::wstring Folder(HMODULE module)
{
    wchar_t path[MAX_PATH];
    const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
    std::wstring folder(path, length);
    return folder.substr(0, folder.find_last_of(L'\\'));
}

std::wstring SystemFolder()
{
    wchar_t path[MAX_PATH];
    const UINT length = GetSystemDirectoryW(path, MAX_PATH);
    return std::wstring(path, length);
}

HMODULE ModuleOf(const void* address)
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(address), &module);
    return module;
}

struct Export
{
    std::string name; // empty when exported by ordinal only
    const BYTE* address;
};

// Ordinal -> export, read from the loaded image's export directory.
std::map<DWORD, Export> ExportsOf(HMODULE module)
{
    const auto* base = reinterpret_cast<const BYTE*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    std::map<DWORD, Export> exports;
    if (directory.VirtualAddress == 0)
        return exports;

    const auto* table = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + directory.VirtualAddress);
    const auto* functions = reinterpret_cast<const DWORD*>(base + table->AddressOfFunctions);
    const auto* names = reinterpret_cast<const DWORD*>(base + table->AddressOfNames);
    const auto* nameIndices = reinterpret_cast<const WORD*>(base + table->AddressOfNameOrdinals);
    for (DWORD i = 0; i < table->NumberOfFunctions; ++i)
    {
        if (functions[i] != 0)
            exports[table->Base + i] = { "", base + functions[i] };
    }
    for (DWORD i = 0; i < table->NumberOfNames; ++i)
        exports[table->Base + nameIndices[i]].name = reinterpret_cast<const char*>(base + names[i]);
    return exports;
}

// The DLL file mapped as an image, as it was built: before the loader or anyone else touches it. Stays mapped.
const BYTE* MapImage(const std::wstring& path)
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return nullptr;
    const HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY | SEC_IMAGE, 0, 0, nullptr);
    CloseHandle(file);
    if (mapping == nullptr)
        return nullptr;
    const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(mapping);
    return static_cast<const BYTE*>(view);
}

// Where our stub at `rva` jumps. dxgi_stubs.asm assembles each as jmp qword ptr [rip+disp32]: FF 25 disp32. The code
// is read from `file`, the DLL file mapped as an image, because security software may patch it in memory (on the dev
// PC Bitdefender hooks CreateDXGIFactory* in whatever module is named dxgi.dll); the slot it jumps through is read
// from `loaded`, the DLL the loader loaded.
const void* StubTarget(const BYTE* file, const BYTE* loaded, size_t rva)
{
    const BYTE* stub = file + rva;
    if (stub[0] != 0xFF || stub[1] != 0x25)
        return nullptr;
    int32_t displacement;
    std::memcpy(&displacement, stub + 2, sizeof displacement);
    const void* target;
    std::memcpy(&target, loaded + rva + 6 + displacement, sizeof target);
    return target;
}

std::string Label(DWORD ordinal, const std::string& name)
{
    return (name.empty() ? std::string("(no name)") : name) + " @" + std::to_string(ordinal);
}

// Which module the object's vtable lives in: the DLL that made it.
std::wstring MakerOf(IUnknown* object)
{
    wchar_t path[MAX_PATH] = L"?";
    GetModuleFileNameW(ModuleOf(*reinterpret_cast<void**>(object)), path, MAX_PATH);
    return path;
}

template <typename Function> Function Get(HMODULE module, const char* name)
{
    return reinterpret_cast<Function>(GetProcAddress(module, name));
}
} // namespace

int main()
{
    const std::wstring ourPath = Folder(nullptr) + L"\\dxgi.dll";
    const std::wstring realPath = SystemFolder() + L"\\dxgi.dll";

    std::puts("Loader");
    // Nothing else in this process loads a dxgi.dll by this path: if it is loaded, the loader chose it for the
    // exe's import.
    const HMODULE ours = GetModuleHandleW(ourPath.c_str());
    Check(ours != nullptr && ModuleOf(__imp_CreateDXGIFactory1) == ours, "this exe's dxgi.dll import resolved to %ls",
          ourPath.c_str());
    const HMODULE real = GetModuleHandleW(realPath.c_str());
    Check(real != nullptr, "the real %ls is loaded", realPath.c_str());
    if (ours == nullptr || real == nullptr)
    {
        std::printf("FAIL (%d)\n", g_failures);
        return g_failures;
    }

    std::puts("Export table");
    const std::map<DWORD, Export> ourExports = ExportsOf(ours);
    const std::map<DWORD, Export> realExports = ExportsOf(real);
    int different = 0;
    for (const auto& [ordinal, e] : realExports)
    {
        const auto found = ourExports.find(ordinal);
        if (found == ourExports.end())
        {
            Check(false, "missing %s", Label(ordinal, e.name).c_str());
            ++different;
        }
        else if (found->second.name != e.name)
        {
            Check(false, "@%lu is %s, the real one is %s", ordinal, Label(ordinal, found->second.name).c_str(),
                  Label(ordinal, e.name).c_str());
            ++different;
        }
    }
    for (const auto& [ordinal, e] : ourExports)
    {
        if (!realExports.contains(ordinal))
        {
            Check(false, "extra %s", Label(ordinal, e.name).c_str());
            ++different;
        }
    }
    Check(different == 0 && !realExports.empty(), "%zu exports, the same names and ordinals as the real one",
          ourExports.size());

    std::puts("Stubs");
    const BYTE* built = MapImage(ourPath);
    Check(built != nullptr, "mapped %ls to read the stubs as built", ourPath.c_str());
    for (const auto& [ordinal, e] : ourExports)
    {
        const char* key = e.name.empty() ? MAKEINTRESOURCEA(ordinal) : e.name.c_str();
        const void* expected = reinterpret_cast<const void*>(GetProcAddress(real, key));
        const size_t rva = e.address - reinterpret_cast<const BYTE*>(ours);
        const void* target = built != nullptr ? StubTarget(built, reinterpret_cast<const BYTE*>(ours), rva) : nullptr;
        const bool patched = built != nullptr && std::memcmp(e.address, built + rva, 6) != 0;
        Check(target != nullptr && target == expected, "%-40s -> %p (real %p)%s", Label(ordinal, e.name).c_str(),
              target, expected, patched ? ", patched in memory by someone else" : "");
    }

    std::puts("Calls");
    using CreateFactory = HRESULT(WINAPI*)(REFIID, void**);
    const struct
    {
        const char* name;
        IID iid;
    } factories[] = {
        { "CreateDXGIFactory", __uuidof(IDXGIFactory) },
        { "CreateDXGIFactory1", __uuidof(IDXGIFactory1) },
    };
    for (const auto& factory : factories)
    {
        const auto create = Get<CreateFactory>(ours, factory.name);
        ComPtr<IUnknown> created;
        const HRESULT result =
            create != nullptr ? create(factory.iid, reinterpret_cast<void**>(created.GetAddressOf())) : E_NOINTERFACE;
        Check(SUCCEEDED(result) && ModuleOf(*reinterpret_cast<void**>(created.Get())) == real,
              "%s -> 0x%08lX, made by %ls", factory.name, result, created ? MakerOf(created.Get()).c_str() : L"-");
    }

    const auto create2 = Get<HRESULT(WINAPI*)(UINT, REFIID, void**)>(ours, "CreateDXGIFactory2");
    ComPtr<IDXGIFactory4> factory;
    const HRESULT result = create2 != nullptr ? create2(0, IID_PPV_ARGS(&factory)) : E_NOINTERFACE;
    Check(SUCCEEDED(result) && ModuleOf(*reinterpret_cast<void**>(factory.Get())) == real,
          "CreateDXGIFactory2 -> 0x%08lX, made by %ls", result, factory ? MakerOf(factory.Get()).c_str() : L"-");

    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 description = {};
    const bool haveAdapter =
        factory && SUCCEEDED(factory->EnumAdapters1(0, &adapter)) && SUCCEEDED(adapter->GetDesc1(&description));
    Check(haveAdapter, "adapter 0: %ls", description.Description);
    ComPtr<ID3D12Device> device;
    const HRESULT deviceResult =
        haveAdapter ? D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)) : E_FAIL;
    Check(SUCCEEDED(deviceResult), "D3D12CreateDevice on it -> 0x%08lX", deviceResult);

    std::puts("dlssnr.log");
    std::ifstream file((Folder(nullptr) + L"\\dlssnr.log").c_str());
    std::stringstream log;
    log << file.rdbuf();
    std::string line;
    bool forwarding = false;
    while (std::getline(log, line))
    {
        std::printf("  | %s\n", line.c_str());
        forwarding = forwarding || line.find("forwarding " + std::to_string(ourExports.size()) + " of " +
                                             std::to_string(ourExports.size()) + " exports") != std::string::npos;
    }
    Check(forwarding, "says it forwards all %zu exports", ourExports.size());
    Check(log.str().find("watching for _nvngx.dll") != std::string::npos,
          "says it is watching for the NGX core to load");

    if (g_failures == 0)
        std::puts("PASS");
    else
        std::printf("FAIL (%d)\n", g_failures);
    return g_failures;
}
