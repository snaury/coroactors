#pragma once
#include <coroactors/detail/local.h>
#include <memory>

namespace coroactors {

    /**
     * Similar to thread locals this class allows passing coroutine locals
     * across nested coroutine calls. Values are bound to the specific
     * coroutine local variable, and this variable must outlive all possible
     * value bindings.
     */
    template<class T>
    class coroutine_local {
    public:
        template<class... Args>
        coroutine_local(Args&&... args)
            : default_value(std::forward<Args>(args)...)
        {}

        coroutine_local(const coroutine_local&) = delete;
        coroutine_local& operator=(const coroutine_local&) = delete;

        /**
         * Returns a reference to a coroutine local value. This is either a
         * reference to the value passed to with_value, or a default value.
         */
        const T& get() const {
            const void* key = this;
            const void* ptr = detail::get_coroutine_local(key);
            if (ptr) {
                return *reinterpret_cast<const T*>(ptr);
            } else {
                return default_value;
            }
        }

        /**
         * Runs the provided awaitable with a new coroutine local value. Note
         * that the caller must guarantee the provided value reference outlives
         * the awaitable, e.g. temporaries may only be used when it is
         * co_awaited in the same expression.
         */
        template<detail::awaitable Awaitable>
        auto with_value(const T& value, Awaitable&& awaitable) const {
            const void* key = this;
            const void* ptr = std::addressof(value);
            return detail::with_coroutine_local_awaiter<Awaitable>(key, ptr,
                std::forward<Awaitable>(awaitable));
        }

    private:
        T default_value;
    };

} // namespace coroactors
