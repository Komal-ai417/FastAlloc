// ============================================================================
// Sanitizer default options for the TEST BINARIES ONLY (never the library).
// ----------------------------------------------------------------------------
// ASan / TSan / LSan replace the libc allocator, and by default they ABORT
// the process on an overflowing calloc/malloc instead of returning nullptr
// (allocator_may_return_null=0). The differential tests deliberately probe
// libc's documented "overflow fails cleanly" contract with e.g.
// std::calloc(SIZE_MAX, 3) -- under a sanitizer runtime that probe must
// behave like libc, i.e. return nullptr, or the test binary dies before it
// can assert anything.
//
// These weak-symbol hooks are the runtimes' documented extension point: they
// supply DEFAULTS for flags the environment does not set, so an explicit
// ASAN_OPTIONS/TSAN_OPTIONS/LSAN_OPTIONS on the command line still wins
// (per-flag precedence: env > *_default_options() > compiled-in defaults).
// Defining them unconditionally is safe: in a non-sanitized build they are
// ordinary unused extern functions the linker discards.
// ============================================================================
extern "C" const char* __asan_default_options() {
    // libc-compatible failure mode for huge/overflowing requests instead of
    // a hard abort (also what the audit evidence methodology used).
    return "allocator_may_return_null=1";
}

extern "C" const char* __tsan_default_options() {
    return "allocator_may_return_null=1";
}

extern "C" const char* __lsan_default_options() {
    return "allocator_may_return_null=1";
}
