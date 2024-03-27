#pragma once

/**
 * Prevents functions from being inlined
 */
#ifndef COROACTORS_NOINLINE
    #if defined(_MSC_VER)
        #define COROACTORS_NOINLINE __declspec(noinline)
    #else
        #define COROACTORS_NOINLINE __attribute__((__noinline__))
    #endif
#endif

/**
 * Compilers may spill local variables into a coroutine frame when inlining
 * `await_suspend`, and this attribute forces them to not be inlined. Should
 * be used when coroutine may be destroyed or resumed before await_suspend
 * returns (e.g. when it is potentially passed to other threads).
 */
#ifndef COROACTORS_AWAIT_SUSPEND
#define COROACTORS_AWAIT_SUSPEND COROACTORS_NOINLINE
#endif

/**
 * When enabled coroactors will use compiler support for symmetric transfer, or
 * symmetric transfer will be emulated otherwise. Note that many compilers
 * either don't implement symmetric transfer in all modes (e.g. gcc), or
 * implement it incorrectly which cause double free bugs (e.g. msvc).
 */
#ifndef COROACTORS_USE_SYMMETRIC_TRANSFER
#define COROACTORS_USE_SYMMETRIC_TRANSFER 0
#endif
