#pragma once

namespace coroactors::detail {

    template<class TAwaitable>
    decltype(auto) get_awaiter(TAwaitable&& awaitable) {
        if constexpr (requires { ((TAwaitable&&) awaitable).operator co_await(); }) {
            return ((TAwaitable&&) awaitable).operator co_await();
        } else if constexpr (requires { operator co_await((TAwaitable&&) awaitable); }) {
            return operator co_await((TAwaitable&&) awaitable);
        } else {
            return ((TAwaitable&&) awaitable);
        }
    }

    template<class TAwaitable>
    using await_result_t = decltype(get_awaiter(std::declval<TAwaitable>()).await_resume());

} // namespace coroactors::detail
