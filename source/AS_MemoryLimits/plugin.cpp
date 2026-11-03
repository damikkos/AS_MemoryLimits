#include "graft/native.hpp"
#include "graft/engine.hpp"
#include "MemoryLimits.hpp"
#include "EngineSpill.hpp"
#include <windows.h>
#include <format>
#include <stdexcept>

GRAFT_PLUGIN("AS_MemoryLimits", 4);

namespace {
memory_limits::EngineSpill spill;
memory_limits::EngineAllocate original_allocate{};

void* allocate(memory_limits::EnginePool* pool, std::uint32_t bytes) {
    if (pool != spill.pool) return original_allocate(pool, bytes);
    auto* result = spill.allocate(bytes, original_allocate);
    if (!result && bytes) {
        graft::log("[AS_MemoryLimits] EngineMB exhausted: allocation stopped before buffer overflow");
        RaiseException(0xc0000017, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    }
    return result;
}

memory_limits::EnginePool* existing_engine_pool(unsigned char* base) {
    HANDLE process = GetCurrentProcess();
    unsigned char* context{};
    memory_limits::EnginePool* pool{};
    memory_limits::EnginePool snapshot{};
    if (!ReadProcessMemory(process, base + memory_limits::engine_context_rva, &context, sizeof(context), nullptr))
        throw std::runtime_error("cannot read Engine context");
    if (!context) return nullptr;
    if (!ReadProcessMemory(process, context + 0x198, &pool, sizeof(pool), nullptr) || !pool ||
        !ReadProcessMemory(process, pool, &snapshot, sizeof(snapshot), nullptr))
        throw std::runtime_error("cannot read existing Engine pool");
    const auto begin = reinterpret_cast<std::uintptr_t>(snapshot.begin);
    const auto end = reinterpret_cast<std::uintptr_t>(snapshot.end);
    const auto cursor = reinterpret_cast<std::uintptr_t>(snapshot.cursor);
    if (snapshot.vtable != base + memory_limits::engine_vtable_rva || !snapshot.synchronized || snapshot.size != 8u * 1024u * 1024u ||
        !begin || end < begin || end - begin != snapshot.size || cursor < begin || cursor > end)
        throw std::runtime_error("unsupported existing Engine pool");
    std::array<void*, 12> table{};
    if (!ReadProcessMemory(process, static_cast<void**>(snapshot.vtable) - 1, table.data(), sizeof(table), nullptr) ||
        table[0] != base + memory_limits::engine_rtti_rva)
        throw std::runtime_error("unsupported Engine RTTI");
    for (std::size_t i = 0; i < memory_limits::engine_vtable_rvas.size(); ++i)
        if (table[i + 1] != base + memory_limits::engine_vtable_rvas[i])
            throw std::runtime_error("unsupported Engine virtual table");
    return pool;
}
} // namespace

GRAFT_ON_LOAD() {
    try {
        wchar_t path[32768]{};
        const auto length = GetModuleFileNameW(nullptr, path, 32768);
        if (!length || length >= 32768) throw std::runtime_error("cannot resolve server path");
        const std::filesystem::path exe(path);
        if (_wcsicmp(exe.filename().c_str(), L"DayZServer_x64.exe") != 0)
            throw std::runtime_error("only DayZServer_x64.exe is supported");
        memory_limits::verify_exe(exe);

        HMODULE self{};
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&graft_plugin_name_), &self))
            throw std::runtime_error("cannot resolve plugin module");
        const auto dll_length = GetModuleFileNameW(self, path, 32768);
        if (!dll_length || dll_length >= 32768) throw std::runtime_error("cannot resolve plugin path");
        const auto ini = std::filesystem::path(path).parent_path() / L"MemoryLimits.ini";
        if (memory_limits::create_default_ini(ini))
            graft::log("[AS_MemoryLimits] created MemoryLimits.ini next to plugin DLL");
        const auto limits = memory_limits::read_ini(ini);
        auto* base = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
        memory_limits::validate_image({base, memory_limits::image_size});
        auto* pool = existing_engine_pool(base);
        if (pool && limits[5] > 8) {
            original_allocate = reinterpret_cast<memory_limits::EngineAllocate>(base + memory_limits::engine_allocator_rva);
            spill.pool = pool;
            spill.capacity = static_cast<std::size_t>(limits[5] - 8) * 1024u * 1024u;
            spill.extra = static_cast<unsigned char*>(VirtualAlloc(nullptr, spill.capacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            if (!spill.extra) throw std::runtime_error("cannot allocate Engine extension");
            EnterCriticalSection(&pool->lock);
            bool patched = false;
            try {
                memory_limits::apply({base, memory_limits::image_size}, limits);
                patched = true;
                if (!spill.install_allocator(reinterpret_cast<void**>(base + memory_limits::engine_vtable_rva), reinterpret_cast<void*>(&allocate)))
                    throw std::runtime_error("Engine virtual table changed during installation");
            } catch (...) {
                if (patched) {
                    try { memory_limits::restore({base, memory_limits::image_size}, limits); }
                    catch (...) {
                        LeaveCriticalSection(&pool->lock);
                        VirtualFree(spill.extra, 0, MEM_RELEASE);
                        spill = {};
                        throw;
                    }
                }
                LeaveCriticalSection(&pool->lock);
                VirtualFree(spill.extra, 0, MEM_RELEASE);
                spill = {};
                throw;
            }
            LeaveCriticalSection(&pool->lock);
            // Existing interned strings retain their addresses; extension lives until process exit.
            graft::log(std::format("[AS_MemoryLimits] Engine pool: existing 8 + extension {} MiB; existing data preserved", limits[5] - 8));
            graft::log("[AS_MemoryLimits] private Engine allocator installed; shared Alloc/SetPos available to KR_MEMSTATS");
            graft::log("[AS_MemoryLimits] KR_MEMSTATS observes the primary Engine arena; the separate Engine extension is not included in its totals");
        } else {
            memory_limits::apply({base, memory_limits::image_size}, limits);
        }
        for (std::size_t i = 0; i < limits.size(); ++i) {
            const auto& site = memory_limits::sites[i];
            graft::log(std::format("[AS_MemoryLimits] {}: {} -> {} MiB, RVA {:#x}",
                                  memory_limits::key_name(site), site.vanilla_mb, limits[i], site.rva + 4));
        }
        graft::log("[AS_MemoryLimits] all six limits applied; EXE on disk unchanged");
    } catch (const std::exception& error) {
        graft::log(std::string("[AS_MemoryLimits] FAILED: ") + error.what());
    }
}
