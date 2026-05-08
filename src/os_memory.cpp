#include "os_memory.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace FastAlloc {

void* OSMemory::AllocatePages(std::size_t size) {
#ifdef _WIN32
    return VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr == MAP_FAILED) {
        return nullptr;
    }
    return ptr;
#endif
}

void OSMemory::FreePages(void* ptr, std::size_t size) {
    if (!ptr) return;
#ifdef _WIN32
    (void)size; // VirtualFree with MEM_RELEASE requires size parameter to be 0
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    munmap(ptr, size);
#endif
}

std::size_t OSMemory::GetPageSize() {
    static const std::size_t page_size = []() {
        std::size_t size = 0;
#ifdef _WIN32
        SYSTEM_INFO sysInfo;
        GetSystemInfo(&sysInfo);
        size = sysInfo.dwPageSize;
#else
        size = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
#endif
        return (size > 0) ? size : 4096;
    }();
    return page_size;
}

} // namespace FastAlloc
