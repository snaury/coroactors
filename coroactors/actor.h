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
        using is_actor_passthru_awaitable = void;
        using promise_type = detail::actor_promise<T>;
        using result_type = detail::result<T>;
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
            return detail::actor_awaiter(std::exchange(handle, {}));
        }

        auto result() && noexcept {
            return detail::actor_result_awaiter(std::exchange(handle, {}));
        }

        void detach() && noexcept {
            auto& p = handle.promise();
            handle = {};
            return p.detach();
        }

    private:
        detail::actor_continuation<T> handle;
    };

} // namespace coroactors
