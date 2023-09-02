#pragma once
#include <stdexcept>

namespace coroactors {

    /**
     * Thrown when a continuation object is not used correctly
     */
    class with_continuation_error : public std::logic_error {
    public:
        using std::logic_error::logic_error;
    };

} // namespace coroactors
