#pragma once
#include <coroactors/detail/task_local.h>
#include <memory>

namespace coroactors {

    /**
     * Similar to thread locals this class allows passing task locals across
     * nested async calls. Values are bound to the specific task local variable,
     * and this variable must outlive all possible value bindings.
     */
    template<class T>
    class task_local {
    public:
        template<class... Args>
        task_local(Args&&... args)
            : default_value(std::forward<Args>(args)...)
        {}

        task_local(const task_local&) = delete;
        task_local& operator=(const task_local&) = delete;

        /**
         * Returns a reference to a coroutine local value. This is either a
         * reference to the value passed to with_value, or a default value.
         */
        const T& get() const {
            const void* key = this;
            const void* value = detail::find_task_local(key);
            if (value) {
                return *reinterpret_cast<const T*>(value);
            } else {
                return default_value;
            }
        }

        /**
         * Runs the provided awaitable with a new task local value. Note that
         * the caller must guarantee the provided value reference outlives
         * the awaitable, e.g. temporaries may only be used when it is
         * co_awaited in the same expression.
         */
        template<detail::awaitable Awaitable>
        auto with_value(const T& value, Awaitable&& awaitable) const {
            const void* key = this;
            const void* ptr = std::addressof(value);
            return detail::with_task_local_impl(
                detail::async_task_local(key, ptr),
                std::forward<Awaitable>(awaitable));
        }

    private:
        T default_value;
    };

} // namespace coroactors
