#pragma once
#include <coroactors/detail/awaiters.h>
#include <coroactors/detail/config.h>
#include <coroutine>

#pragma once
#ifndef coroactors_use_std_stop_token
#include <version>
#if __has_include(<stop_token>) && defined(__cpp_lib_jthread)
#define coroactors_use_std_stop_token 1
#else
#define coroactors_use_std_stop_token 0
#endif
#endif

#if coroactors_use_std_stop_token
#include <stop_token>

namespace coroactors::detail {

    using std::nostopstate_t;
    using std::nostopstate;

    using std::stop_token;
    using std::stop_source;
    using std::stop_callback;

} // namespace coroactors::detail
#else
#include <coroactors/detail/stop_token_polyfill.h>
#endif
