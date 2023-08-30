#pragma once
#include <stdexcept>

namespace coroactors {

    /**
     * Exception thrown when task_group<T> is used incorrectly
     */
    class task_group_error : public std::logic_error {
    public:
        using std::logic_error::logic_error;
    };

} // namespace coroactors
