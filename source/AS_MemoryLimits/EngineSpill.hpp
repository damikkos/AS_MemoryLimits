#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <array>
#include <cstring>

namespace memory_limits {
// Layout verified in Server 1.30.164289.70: interned strings must never move.
struct EnginePool {
    void* vtable;
    std::uint32_t references, reserved;
    unsigned char* cursor;
    unsigned char* end;
    unsigned char* begin;
    std::uint32_t size;
    bool synchronized;
    unsigned char padding[19];
    CRITICAL_SECTION lock;
};
static_assert(offsetof(EnginePool, cursor) == 0x10);
static_assert(offsetof(EnginePool, size) == 0x28);
static_assert(offsetof(EnginePool, lock) == 0x40);
static_assert(sizeof(EnginePool) == 0x68);
using EngineAllocate = void* (*)(EnginePool*, std::uint32_t);

struct EngineSpill {
    EnginePool* pool{};
    unsigned char* extra{};
    std::size_t capacity{}, used{};
    // shortcut: 11 audited virtual slots plus RTTI; re-audit for another server build.
    std::array<void*, 12> dispatch{};

    bool install_allocator(void** original, void* replacement) noexcept {
        if (pool->vtable != original) return false;
        std::memcpy(dispatch.data(), original - 1, sizeof(dispatch));
        dispatch[8] = replacement;
        return InterlockedCompareExchangePointer(&pool->vtable, dispatch.data() + 1, original) == original;
    }

    void* allocate(std::uint32_t bytes, EngineAllocate original = nullptr) {
        if (!bytes) return nullptr;
        EnterCriticalSection(&pool->lock);
        struct Unlock {
            CRITICAL_SECTION* lock;
            ~Unlock() { LeaveCriticalSection(lock); }
        } unlock{&pool->lock};
        void* result{};
        const auto aligned = (reinterpret_cast<std::uintptr_t>(pool->cursor) + 7) & ~std::uintptr_t{7};
        const auto end = reinterpret_cast<std::uintptr_t>(pool->end);
        if (aligned <= end && bytes <= end - aligned) {
            // Keep observers on the shared allocator informed about the primary arena.
            if (original) return original(pool, bytes);
            result = reinterpret_cast<void*>(aligned);
            pool->cursor = reinterpret_cast<unsigned char*>(aligned + bytes);
        } else {
            const auto offset = (used + 7) & ~std::size_t{7};
            if (offset <= capacity && bytes <= capacity - offset) {
                result = extra + offset;
                used = offset + bytes;
            }
        }
        return result;
    }
};
} // namespace memory_limits
