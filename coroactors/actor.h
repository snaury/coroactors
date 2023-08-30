#pragma once
#include <coroactors/detail/actor.h>

namespace coroactors {

    /**
     * Used as the return type of actor coroutines
     *
     * Actor coroutines are eagerly started, but must either return the result,
     * or co_await context() first. When bound to a context they will continue
     * executing on that context, automatically releasing it on co_awaits and
     * reacquiring it before they return.
     */
    template<class T>
    class [[nodiscard]] actor {
        friend class detail::actor_promise<T>;

    private:
        explicit actor(detail::actor_continuation<T> handle) noexcept
            : handle(handle)
        {}

    public:
        using promise_type = detail::actor_promise<T>;
        using result_type = class result<T>;
        using value_type = T;

    public:
        actor() noexcept = default;

        actor(actor&& rhs) noexcept
            : handle(std::exchange(rhs.handle, {}))
        {}

        ~actor() noexcept {
            if (handle) {
                handle.destroy();
            }
        }

        actor& operator=(actor&& rhs) noexcept {
            if (this != &rhs) {
                auto prev = std::exchange(handle, {});
                handle = std::exchange(rhs.handle, {});
                if (prev) {
                    prev.destroy();
                }
            }
            return *this;
        }

        explicit operator bool() const noexcept {
            return bool(handle);
        }

        auto operator co_await() && noexcept {
            return detail::actor_awaiter<T>(std::exchange(handle, {}));
        }

        auto result() && noexcept {
            return detail::actor_result_awaiter<T>(std::exchange(handle, {}));
        }

        void detach() && noexcept {
            auto& p = handle.promise();
            handle = {};
            return p.start_detached();
        }

    private:
        detail::actor_continuation<T> handle;
    };

    static_assert(std::is_same_v<
        detail::awaitable_unwrap_awaiter_type<actor<int>, detail::actor_promise<int>>,
        detail::actor_awaiter<int>>);

} // namespace coroactors
