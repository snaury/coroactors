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
