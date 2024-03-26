#pragma once
#include <utility>

namespace coroactors::detail {

    namespace tag_invoke_polyfill {
        // dummy function that never matches
        void tag_invoke();

        template<class Tag, class... Args>
        concept tag_invocable = requires(Tag tag, Args&&... args) {
            tag_invoke((Tag&&) tag, (Args&&) args...);
        };

        template<class Tag, class... Args>
        concept nothrow_tag_invocable = requires(Tag tag, Args&&... args) {
            { tag_invoke((Tag&&) tag, (Args&&) args...) } noexcept;
        };

        template<class Tag, class... Args>
        using tag_invoke_result_t = decltype(tag_invoke(std::declval<Tag>(), std::declval<Args>()...));

        struct tag_invoke_t {
            template<class Tag, class... Args>
                requires tag_invocable<Tag, Args...>
            constexpr auto operator()(Tag tag, Args&&... args) const
                noexcept(nothrow_tag_invocable<Tag, Args...>)
                -> tag_invoke_result_t<Tag, Args...>
            {
                return tag_invoke((Tag&&) tag, (Args&&) args...);
            }
        };
    }

    using tag_invoke_polyfill::tag_invoke_t;

    namespace tag_invoke_polyfill_cpo {
        inline constexpr tag_invoke_t tag_invoke{};
    }

    using namespace tag_invoke_polyfill_cpo;

    template<auto& tag>
    using tag_t = std::decay_t<decltype(tag)>;

    using tag_invoke_polyfill::tag_invocable;
    using tag_invoke_polyfill::nothrow_tag_invocable;
    using tag_invoke_polyfill::tag_invoke_result_t;

} // namespace coroactors::detail
