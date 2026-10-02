# FastAlloc audit suite (evidence-backed audit, 2026-09-30).
# 49 gtest cases in 5 files: functional boundaries, property-based,
# differential vs glibc, concurrency, slab/OOM/fragmentation.
add_executable(fast_alloc_audit_tests
    test_audit_functional.cpp
    test_audit_property.cpp
    test_audit_differential.cpp
    test_audit_concurrency.cpp
    test_audit_slab_oom_frag.cpp
    ../sanitizer_defaults.cpp
)
target_compile_options(fast_alloc_audit_tests PRIVATE ${WARNING_FLAGS})
target_link_libraries(fast_alloc_audit_tests PRIVATE fast_alloc GTest::gtest_main)
fastalloc_stage_asan_runtime(fast_alloc_audit_tests)

include(CTest)
# Register the audit suite for ctest in every configuration EXCEPT
# FASTALLOC_DEBUG. Its heavy property/concurrency tiers are calibrated for
# release semantics; under the debug registry they run orders of magnitude
# slower with no added assurance (debug semantics are already covered by the
# validate suite's 11 debug-only cases and the debug-semantics fuzz campaign).
# Gating also keeps `ctest` in a debug build from reporting the audit entry
# as "Not Run" when only fast_alloc_tests was built (target-scoped builds).
# The binary itself still builds in all configurations if you want to run it
# directly.
if(NOT FASTALLOC_DEBUG)
    add_test(NAME fast_alloc_audit_tests COMMAND fast_alloc_audit_tests)
endif()
