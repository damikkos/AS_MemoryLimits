#include "../MemoryLimits.hpp"
#include "../EngineSpill.hpp"
#include <windows.h>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <set>
#include <stdexcept>

using namespace memory_limits;
namespace {
EngineSpill* test_extension{};
unsigned observed_allocations{};
void* observed_allocate(EnginePool* pool, std::uint32_t bytes) {
    ++observed_allocations;
    auto* result = reinterpret_cast<unsigned char*>((reinterpret_cast<std::uintptr_t>(pool->cursor) + 7) & ~std::uintptr_t{7});
    pool->cursor = result + bytes;
    return result;
}
void* private_allocate(EnginePool*, std::uint32_t bytes) {
    return test_extension->allocate(bytes, observed_allocate);
}
}
template<class F> void rejects(F fn) {
    bool failed = false;
    try { fn(); } catch (const std::exception&) { failed = true; }
    assert(failed);
}

int wmain(int argc, wchar_t** argv) {
    {
        alignas(8) unsigned char primary[16] = "old-data", extra[32]{};
        int rtti{};
        std::array<void*, 12> canonical{};
        canonical.fill(&rtti);
        canonical[8] = reinterpret_cast<void*>(&observed_allocate);
        const auto original_table = canonical;
        EnginePool pool{}, other{};
        pool.vtable = other.vtable = canonical.data() + 1;
        pool.begin = primary;
        pool.cursor = primary + 8;
        pool.end = primary + sizeof(primary);
        InitializeCriticalSection(&pool.lock);
        EngineSpill extension{&pool, extra, sizeof(extra), 0};
        test_extension = &extension;
        assert(extension.install_allocator(canonical.data() + 1, reinterpret_cast<void*>(&private_allocate)));
        auto** table = static_cast<void**>(pool.vtable);
        assert(table[-1] == &rtti && table[7] == reinterpret_cast<void*>(&private_allocate));
        for (std::size_t i = 0; i < 11; ++i)
            if (i != 7) assert(table[i] == canonical[i + 1]);
        assert(canonical == original_table && other.vtable == canonical.data() + 1);
        assert(!extension.install_allocator(canonical.data() + 1, reinterpret_cast<void*>(&private_allocate)));
        rejects([&] {
            extension.allocate(1, [](EnginePool*, std::uint32_t) -> void* {
                throw std::runtime_error("observer failure");
            });
        });
        bool unlocked = false;
        std::thread check_lock([&] {
            unlocked = TryEnterCriticalSection(&pool.lock) != FALSE;
            if (unlocked) LeaveCriticalSection(&pool.lock);
        });
        check_lock.join();
        assert(unlocked && pool.cursor == primary + 8 && extension.used == 0);
        const auto allocate = reinterpret_cast<EngineAllocate>(table[7]);
        assert(allocate(&pool, 5) == primary + 8 && observed_allocations == 1);
        assert(allocate(&pool, 4) == extra && observed_allocations == 1);
        assert(std::memcmp(primary, "old-data", 8) == 0);
        assert(pool.begin == primary && pool.cursor == primary + 13);
        pool.vtable = canonical.data() + 1;
        test_extension = nullptr;
        DeleteCriticalSection(&pool.lock);
    }
    {
        alignas(8) unsigned char primary[16] = "old-data";
        alignas(8) unsigned char extra[32]{};
        EnginePool pool{};
        pool.begin = primary;
        pool.cursor = primary + 8;
        pool.end = primary + sizeof(primary);
        InitializeCriticalSection(&pool.lock);
        EngineSpill extension{&pool, extra, sizeof(extra), 0};
        assert(extension.allocate(5) == primary + 8);
        assert(extension.allocate(4) == extra);
        assert(extension.allocate(3) == extra + 8);
        const auto used = extension.used;
        assert(!extension.allocate(30) && extension.used == used);
        assert(!extension.allocate(UINT32_MAX) && extension.used == used);
        assert(!extension.allocate(0));
        assert(std::memcmp(primary, "old-data", 8) == 0);
        DeleteCriticalSection(&pool.lock);
    }
    {
        alignas(8) unsigned char primary[1024]{}, extra[1024]{};
        EnginePool pool{};
        pool.begin = pool.cursor = primary;
        pool.end = primary + sizeof(primary);
        InitializeCriticalSection(&pool.lock);
        EngineSpill extension{&pool, extra, sizeof(extra), 0};
        std::mutex lock;
        std::set<void*> allocations;
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t) threads.emplace_back([&] {
            for (int n = 0; n < 8; ++n) {
                auto* pointer = extension.allocate(32);
                assert(pointer);
                std::memset(pointer, 0x42, 32);
                std::lock_guard held(lock);
                assert(allocations.insert(pointer).second);
            }
        });
        for (auto& thread : threads) thread.join();
        assert(allocations.size() == 64 && !extension.allocate(1));
        DeleteCriticalSection(&pool.lock);
    }
    assert(parse_mb(L" 128\t", 32) == 128);
    assert(parse_mb(L"1024", 32) == 1024);
    for (const auto* invalid : {L"", L" ", L"0", L"31", L"-1", L"+128", L"128MB", L"128.0",
                               L"128;comment", L"1025", L"9999999999999999999999999"})
        rejects([&] { parse_mb(invalid, 32); });

    const auto temp = std::filesystem::temp_directory_path() /
                      (L"as_memory_limits_" + std::to_wstring(GetCurrentProcessId()));
    assert(std::filesystem::create_directory(temp));
    const auto ini = temp / L"MemoryLimits.ini";
    const auto wrong_exe = temp / L"wrong.exe";
    { std::ofstream output(wrong_exe, std::ios::binary); output << "MZ"; }
    rejects([&] { verify_exe(wrong_exe); });
    rejects([&] { read_ini(ini); });
    rejects([&] { create_default_ini(temp / L"missing_directory" / L"MemoryLimits.ini"); });
    assert(create_default_ini(ini));
    assert(!create_default_ini(ini));
    const auto defaults = read_ini(ini);
    assert((defaults == Limits{64,64,64,16,8,32}));
    assert(WritePrivateProfileStringW(L"MemoryLimits", L"WorldMB", L"-1", ini.c_str()));
    rejects([&] { read_ini(ini); });
    assert(WritePrivateProfileStringW(L"MemoryLimits", L"WorldMB", nullptr, ini.c_str()));
    rejects([&] { read_ini(ini); });
    assert(WritePrivateProfileStringW(L"MemoryLimits", L"WorldMB", L"96", ini.c_str()));
    assert(!create_default_ini(ini));
    assert(read_ini(ini)[1] == 96);
    assert(WritePrivateProfileStringW(L"MemoryLimits", L"EngineMB", std::wstring(200, L'9').c_str(), ini.c_str()));
    rejects([&] { read_ini(ini); });

    auto* data = static_cast<unsigned char*>(VirtualAlloc(nullptr, image_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    assert(data);
    std::span<unsigned char> image(data, image_size);
    if (argc == 2) {
        verify_exe(argv[1]);
        std::filesystem::copy_file(argv[1], wrong_exe, std::filesystem::copy_options::overwrite_existing);
        {
            std::fstream changed(wrong_exe, std::ios::in | std::ios::out | std::ios::binary);
            changed.seekp(0xa58e11); // World immediate in the audited 1.30.164289.70 EXE file.
            const std::uint32_t world64 = 64u * 1024u * 1024u;
            changed.write(reinterpret_cast<const char*>(&world64), sizeof(world64));
        }
        rejects([&] { verify_exe(wrong_exe); });
        std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
        std::vector<unsigned char> raw(static_cast<std::size_t>(input.tellg()));
        input.seekg(0);
        assert(input.read(reinterpret_cast<char*>(raw.data()), raw.size()));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(raw.data());
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(raw.data() + dos->e_lfanew);
        std::memcpy(data, raw.data(), nt->OptionalHeader.SizeOfHeaders);
        const auto* section = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            assert(section[i].PointerToRawData + section[i].SizeOfRawData <= raw.size());
            assert(section[i].VirtualAddress + section[i].SizeOfRawData <= image.size());
            std::memcpy(data + section[i].VirtualAddress, raw.data() + section[i].PointerToRawData, section[i].SizeOfRawData);
        }
        const auto preferred_base = nt->OptionalHeader.ImageBase;
        std::uint64_t pointer{};
        std::memcpy(&pointer, data + engine_vtable_rva - sizeof(pointer), sizeof(pointer));
        assert(pointer == preferred_base + engine_rtti_rva);
        for (std::size_t i = 0; i < engine_vtable_rvas.size(); ++i) {
            std::memcpy(&pointer, data + engine_vtable_rva + i * sizeof(pointer), sizeof(pointer));
            assert(pointer == preferred_base + engine_vtable_rvas[i]);
        }
        assert(engine_vtable_rvas[7] == engine_allocator_rva);
        std::cout << "Audited real server SHA-256, all six instruction sites, Engine RTTI and 11 virtual slots.\n";
    } else {
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(data);
        dos->e_magic = IMAGE_DOS_SIGNATURE;
        dos->e_lfanew = 0x100;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(data + dos->e_lfanew);
        nt->Signature = IMAGE_NT_SIGNATURE;
        nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
        nt->FileHeader.TimeDateStamp = timestamp;
        nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        nt->OptionalHeader.SizeOfImage = image_size;
        for (const auto& site : sites) std::memcpy(data + site.rva, site.original.data(), site.original.size());
    }
    validate_image(image);
    // A GRAFT observer may already own the shared allocator; our six sites remain guarded.
    const auto allocator_byte = data[engine_allocator_rva];
    data[engine_allocator_rva] = 0xe9;
    validate_image(image);
    data[engine_allocator_rva] = allocator_byte;
    const std::vector<unsigned char> original(data, data + image_size);
    data[sites.back().rva] ^= 1;
    rejects([&] { memory_limits::apply(image, defaults); });
    data[sites.back().rva] ^= 1;
    assert(std::memcmp(data, original.data(), image_size) == 0);
    auto invalid = defaults;
    invalid.back() = 0;
    rejects([&] { memory_limits::apply(image, invalid); });
    assert(std::memcmp(data, original.data(), image_size) == 0);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(data);
    const auto saved_offset = dos->e_lfanew;
    dos->e_lfanew = -1;
    rejects([&] { validate_image(image); });
    dos->e_lfanew = saved_offset;
    rejects([&] { validate_image(image.first(64)); });

    DWORD ignored{};
    assert(VirtualProtect(data, image_size, PAGE_EXECUTE_READ, &ignored));
    const Limits custom{64,256,96,4,2,32};
    memory_limits::apply(image, custom);
    auto expected = original;
    for (std::size_t i = 0; i < sites.size(); ++i) {
        const auto bytes = custom[i] * 1024u * 1024u;
        std::memcpy(expected.data() + sites[i].rva + 4, &bytes, sizeof(bytes));
        MEMORY_BASIC_INFORMATION info{};
        assert(VirtualQuery(data + sites[i].rva, &info, sizeof(info)));
        assert(info.Protect == PAGE_EXECUTE_READ);
    }
    assert(std::memcmp(data, expected.data(), image_size) == 0);
    rejects([&] { memory_limits::apply(image, defaults); });
    assert(std::memcmp(data, expected.data(), image_size) == 0);
    memory_limits::restore(image, custom);
    assert(std::memcmp(data, original.data(), image_size) == 0);
    assert(VirtualFree(data, 0, MEM_RELEASE));
    assert(std::filesystem::remove(ini));
    assert(std::filesystem::remove(wrong_exe));
    assert(std::filesystem::remove(temp));
    std::cout << "PASS: INI, build guards, conflict rejection, six limits, exact writes, rollback, page protection, private Engine vtable and RTTI, primary observer, pointer preservation, alignment, capacity and concurrent allocations.\n";
}
