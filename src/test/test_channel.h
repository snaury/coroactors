#include <coroactors/with_continuation.h>
#include <deque>
#include <mutex>

template<class T>
class test_channel {
public:
    auto get() {
        return coroactors::with_continuation<T>([this](coroactors::continuation<T> c) {
            std::unique_lock l(lock);
            if (results.empty()) {
                queue.push_back(std::move(c));
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

    coroactors::continuation<T> take() {
        std::unique_lock l(lock);
        assert(!queue.empty());
        auto c = std::move(queue.front());
        queue.pop_front();
        return c;
    }

    coroactors::continuation<T> take_at(size_t index) {
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
    std::deque<coroactors::continuation<T>> queue;
};
