#include "../../src/kernel/nv2a_shader_cache.h"
#include <winioctl.h>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

static int failures;
static void check(const char *name, bool condition)
{
    if (!condition) { std::printf("FAIL: %s\n", name); failures++; }
}

int main()
{
    wchar_t temporary[MAX_PATH], root[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, root) || !GetTempFileNameW(root, L"nvt", 0, temporary) ||
        !DeleteFileW(temporary) || !CreateDirectoryW(temporary, nullptr)) {
        std::fprintf(stderr, "shader cache test directory failed: %lu\n", GetLastError());
        return 1;
    }
    fs::path directory(temporary);
    if (!SetEnvironmentVariableW(L"RECOMP_NV2A_SHADER_CACHE_DIR", temporary) ||
        !SetEnvironmentVariableW(L"RECOMP_NV2A_SHADER_CACHE", L"1")) {
        std::fprintf(stderr, "shader cache test environment failed: %lu\n", GetLastError());
        return 1;
    }
    Nv2aShaderCache cache;
    const char source[] =
        "float4 vs(float4 position:POSITION):SV_POSITION{return position+float4(VALUE,0,0,0);}"
        "float4 alternate(float4 position:POSITION):SV_POSITION{return position;}";
    D3D_SHADER_MACRO macros[] = {{"VALUE", "0"}, {nullptr, nullptr}};
    constexpr UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS;
    ComPtr<ID3DBlob> original, loaded, direct, errors;
    auto compile = [&](const char *text, const char *entry, const char *profile, UINT options, ComPtr<ID3DBlob> &output) {
        output.Reset(); errors.Reset();
        HRESULT result = cache.compile(text, std::strlen(text), "cache-test", macros,
                                       entry, profile, options, &output, &errors);
        check("valid shader compiles", SUCCEEDED(result));
        return SUCCEEDED(result);
    };
    bool first = compile(source, "vs", "vs_5_0", flags, original);
    check("cold request writes one entry", cache.stats.misses == 1 && cache.stats.writes == 1);
    if (first && compile(source, "vs", "vs_5_0", flags, loaded)) {
        check("identical request hits", cache.stats.hits == 1);
        check("cached bytecode is exact", original->GetBufferSize() == loaded->GetBufferSize() &&
              !std::memcmp(original->GetBufferPointer(), loaded->GetBufferPointer(), original->GetBufferSize()));
    }
    HRESULT result = D3DCompile(source, sizeof source - 1, "cache-test", macros, nullptr,
                               "vs", "vs_5_0", flags, 0, &direct, &errors);
    check("reference compiler succeeds", SUCCEEDED(result));
    if (first && direct)
        check("cache preserves compiler output", original->GetBufferSize() == direct->GetBufferSize() &&
              !std::memcmp(original->GetBufferPointer(), direct->GetBufferPointer(), direct->GetBufferSize()));
    fs::path entry;
    for (const auto &file : fs::directory_iterator(directory))
        if (file.path().extension() == L".nvsc") entry = file.path();
    check("entry exists", !entry.empty());
    if (!entry.empty()) {
        { std::ofstream broken(entry, std::ios::binary | std::ios::trunc); broken << "invalid"; }
        compile(source, "vs", "vs_5_0", flags, loaded);
        check("truncated entry is diagnosed and repaired", cache.stats.invalid == 1 && cache.stats.writes == 2);
        {
            std::fstream broken(entry, std::ios::binary | std::ios::in | std::ios::out);
            broken.seekg(-1, std::ios::end);
            char byte = 0; broken.get(byte);
            broken.seekp(-1, std::ios::end); broken.put(byte ^ 1);
        }
        compile(source, "vs", "vs_5_0", flags, loaded);
        check("bytecode checksum is verified", cache.stats.invalid == 2 && cache.stats.writes == 3);
        {
            std::fstream broken(entry, std::ios::binary | std::ios::in | std::ios::out);
            uint32_t oversized = UINT32_MAX;
            broken.seekp(12); broken.write(reinterpret_cast<const char *>(&oversized), sizeof oversized);
        }
        compile(source, "vs", "vs_5_0", flags, loaded);
        check("oversized bytecode is rejected before allocation", cache.stats.invalid == 3 && cache.stats.writes == 4);
        {
            std::ofstream broken(entry, std::ios::binary | std::ios::app); broken.put('x');
        }
        compile(source, "vs", "vs_5_0", flags, loaded);
        check("trailing bytes are rejected", cache.stats.invalid == 4 && cache.stats.writes == 5);
        {
            std::fstream broken(entry, std::ios::binary | std::ios::in | std::ios::out);
            broken.seekp(48); broken.put('\0');
        }
        compile(source, "vs", "vs_5_0", flags, loaded);
        check("stored complete request is verified", cache.stats.invalid == 5 && cache.stats.writes == 6);
    }
    uint64_t misses = cache.stats.misses;
    macros[0].Definition = "1";
    compile(source, "vs", "vs_5_0", flags, loaded);
    check("macro values invalidate cache", cache.stats.misses == ++misses);
    compile(source, "alternate", "vs_5_0", flags, loaded);
    check("entry point invalidates cache", cache.stats.misses == ++misses);
    compile(source, "vs", "vs_4_0", flags, loaded);
    check("profile invalidates cache", cache.stats.misses == ++misses);
    compile(source, "vs", "vs_5_0", D3DCOMPILE_IEEE_STRICTNESS, loaded);
    check("compiler flags invalidate cache", cache.stats.misses == ++misses);
    std::string changed = std::string(source) + "\n";
    compile(changed.c_str(), "vs", "vs_5_0", flags, loaded);
    check("complete source invalidates cache", cache.stats.misses == ++misses);
    Nv2aShaderCache persisted;
    persisted.compile(source, sizeof source - 1, "cache-test", macros, "vs", "vs_5_0", flags, &loaded, &errors);
    check("fresh cache instance loads persisted entry", persisted.stats.hits == 1 && persisted.stats.writes == 0);
    uint64_t writes = cache.stats.writes;
    errors.Reset(); loaded.Reset();
    result = cache.compile("invalid", 7, "cache-test", nullptr, "vs", "vs_5_0", flags, &loaded, &errors);
    check("compiler failure and diagnostics propagate", FAILED(result) && errors && !loaded);
    check("failed compilation is not persisted", cache.stats.writes == writes);
    SetEnvironmentVariableW(L"RECOMP_NV2A_SHADER_CACHE", L"0");
    Nv2aShaderCache disabled;
    loaded.Reset(); errors.Reset();
    result = disabled.compile(source, sizeof source - 1, "cache-test", macros, "vs", "vs_5_0", flags, &loaded, &errors);
    check("disabled cache compiles normally", SUCCEEDED(result) && disabled.stats.hits == 0 && disabled.stats.writes == 0);
    SetEnvironmentVariableW(L"RECOMP_NV2A_SHADER_CACHE", L"1");
    fs::path blocker = directory / L"blocker";
    { std::ofstream blocked(blocker); blocked << "not a directory"; }
    fs::path unavailable = blocker / L"cache";
    SetEnvironmentVariableW(L"RECOMP_NV2A_SHADER_CACHE_DIR", unavailable.c_str());
    Nv2aShaderCache failed;
    loaded.Reset(); errors.Reset();
    result = failed.compile(source, sizeof source - 1, "cache-test", macros, "vs", "vs_5_0", flags, &loaded, &errors);
    check("unavailable cache reports error but retains compilation", SUCCEEDED(result) && failed.stats.errors == 1);
    SetEnvironmentVariableW(L"RECOMP_NV2A_SHADER_CACHE_DIR", directory.c_str());
    fs::path full = directory / L"budget.nvsc";
    HANDLE file = CreateFileW(full.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD returned = 0;
    LARGE_INTEGER limit; limit.QuadPart = 256ll * 1024 * 1024;
    bool sparse = file != INVALID_HANDLE_VALUE &&
        DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) &&
        SetFilePointerEx(file, limit, nullptr, FILE_BEGIN) && SetEndOfFile(file);
    check("exact 256 MiB sparse budget fixture created", sparse);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (sparse) {
        Nv2aShaderCache full_cache;
        loaded.Reset(); errors.Reset();
        result = full_cache.compile(source, sizeof source - 1, "cache-test", macros,
                                    "vs", "vs_5_0", flags, &loaded, &errors);
        check("budget saturation retains existing hits", SUCCEEDED(result) && full_cache.stats.hits == 1);
        macros[0].Definition = "2";
        loaded.Reset(); errors.Reset();
        result = full_cache.compile(source, sizeof source - 1, "cache-test", macros,
                                    "vs", "vs_5_0", flags, &loaded, &errors);
        check("budget saturation compiles without growing cache",
              SUCCEEDED(result) && full_cache.stats.misses == 1 && full_cache.stats.writes == 0);
    }
    for (const auto &file : fs::directory_iterator(directory))
        check("test entry removed", fs::remove(file.path()));
    check("test directory removed", fs::remove(directory));
    std::printf("nv2a_shader_cache_smoke: %s\n", failures ? "FAILED" : "ALL PASS");
    return failures ? 1 : 0;
}
