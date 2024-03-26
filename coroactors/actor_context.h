#pragma once
#include <coroactors/actor_context_base.h>
#include <coroactors/detail/actor_sleep_until.h>
#include <coroactors/detail/actor_with_deadline.h>

namespace coroactors {

    inline auto actor_context::sleep_until(actor_scheduler::time_point deadline) const {
        return detail::sleep_until_awaiter(
            ptr ? &ptr->scheduler : nullptr,
            deadline);
    }

    inline auto actor_context::sleep_for(actor_scheduler::duration timeout) const {
        return sleep_until(actor_scheduler::clock_type::now() + timeout);
    }

    template<detail::awaitable Awaitable>
    inline auto actor_context::with_deadline(
            actor_scheduler::time_point deadline,
            Awaitable&& awaitable) const
    {
        return detail::with_deadline_impl(
            ptr ? &ptr->scheduler : nullptr,
            deadline,
            std::forward<Awaitable>(awaitable));
    }

    template<detail::awaitable Awaitable>
    inline auto actor_context::with_timeout(
            actor_scheduler::duration timeout,
            Awaitable&& awaitable) const
    {
        return with_deadline(
            actor_scheduler::clock_type::now() + timeout,
            std::forward<Awaitable>(awaitable));
    }

} // namespace coroactors
