#pragma once

// -----------------------------------------------------------------------------------------
// Put small, long-lived bookkeeping in PSRAM instead of the internal heap.
//
// ⛔ WHY THIS EXISTS - measured on Jake's colour T-Deck, 2026-09-29. The internal heap is
// the scarce thing on this device (~40KB free after boot, and Wi-Fi, TLS, Bluetooth and every
// task stack MUST come from it). ESP-IDF's malloc() puts anything under 4KB there first, so a
// container with one small node per mesh node quietly eats it: opening Maps with ~245 nodes
// took 27KB of internal heap in one go and never gave it back, and hopping between apps
// afterwards walked it down to 4KB until the device froze. PSRAM has megabytes free.
//
// Use it for per-node / per-packet containers that nothing touches from an interrupt or while
// the flash cache is off. That is every UI-side container here - they are only ever used
// from ordinary tasks.
//
//   std::unordered_map<K, V, std::hash<K>, std::equal_to<K>, PsramAllocator<std::pair<const K, V>>>
//   or just   PsramUnorderedMap<K, V>
//
// Falls back to the normal heap when PSRAM is absent or full, so it can never make an
// allocation fail that would have succeeded before. heap_caps_free() releases either kind.
// -----------------------------------------------------------------------------------------

#include <cstddef>
#include <cstdlib>
#include <functional>
#include <new>
#include <unordered_map>
#include <utility>

#if defined(ARDUINO_ARCH_ESP32) && defined(BOARD_HAS_PSRAM)
#include <esp_heap_caps.h>
#define TUI_HAVE_PSRAM_ALLOC 1
#endif

inline void *tui_psram_malloc(size_t n)
{
#ifdef TUI_HAVE_PSRAM_ALLOC
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p)
        return p;
#endif
    return malloc(n);
}

inline void tui_psram_free(void *p)
{
#ifdef TUI_HAVE_PSRAM_ALLOC
    heap_caps_free(p); // frees PSRAM and internal blocks alike
#else
    free(p);
#endif
}

template <class T> struct PsramAllocator {
    using value_type = T;
    PsramAllocator() noexcept {}
    template <class U> PsramAllocator(const PsramAllocator<U> &) noexcept {}
    T *allocate(size_t n)
    {
        void *p = tui_psram_malloc(n * sizeof(T));
        if (!p)
            std::abort(); // same outcome as std::allocator running dry, without needing exceptions
        return static_cast<T *>(p);
    }
    void deallocate(T *p, size_t) noexcept { tui_psram_free(p); }
    template <class U> bool operator==(const PsramAllocator<U> &) const noexcept { return true; }
    template <class U> bool operator!=(const PsramAllocator<U> &) const noexcept { return false; }
};

template <class K, class V>
using PsramUnorderedMap = std::unordered_map<K, V, std::hash<K>, std::equal_to<K>, PsramAllocator<std::pair<const K, V>>>;

// For a class allocated one at a time with `new`: put this inside the class body.
#define TUI_PSRAM_NEW_DELETE                                                                                           \
    static void *operator new(size_t n)                                                                                \
    {                                                                                                                  \
        void *p = tui_psram_malloc(n);                                                                                 \
        if (!p)                                                                                                        \
            std::abort();                                                                                              \
        return p;                                                                                                      \
    }                                                                                                                  \
    static void operator delete(void *p) noexcept { tui_psram_free(p); }
