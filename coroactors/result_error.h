#pragma once
#include <stdexcept>

namespace coroactors {

    /**
     * Exception thrown when result<T> is used incorrectly
     */
    class result_error : public std::logic_error {
    public:
        result_error(const char* message)
            : logic_error(message)
        {}
    };

} // namespace coroactors
