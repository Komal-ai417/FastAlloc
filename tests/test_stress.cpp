// ============================================================================
// Randomized stress / soak test with full pattern verification.
// Deterministic (fixed seed): every allocation is filled with a
// position-dependent pattern and re-verified before free. Any routing,
// double-handout or metadata-corruption bug shows up here.
//
// Run under ASan/TSan/UBSan in CI (see .github/workflows/ci.yml).
// ============================================================================
#include <gtest/gtest.h>
#include "fast_alloc.h"
#include "fast_alloc_config.h"

#include <vector>
#include <random>
#include <cstring>
#include <cstdint>
#include <thread>

using namespace FastAlloc;

namespace {

constexpr unsigned char PatternByte(std::size_t seed, std::size_t i) {
    return static_cast<unsigned char>((seed * 2654435761u + i * 40503u) >> 8);
}

struct LiveBlock {
    void* ptr;
    std::size_t size;
    std::size_t seed;
};

void StressRound(unsigned base_seed, int ops, std::size_t max_live,
                 std::size_t max_size, int realloc_prob) {
    std::mt19937 rng(base_seed);
    std::vector<LiveBlock> live;
    live.reserve(max_live);

    for (int op = 0; op < ops; ++op) {
        int action = static_cast<int>(rng() % 100);
        if (live.size() >= max_live) action = 90; // force free/verify
        if (live.empty()) action = 0;             // must allocate

        if (action < 60) {
            std::size_t s = 1 + rng() % max_size;
            void* p = fast_malloc(s);
            ASSERT_NE(p, nullptr);
            std::size_t seed = rng();
            unsigned char* c = static_cast<unsigned char*>(p);
            for (std::size_t i = 0; i < s; ++i) c[i] = PatternByte(seed, i);
            live.push_back({p, s, seed});
        } else if (action < 60 + realloc_prob) {
            std::size_t idx = rng() % live.size();
            LiveBlock& b = live[idx];
            std::size_t ns = 1 + rng() % max_size;
            void* np = fast_realloc(b.ptr, ns);
            ASSERT_NE(np, nullptr);
            // Old prefix must survive.
            unsigned char* c = static_cast<unsigned char*>(np);
            std::size_t check = b.size < ns ? b.size : ns;
            for (std::size_t i = 0; i < check; ++i) {
                ASSERT_EQ(c[i], PatternByte(b.seed, i))
                    << "realloc pattern broken at " << i;
            }
            std::size_t nseed = rng();
            for (std::size_t i = 0; i < ns; ++i) c[i] = PatternByte(nseed, i);
            b.ptr = np; b.size = ns; b.seed = nseed;
        } else {
            std::size_t idx = rng() % live.size();
            LiveBlock b = live[idx];
            live[idx] = live.back();
            live.pop_back();
            // Verify the FULL pattern before freeing.
            unsigned char* c = static_cast<unsigned char*>(b.ptr);
            for (std::size_t i = 0; i < b.size; ++i) {
                ASSERT_EQ(c[i], PatternByte(b.seed, i))
                    << "block corrupted before free at offset " << i;
            }
            fast_free(b.ptr);
        }
    }
    for (LiveBlock b : live) fast_free(b.ptr);
}

} // namespace

TEST(StressTest, SmallBlockChurn) {
    StressRound(0xDEADBEEF, 30000, 512, 256, 0);
}

TEST(StressTest, MixedClasses) {
    StressRound(0x5EED1, 30000, 1024, 8176, 0);
}

TEST(StressTest, WithRealloc) {
    StressRound(0xABCDEF, 20000, 512, 8176, 20);
}

TEST(StressTest, LargeBlockChurn) {
    StressRound(0xFEED2, 3000, 64, 2 * 1024 * 1024, 10);
}

TEST(StressTest, MultithreadedSoak) {
    // Multiple threads running independent pattern-verified churns. Each
    // thread uses its own storage (no cross-thread sharing here - the
    // dedicated cross-thread tests cover that).
    const int kThreads = 4;
    std::atomic<int> failures{0};

    auto worker = [&](unsigned seed) {
        std::mt19937 rng(seed);
        std::vector<LiveBlock> live;
        live.reserve(256);
        for (int op = 0; op < 20000; ++op) {
            int action = static_cast<int>(rng() % 100);
            if (live.size() >= 256) action = 90;
            if (live.empty()) action = 0;
            if (action < 60) {
                std::size_t s = 1 + rng() % 8176;
                void* p = fast_malloc(s);
                if (!p) { ++failures; continue; }
                std::size_t sd = rng();
                unsigned char* c = static_cast<unsigned char*>(p);
                for (std::size_t i = 0; i < s; ++i) c[i] = PatternByte(sd, i);
                live.push_back({p, s, sd});
            } else {
                std::size_t idx = rng() % live.size();
                LiveBlock b = live[idx];
                live[idx] = live.back();
                live.pop_back();
                unsigned char* c = static_cast<unsigned char*>(b.ptr);
                bool ok = true;
                for (std::size_t i = 0; i < b.size; ++i) {
                    if (c[i] != PatternByte(b.seed, i)) { ok = false; break; }
                }
                if (!ok) ++failures;
                fast_free(b.ptr);
            }
        }
        for (LiveBlock b : live) fast_free(b.ptr);
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back(worker, 0x1000u + static_cast<unsigned>(i));
    }
    for (auto& t : threads) t.join();

    EXPECT_EQ(failures.load(), 0);
}

TEST(StressTest, FreeInReverseOrder) {
    // LIFO free pattern: entire slabs empty at once, exercising the
    // "slab becomes empty -> return to OS" path heavily.
    std::vector<void*> ptrs;
    for (int i = 0; i < 20000; ++i) {
        void* p = fast_malloc(64);
        ASSERT_NE(p, nullptr);
        *static_cast<uint64_t*>(p) = i;
        ptrs.push_back(p);
    }
    for (std::size_t i = ptrs.size(); i-- > 0;) {
        fast_free(ptrs[i]);
    }
}

TEST(StressTest, InterleavedFreeOrder) {
    // Round-robin free pattern: slabs drain gradually.
    std::vector<void*> ptrs;
    for (int i = 0; i < 10000; ++i) {
        void* p = fast_malloc(128);
        ASSERT_NE(p, nullptr);
        ptrs.push_back(p);
    }
    for (std::size_t i = 0; i < ptrs.size(); ++i) {
        fast_free(ptrs[(i * 7 + 3) % ptrs.size()]);
    }
}
