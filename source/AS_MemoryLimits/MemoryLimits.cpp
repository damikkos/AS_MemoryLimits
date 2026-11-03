#include "MemoryLimits.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace memory_limits {
namespace {
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
} // namespace

std::string key_name(const Site& site) {
    std::string result;
    for (wchar_t c : std::wstring_view(site.key)) result += static_cast<char>(c);
    return result;
}

std::uint32_t parse_mb(std::wstring value, std::uint32_t minimum) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    require(first != std::wstring::npos, "empty memory limit");
    value = value.substr(first, value.find_last_not_of(L" \t\r\n") - first + 1);
    std::uint32_t result = 0;
    for (wchar_t c : value) {
        require(c >= L'0' && c <= L'9', "memory limits must be decimal integers");
        result = result * 10 + static_cast<unsigned>(c - L'0');
        require(result <= 1024, "memory limit exceeds 1024 MiB");
    }
    require(result >= minimum, "memory limit is below the vanilla minimum");
    return result;
}

bool create_default_ini(const std::filesystem::path& path) {
    std::string text = "[MemoryLimits]\r\n";
    for (const auto& site : sites)
        text += key_name(site) + " = " + std::to_string(site.default_mb) + "\r\n";
    const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_EXISTS) return false;
        throw std::runtime_error("cannot create MemoryLimits.ini next to plugin DLL");
    }
    DWORD written{};
    const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                    written == text.size();
    CloseHandle(file);
    if (!ok) DeleteFileW(path.c_str());
    require(ok, "cannot write default MemoryLimits.ini");
    return true;
}

Limits read_ini(const std::filesystem::path& path) {
    require(std::filesystem::is_regular_file(path), "MemoryLimits.ini is missing");
    Limits result{};
    for (std::size_t i = 0; i < sites.size(); ++i) {
        wchar_t text[128]{};
        const auto count = GetPrivateProfileStringW(L"MemoryLimits", sites[i].key, L"", text, 128, path.c_str());
        require(count < 127, "INI value is too long");
        try {
            result[i] = parse_mb(text, sites[i].vanilla_mb);
        } catch (const std::exception& error) {
            throw std::runtime_error(key_name(sites[i]) + ": " + error.what());
        }
    }
    return result;
}

void verify_exe(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    require(input.good(), "cannot open server EXE");
    require(input.tellg() == static_cast<std::streamoff>(file_size), "unsupported server EXE size (expected DayZ Server 1.30.164289.70)");
    std::vector<unsigned char> bytes(file_size);
    input.seekg(0);
    require(static_cast<bool>(input.read(reinterpret_cast<char*>(bytes.data()), bytes.size())), "cannot read server EXE");
    BCRYPT_ALG_HANDLE algorithm{};
    require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "SHA-256 unavailable");
    std::array<unsigned char, 32> digest{};
    const auto status = BCryptHash(algorithm, nullptr, 0, bytes.data(), static_cast<ULONG>(bytes.size()),
                                   digest.data(), static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    require(status >= 0, "SHA-256 failed");
    std::string hex;
    for (auto byte : digest) {
        hex += "0123456789abcdef"[byte >> 4];
        hex += "0123456789abcdef"[byte & 15];
    }
    require(hex == server_sha256, "unsupported EXE SHA-256 (updated or already patched); no memory changed");
}

void validate_image(std::span<const unsigned char> image) {
    require(image.size() == image_size, "unsupported image size");
    IMAGE_DOS_HEADER dos{};
    std::memcpy(&dos, image.data(), sizeof(dos));
    require(dos.e_magic == IMAGE_DOS_SIGNATURE && dos.e_lfanew > 0 &&
            static_cast<std::size_t>(dos.e_lfanew) <= image.size() - sizeof(IMAGE_NT_HEADERS64), "invalid PE header");
    IMAGE_NT_HEADERS64 nt{};
    std::memcpy(&nt, image.data() + dos.e_lfanew, sizeof(nt));
    require(nt.Signature == IMAGE_NT_SIGNATURE && nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
            nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC &&
            nt.FileHeader.TimeDateStamp == timestamp && nt.OptionalHeader.SizeOfImage == image_size,
            "unsupported server build");
    for (const auto& site : sites) {
        if (std::memcmp(image.data() + site.rva, site.original.data(), site.original.size()) != 0)
            throw std::runtime_error(key_name(site) + ": instruction mismatch or another memory patch is active");
    }
    // The shared allocator belongs to observers; this plugin only replaces the Engine instance's virtual slot.
}

namespace {
void write_limits(std::span<unsigned char> image, const Limits& limits) {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    struct Page { unsigned char* at; DWORD previous; };
    std::vector<Page> pages;
    for (const auto& site : sites) {
        auto address = reinterpret_cast<std::uintptr_t>(image.data() + site.rva + 4);
        auto* at = reinterpret_cast<unsigned char*>(address - address % system.dwPageSize);
        if (std::none_of(pages.begin(), pages.end(), [at](const auto& p) { return p.at == at; }))
            pages.push_back({at, 0});
    }
    std::size_t protected_count = 0;
    for (; protected_count < pages.size(); ++protected_count) {
        auto& page = pages[protected_count];
        if (!VirtualProtect(page.at, system.dwPageSize, PAGE_EXECUTE_READWRITE, &page.previous)) break;
    }
    const bool writable = protected_count == pages.size();
    bool flushed = false;
    if (writable) {
        for (std::size_t i = 0; i < sites.size(); ++i) {
            const auto bytes = limits[i] * 1024u * 1024u;
            std::memcpy(image.data() + sites[i].rva + 4, &bytes, sizeof(bytes));
        }
        flushed = FlushInstructionCache(GetCurrentProcess(), image.data(), image.size()) != FALSE;
        if (!flushed) {
            for (const auto& site : sites)
                std::memcpy(image.data() + site.rva + 4, site.original.data() + 4, 4);
            FlushInstructionCache(GetCurrentProcess(), image.data(), image.size());
        }
    }
    bool restored = true;
    for (std::size_t i = 0; i < protected_count; ++i) {
        DWORD ignored{};
        if (!VirtualProtect(pages[i].at, system.dwPageSize, pages[i].previous, &ignored)) restored = false;
    }
    require(restored, "CRITICAL: could not restore page protection");
    require(writable, "VirtualProtect failed; no memory changed");
    require(flushed, "FlushInstructionCache failed; original limits restored");
}
} // namespace

void apply(std::span<unsigned char> image, const Limits& limits) {
    validate_image(image);
    for (std::size_t i = 0; i < sites.size(); ++i)
        require(limits[i] >= sites[i].vanilla_mb && limits[i] <= 1024, "invalid memory limit");
    write_limits(image, limits);
}

void restore(std::span<unsigned char> image, const Limits& limits) {
    require(image.size() == image_size, "invalid image for rollback");
    Limits vanilla{};
    for (std::size_t i = 0; i < sites.size(); ++i) {
        auto expected = sites[i].original;
        const auto bytes = limits[i] * 1024u * 1024u;
        std::memcpy(expected.data() + 4, &bytes, sizeof(bytes));
        require(std::memcmp(image.data() + sites[i].rva, expected.data(), expected.size()) == 0,
                "rollback refused: patch changed by another writer");
        vanilla[i] = sites[i].vanilla_mb;
    }
    write_limits(image, vanilla);
}
} // namespace memory_limits
