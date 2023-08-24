#pragma once

#ifndef coroactors_use_std_stop_token
#if __has_include(<stop_token>)
#define coroactors_use_std_stop_token 1
#else
#define coroactors_use_std_stop_token 0
#endif
#endif

#if coroactors_use_std_stop_token
#include <stop_token>

namespace coroactors {

    using std::nostopstate_t;
    using std::nostopstate;

    using std::stop_token;
    using std::stop_source;
    using std::stop_callback;

} // namespace coroactors
#else
#include <coroactors/detail/stop_token_polyfill.h>

namespace coroactors {

    using detail::nostopstate_t;
    using detail::nostopstate;

    using detail::stop_token;
    using detail::stop_source;
    using detail::stop_callback;

} // namespace coroactors
#endif
