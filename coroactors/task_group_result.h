#pragma once
#include <coroactors/result.h>
#include <coroactors/task_group_error.h>

namespace coroactors {

    /**
     * Encapsulates a single result in a task group
     *
     * In addition to being a result<T> it also contains a task index.
     */
    template<class T>
    class task_group_result : public result<T> {
        static constexpr size_t index_not_set = size_t(-1);

    public:
        task_group_result() noexcept = default;

        size_t index() const noexcept {
            return index_;
        }

        void set_index(size_t index) noexcept {
            index_ = index;
        }

    private:
        size_t index_{ index_not_set };
    };

} // namespace coroactors
