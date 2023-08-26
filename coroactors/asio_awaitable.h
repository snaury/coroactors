#pragma once
#include <coroactors/detail/asio_awaitable.h>

namespace coroactors {

    inline constexpr asio_awaitable_t<> asio_awaitable{};

} // namespace coroactors
