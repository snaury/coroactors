#pragma once
#include <atomic>

namespace coroactors::detail {

#if __cpp_lib_atomic_lock_free_type_aliases	>= 201907L
    using semaphore_atomic_t = std::atomic_signed_lock_free;
#elif defined(__linux__)
    // Linux uses int for a futex
    using semaphore_atomic_t = std::atomic<int>;
#else
    // Asume other OSes use int64 for now
    using semaphore_atomic_t = std::atomic<int64_t>;
#endif

} // namespace coroactors::detail
