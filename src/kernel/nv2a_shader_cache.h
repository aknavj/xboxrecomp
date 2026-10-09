#ifndef NV2A_SHADER_CACHE_H
#define NV2A_SHADER_CACHE_H

#include <windows.h>
#include <bcrypt.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

struct Nv2aShaderCacheStats {
    uint64_t hits = 0, misses = 0, writes = 0, invalid = 0, errors = 0;
};

class Nv2aShaderCache {
    using Blob = Microsoft::WRL::ComPtr<ID3DBlob>;
    using Path = std::filesystem::path;
    struct Header {
        std::array<uint8_t, 8> magic;
        uint32_t request_bytes, code_bytes;
        std::array<uint8_t, 32> checksum;
    };
    static_assert(sizeof(Header) == 48);
    static constexpr std::array<uint8_t, 8> magic = {'N', 'V', '2', 'A', 'S', 'H', 1, 0};
    static constexpr uint64_t budget = 256ull * 1024 * 1024;
    static constexpr uint32_t maximum_bytes = 16u * 1024 * 1024;
    bool initialized = false, enabled = false, budget_reported = false;
    Path directory;
    uint64_t stored_bytes = 0;

    void error(const char *operation, unsigned long code)
    {
        stats.errors++;
        std::fprintf(stderr, "[GPU-D3D11] shader disk cache %s failed: 0x%08lX\n", operation, code);
    }

    std::wstring environment(const wchar_t *name)
    {
        DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
        if (!size) return {};
        std::wstring value(size, L'\0');
        DWORD length = GetEnvironmentVariableW(name, value.data(), size);
        if (!length || length >= size) {
            error("read environment", length >= size ? ERROR_INSUFFICIENT_BUFFER : GetLastError());
            return {};
        }
        value.resize(length);
        return value;
    }

    void initialize()
    {
        if (initialized) return;
        initialized = true;
        if (environment(L"RECOMP_NV2A_SHADER_CACHE") == L"0") return;
        auto custom = environment(L"RECOMP_NV2A_SHADER_CACHE_DIR");
        if (!custom.empty()) directory = custom;
        else {
            auto local = environment(L"LOCALAPPDATA");
            if (local.empty()) {
                error("locate LOCALAPPDATA", ERROR_ENVVAR_NOT_FOUND);
                return;
            }
            directory = Path(local) / L"XboxRecomp" / L"ShaderCache" / L"v1";
        }
        if (directory.native().size() + 80 >= MAX_PATH) {
            error("directory length", ERROR_FILENAME_EXCED_RANGE);
            return;
        }
        std::error_code failure;
        std::filesystem::create_directories(directory, failure);
        if (failure) { error("create directory", failure.value()); return; }
        std::filesystem::directory_iterator entries(directory, failure), end;
        if (failure) { error("list directory", failure.value()); return; }
        while (entries != end) {
            const auto &entry = *entries;
            if (entry.path().extension() == L".nvsc") {
                uint64_t bytes = entry.file_size(failure);
                if (failure) { error("measure entry", failure.value()); return; }
                stored_bytes += bytes;
            }
            entries.increment(failure);
            if (failure) { error("list directory", failure.value()); return; }
        }
        enabled = true;
    }

    bool digest(const void *data, size_t bytes, std::array<uint8_t, 32> &hash)
    {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash_handle = nullptr;
        NTSTATUS status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        if (status >= 0) status = BCryptCreateHash(algorithm, &hash_handle, nullptr, 0, nullptr, 0, 0);
        if (status >= 0) status = BCryptHashData(hash_handle, const_cast<UCHAR *>(static_cast<const UCHAR *>(data)), (ULONG)bytes, 0);
        if (status >= 0) status = BCryptFinishHash(hash_handle, hash.data(), (ULONG)hash.size(), 0);
        if (hash_handle) BCryptDestroyHash(hash_handle);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        if (status < 0) { error("hash", (unsigned long)status); return false; }
        return true;
    }

    static void append(std::vector<uint8_t> &request, const void *data, size_t bytes)
    {
        uint32_t length = (uint32_t)bytes;
        const auto *prefix = reinterpret_cast<const uint8_t *>(&length);
        request.insert(request.end(), prefix, prefix + sizeof length);
        if (bytes) {
            const auto *source = static_cast<const uint8_t *>(data);
            request.insert(request.end(), source, source + bytes);
        }
    }

    static void append(std::vector<uint8_t> &request, const char *value)
    {
        uint32_t present = value != nullptr;
        append(request, &present, sizeof present);
        append(request, value, value ? std::strlen(value) : 0);
    }

    bool load(const Path &path, const std::vector<uint8_t> &request, Blob &code)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            std::error_code failure;
            bool exists = std::filesystem::exists(path, failure);
            if (failure || exists) error("open entry", failure ? failure.value() : ERROR_READ_FAULT);
            return false;
        }
        Header header = {};
        file.read(reinterpret_cast<char *>(&header), sizeof header);
        bool valid = file.good() && header.magic == magic && header.request_bytes == request.size() &&
            header.code_bytes && header.code_bytes <= maximum_bytes;
        std::vector<uint8_t> stored;
        if (valid) {
            stored.resize(header.request_bytes);
            file.read(reinterpret_cast<char *>(stored.data()), stored.size());
            valid = file.good() && stored == request;
        }
        if (valid) {
            HRESULT result = D3DCreateBlob(header.code_bytes, &code);
            if (FAILED(result)) { error("allocate bytecode", (unsigned long)result); return false; }
            file.read(static_cast<char *>(code->GetBufferPointer()), header.code_bytes);
            valid = file.good() && file.peek() == std::ifstream::traits_type::eof() && !file.bad();
            std::array<uint8_t, 32> checksum = {};
            valid = valid && digest(code->GetBufferPointer(), code->GetBufferSize(), checksum) && checksum == header.checksum;
        }
        if (!valid) {
            code.Reset();
            stats.invalid++;
            std::fprintf(stderr, "[GPU-D3D11] shader disk cache invalid entry; recompiling: %s\n", path.u8string().c_str());
        }
        return valid;
    }

    void store(const Path &path, const std::vector<uint8_t> &request, ID3DBlob *code)
    {
        if (request.size() > maximum_bytes || code->GetBufferSize() > maximum_bytes) {
            error("entry size", ERROR_FILE_TOO_LARGE);
            return;
        }
        std::error_code failure;
        uint64_t old_bytes = std::filesystem::file_size(path, failure);
        if (failure) {
            if (failure != std::errc::no_such_file_or_directory) { error("measure replaced entry", failure.value()); return; }
            old_bytes = 0;
        }
        uint64_t bytes = sizeof(Header) + request.size() + code->GetBufferSize();
        uint64_t retained = stored_bytes - (old_bytes <= stored_bytes ? old_bytes : 0);
        if (retained > budget || bytes > budget - retained) {
            if (!budget_reported) {
                std::fprintf(stderr, "[GPU-D3D11] shader disk cache reached 256 MiB; retaining hits without new writes\n");
                budget_reported = true;
            }
            return;
        }
        Header header = {magic, (uint32_t)request.size(), (uint32_t)code->GetBufferSize(), {}};
        if (!digest(code->GetBufferPointer(), code->GetBufferSize(), header.checksum)) return;
        wchar_t temporary[MAX_PATH];
        if (!GetTempFileNameW(directory.c_str(), L"nvs", 0, temporary)) {
            error("create temporary entry", GetLastError());
            return;
        }
        bool written;
        {
            std::ofstream file(Path(temporary), std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char *>(&header), sizeof header);
            file.write(reinterpret_cast<const char *>(request.data()), request.size());
            file.write(static_cast<const char *>(code->GetBufferPointer()), code->GetBufferSize());
            file.close();
            written = !file.fail();
        }
        if (!written) error("write entry", ERROR_WRITE_FAULT);
        else if (!MoveFileExW(temporary, path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            error("publish entry", GetLastError());
        else {
            stored_bytes = retained + bytes;
            stats.writes++;
            return;
        }
        if (!DeleteFileW(temporary)) error("remove temporary entry", GetLastError());
    }

public:
    Nv2aShaderCacheStats stats;

    HRESULT compile(const void *source, size_t bytes, const char *name, const D3D_SHADER_MACRO *macros,
                    const char *entry, const char *profile, UINT flags, ID3DBlob **code, ID3DBlob **errors)
    {
        initialize();
        Path path;
        std::vector<uint8_t> request;
        if (enabled && bytes <= maximum_bytes) {
            append(request, &flags, sizeof flags);
            uint32_t compiler = D3D_COMPILER_VERSION;
            append(request, &compiler, sizeof compiler);
            append(request, name); append(request, entry); append(request, profile);
            uint32_t macro_count = 0;
            if (macros) for (auto *macro = macros; macro->Name; macro++) macro_count++;
            append(request, &macro_count, sizeof macro_count);
            if (macros) for (auto *macro = macros; macro->Name; macro++) {
                append(request, macro->Name); append(request, macro->Definition);
            }
            append(request, source, bytes);
            std::array<uint8_t, 32> hash = {};
            if (request.size() <= maximum_bytes && digest(request.data(), request.size(), hash)) {
                char filename[70];
                for (size_t index = 0; index < hash.size(); index++)
                    std::snprintf(filename + index * 2, 3, "%02x", (unsigned)hash[index]);
                std::strcpy(filename + 64, ".nvsc");
                path = directory / filename;
                Blob cached;
                if (load(path, request, cached)) {
                    stats.hits++;
                    if (errors) *errors = nullptr;
                    return cached.CopyTo(code);
                }
                stats.misses++;
            }
        }
        HRESULT result = D3DCompile(source, bytes, name, macros, nullptr, entry, profile, flags, 0, code, errors);
        if (SUCCEEDED(result) && !path.empty()) store(path, request, *code);
        return result;
    }
};

#endif
