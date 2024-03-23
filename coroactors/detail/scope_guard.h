#pragma once
#include <type_traits>

namespace coroactors::detail {

    /**
     * Runs the specified callback at scope exit unless cancelled
     */
    template<class Callback>
    class scope_guard {
    public:
        template<class... Args>
        explicit scope_guard(Args&&... args)
            : callback(std::forward<Args>(args)...)
        {}

        scope_guard(const scope_guard&) = delete;
        scope_guard& operator=(const scope_guard&) = delete;

        ~scope_guard() {
            if (!cancelled) {
                callback();
            }
        }

        void cancel() {
            cancelled = true;
        }

    private:
        Callback callback;
        bool cancelled = false;
    };

    template<class Callback>
    scope_guard(Callback&&) -> scope_guard<std::decay_t<Callback>>;

} // namespace coroactors::detail
