#pragma once
#include "slab.h"
#include "fast_alloc_config.h"
#include <array>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace FastAlloc {

class TLSCache {
public:
    static TLSCache& Get();

    void* AllocateBlock(std::size_t class_index);
    void DeallocateBlock(std::size_t class_index, FreeBlock* block);

    // Destructor must be public for the FLS/pthread cleanup callbacks
    ~TLSCache();

private:
    TLSCache() = default;

    std::array<FreeBlock*, NUM_SIZE_CLASSES> fast_bins_{};
    std::array<std::size_t, NUM_SIZE_CLASSES> counts_{};

    // MEMORY OPTIMIZATION: Prevent memory hoarding.
    // Scale the cache limit inversely to the block size.
    static std::size_t GetMaxCacheSize(std::size_t class_index) {
        std::size_t size = ClassIndexToSize(class_index);
        if (size <= 256) return 256;  // Tiny objects: Cache aggressively 
        if (size <= 4096) return 64;  // Medium objects: Cache moderately
        return 8;                     // Large objects (>4K): Keep cache tiny
    }

    // Platform-specific TLS key
    // Using FlsAlloc (Windows) / pthread_key_create (Linux) avoids the
    // C++ thread_local loader-lock deadlock on Windows/MinGW.
#ifdef _WIN32
    static DWORD tls_key_;
#else
    static pthread_key_t tls_key_;
    static void TlsDestructor(void* ptr);
#endif
    static void InitTlsKey();
};

} // namespace FastAlloc
