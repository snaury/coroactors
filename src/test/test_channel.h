#pragma once
#include <coroactors/detail/async_task.h>
#include <coroactors/stop_token.h>
#include <coroactors/with_continuation.h>
#include <deque>
#include <mutex>

template<class T>
class test_channel {
public:
    class continuation : public coroactors::continuation<T> {
        using base_t = coroactors::continuation<T>;
    public:
        using base_t::base_t;

        continuation(base_t&& rhs, coroactors::stop_token t)
            : base_t(std::move(rhs))
            , stop_token_(std::move(t))
        {}

        const coroactors::stop_token& get_stop_token() const {
            return stop_token_;
        }

    private:
        coroactors::stop_token stop_token_;
    };

    auto get() {
        return coroactors::with_continuation<T>(
            [this](coroactors::continuation<T> c) {
                std::unique_lock l(lock);
                if (results.empty()) {
                    queue.push_back(continuation(std::move(c), coroactors::detail::current_stop_token()));
                } else {
                    // Note: we have not suspended yet, no risk of resuming
                    c.resume(std::move(results.front()));
                    results.pop_front();
                }
            });
    }

    void provide(T value) {
        std::unique_lock l(lock);
        if (queue.empty()) {
            results.push_back(std::move(value));
        } else {
            auto c = std::move(queue.front());
            queue.pop_front();
            l.unlock();
            c.resume(std::move(value));
        }
    }

    size_t awaiters() const {
        std::unique_lock l(lock);
        return queue.size();
    }

    continuation take() {
        std::unique_lock l(lock);
        assert(!queue.empty());
        auto c = std::move(queue.front());
        queue.pop_front();
        return c;
    }

    continuation take_at(size_t index) {
        std::unique_lock l(lock);
        assert(index <= queue.size());
        auto it = queue.begin() + index;
        auto c = std::move(*it);
        queue.erase(it);
        return c;
    }

    void resume(T value) {
        take().resume(std::move(value));
    }

    void resume_at(size_t index, T value) {
        take_at(index).resume(std::move(value));
    }

    template<class E>
    void resume_with_exception(E&& e) {
        take().resume_with_exception(std::forward<E>(e));
    }

private:
    mutable std::mutex lock;
    std::deque<T> results;
    std::deque<continuation> queue;
};
