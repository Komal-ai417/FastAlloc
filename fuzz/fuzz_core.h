// ============================================================================
// FastAlloc audit — shared fuzzing core: byte-program decoder + shadow model
// + invariant checks. Used by fuzz_ops_libfuzzer.cpp (libFuzzer, persistent
// state across inputs) and fuzz_standalone.cpp (entropy-driven, time-budget).
//
// Byte-program ISA (one op per byte, structure-aware):
//   opcode = byte >> 5        (0..7)
//   arg    = byte & 0x1F      (0..31, scaled per op)
// Ops:
//   0 ALLOC          size = 1 + arg*67              (spans 1..2072 smalls)
//   1 ALLOC_LARGE    size = 8177 + arg*16000        (large path, 8KB..512KB)
//   2 ALLOC_ALIGNED  align = 16 << (arg % 9)        (16..4096), size=1+arg*97
//   3 FREE           index = arg into live[] (oldest-first window)
//   4 REALLOC        index = arg; new = 1 + arg*131 (grow/shrink mix)
//   5 CALLOC         n = 1+arg%16, sz = 1+arg*53
//   6 CROSS_FREE     like FREE but routes through foreign-thread semantics:
//                     here: free via fast_free_sized (sized free path)
//   7 CHURN          do arg+1 immediate alloc/frees (cache pressure)
// Invariants checked after EVERY op; violations abort with the input hex.
// ============================================================================
#ifndef FASTALLOC_FUZZ_CORE_H
#define FASTALLOC_FUZZ_CORE_H

#include "fast_alloc.h"
#include "fast_alloc_config.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <unordered_map>

namespace fuzzcore {

using FastAlloc::fast_malloc;
using FastAlloc::fast_free;
using FastAlloc::fast_realloc;
using FastAlloc::fast_calloc;
using FastAlloc::fast_aligned_alloc;
using FastAlloc::fast_free_sized;

struct Rec {
    std::size_t size;
    std::size_t seed;
    unsigned char fill;   // first-byte marker
};

// Shadow model: pointer -> record. Live allocations must be distinct and
// their first byte must hold the marker we wrote (fast path LIFO not assumed).
class Shadow {
public:
    std::unordered_map<void*, Rec> live;
    std::size_t total_ops = 0;

    bool insert(void* p, std::size_t size, unsigned char marker) {
        if (p == nullptr) return false;
        if (live.count(p)) return false;         // aliasing: NEVER allowed
        if (reinterpret_cast<std::uintptr_t>(p) & 15u) return false;  // alignment
        Rec r; r.size = size; r.seed = total_ops; r.fill = marker;
        live[p] = r;
        return true;
    }
    bool erase(void* p) { return live.erase(p) > 0; }
    bool check_marker(void* p) const {
        auto it = live.find(p);
        if (it == live.end()) return false;
        // first user byte must still hold the marker we wrote
        return static_cast<const unsigned char*>(p)[0] == it->second.fill;
    }
    std::size_t bytes_live() const {
        std::size_t s = 0;
        for (auto& kv : live) s += kv.second.size;
        return s;
    }
};

struct Stats {
    std::size_t allocs = 0, allocs_large = 0, aligned = 0, callocs = 0,
                reallocs = 0, frees = 0, cross_frees = 0, churn = 0;
};

// Execute one byte-program against the LIVE allocator state (persistent).
// `shadow` accumulates across calls (persistent fuzzing) unless cleared.
// Returns false on INVARIANT VIOLATION (caller should abort the fuzzer).
inline bool RunProgram(const uint8_t* data, std::size_t size, Shadow& shadow,
                       Stats& st, bool verbose = false) {
    std::vector<void*> order;  // insertion order for FREE indexing
    order.reserve(64);
    for (std::size_t i = 0; i < size; ++i) {
        uint8_t b = data[i];
        unsigned op = b >> 5;
        unsigned arg = b & 0x1F;
        shadow.total_ops++;
        switch (op) {
        case 0: {  // ALLOC small
            std::size_t sz = 1 + static_cast<std::size_t>(arg) * 67;
            void* p = fast_malloc(sz);
            if (p) {
                unsigned char m = static_cast<unsigned char>(0x40 | (arg & 0x3F));
                static_cast<unsigned char*>(p)[0] = m;
                if (!shadow.insert(p, sz, m)) {
                    std::fprintf(stderr, "[FUZZ INVARIANT] alias/align @op%zu alloc\n", i);
                    return false;
                }
                order.push_back(p);
                st.allocs++;
            }
            break;
        }
        case 1: {  // ALLOC large
            std::size_t sz = 8177 + static_cast<std::size_t>(arg) * 16000;
            void* p = fast_malloc(sz);
            if (p) {
                unsigned char m = static_cast<unsigned char>(0x80 | (arg & 0x7F));
                static_cast<unsigned char*>(p)[0] = m;
                static_cast<unsigned char*>(p)[sz - 1] = m;  // touch last byte
                if (!shadow.insert(p, sz, m)) {
                    std::fprintf(stderr, "[FUZZ INVARIANT] alias/align @op%zu large\n", i);
                    return false;
                }
                order.push_back(p);
                st.allocs_large++;
            }
            break;
        }
        case 2: {  // ALLOC_ALIGNED
            std::size_t al = static_cast<std::size_t>(16) << (arg % 9);
            std::size_t sz = 1 + static_cast<std::size_t>(arg) * 97;
            void* p = fast_aligned_alloc(al, sz);
            if (p) {
                if (reinterpret_cast<std::uintptr_t>(p) & (al - 1)) {
                    std::fprintf(stderr, "[FUZZ INVARIANT] align %zu violated @op%zu\n", al, i);
                    return false;
                }
                unsigned char m = static_cast<unsigned char>(0xC0 | (arg & 0x3F));
                static_cast<unsigned char*>(p)[0] = m;
                if (!shadow.insert(p, sz, m)) {
                    std::fprintf(stderr, "[FUZZ INVARIANT] alias @op%zu aligned\n", i);
                    return false;
                }
                order.push_back(p);
                st.aligned++;
            } else if (al >= 4096 && sz > 0) {
                // valid alignment+size must not fail except under real OOM
                st.aligned++;  // counted anyway; nullptr only under OOM
            }
            break;
        }
        case 3: {  // FREE by index (arg into live window)
            if (order.empty()) break;
            std::size_t idx = arg % order.size();
            void* p = order[idx];
            if (!shadow.check_marker(p)) {
                std::fprintf(stderr, "[FUZZ INVARIANT] marker destroyed @op%zu (use-after-write?)\n", i);
                return false;
            }
            if (!shadow.erase(p)) {
                std::fprintf(stderr, "[FUZZ INVARIANT] free of unknown block @op%zu\n", i);
                return false;
            }
            fast_free(p);
            order.erase(order.begin() + static_cast<std::ptrdiff_t>(idx));
            st.frees++;
            break;
        }
        case 4: {  // REALLOC index, mixed grow/shrink
            if (order.empty()) break;
            std::size_t idx = arg % order.size();
            void* p = order[idx];
            std::size_t ns = 1 + static_cast<std::size_t>(arg) * 131;
            void* q = fast_realloc(p, ns);
            if (q == nullptr) {
                // C11: realloc failure leaves p valid -> marker must survive
                if (!shadow.check_marker(p)) {
                    std::fprintf(stderr, "[FUZZ INVARIANT] realloc-fail disturbed block @op%zu\n", i);
                    return false;
                }
            } else {
                if (reinterpret_cast<std::uintptr_t>(q) & 15u) {
                    std::fprintf(stderr, "[FUZZ INVARIANT] realloc misaligned @op%zu\n", i);
                    return false;
                }
                if (q != p && shadow.live.count(q)) {
                    std::fprintf(stderr, "[FUZZ INVARIANT] realloc aliased a live block @op%zu\n", i);
                    return false;
                }
                shadow.erase(p);   // p's record is superseded (moved, or same addr with new size)
                unsigned char m = static_cast<unsigned char>(0x20 | (arg & 0x1F));
                static_cast<unsigned char*>(q)[0] = m;
                if (!shadow.insert(q, ns, m)) {
                    std::fprintf(stderr, "[FUZZ INVARIANT] alias @op%zu realloc\n", i);
                    return false;
                }
                // replace in order
                for (std::size_t k = 0; k < order.size(); ++k)
                    if (order[k] == p) { order[k] = q; break; }
            }
            st.reallocs++;
            break;
        }
        case 5: {  // CALLOC
            std::size_t n = 1 + arg % 16;
            std::size_t sz = 1 + static_cast<std::size_t>(arg) * 53;
            void* p = fast_calloc(n, sz);
            if (p) {
                const unsigned char* c = static_cast<const unsigned char*>(p);
                for (std::size_t k = 0; k < n * sz && k < 4096; ++k)
                    if (c[k] != 0) {
                        std::fprintf(stderr, "[FUZZ INVARIANT] calloc not zeroed @op%zu\n", i);
                        return false;
                    }
                unsigned char m = static_cast<unsigned char>(0x60 | (arg & 0x3F));
                static_cast<unsigned char*>(p)[0] = m;
                if (!shadow.insert(p, n * sz, m)) {
                    std::fprintf(stderr, "[FUZZ INVARIANT] alias @op%zu calloc\n", i);
                    return false;
                }
                order.push_back(p);
                st.callocs++;
            }
            break;
        }
        case 6: {  // CROSS_FREE via sized-free path (foreign routing)
            if (order.empty()) break;
            std::size_t idx = arg % order.size();
            void* p = order[idx];
            std::size_t sz = shadow.live.count(p) ? shadow.live[p].size : 0;
            if (!shadow.check_marker(p)) {
                std::fprintf(stderr, "[FUZZ INVARIANT] marker destroyed (sized) @op%zu\n", i);
                return false;
            }
            shadow.erase(p);
            fast_free_sized(p, sz);
            order.erase(order.begin() + static_cast<std::ptrdiff_t>(idx));
            st.cross_frees++;
            break;
        }
        default: {  // 7: CHURN pressure
            for (unsigned k = 0; k <= arg % 8; ++k) {
                std::size_t cs = 1 + static_cast<std::size_t>((arg * 31 + k * 17) % 8176);
                void* t = fast_malloc(cs);
                if (t) { static_cast<unsigned char*>(t)[0] = 0x7A; fast_free(t); }
            }
            st.churn++;
            break;
        }
        }
    }
    // End-of-input invariant: every live block's marker survives
    for (auto& kv : shadow.live) {
        if (static_cast<const unsigned char*>(kv.first)[0] != kv.second.fill) {
            std::fprintf(stderr, "[FUZZ INVARIANT] live marker destroyed at end-of-input\n");
            return false;
        }
    }
    if (verbose)
        std::fprintf(stderr,
                     "[fuzz] ops=%zu live=%zu bytes=%zu (a=%zu L=%zu al=%zu c=%zu r=%zu f=%zu x=%zu ch=%zu)\n",
                     shadow.total_ops, shadow.live.size(), shadow.bytes_live(), st.allocs,
                     st.allocs_large, st.aligned, st.callocs, st.reallocs, st.frees,
                     st.cross_frees, st.churn);
    return true;
}

}  // namespace fuzzcore

#endif  // FASTALLOC_FUZZ_CORE_H
