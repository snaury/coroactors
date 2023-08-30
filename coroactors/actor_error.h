#pragma once
#include <stdexcept>

namespace coroactors {

    /**
     * Exception thrown when actor<T> is used incorrectly
     */
    class actor_error : public std::logic_error {
    public:
        using std::logic_error::logic_error;
    };

} // namespace coroactors
