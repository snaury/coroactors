#pragma once
#include <variant>
#include <exception>
#include <stdexcept>

namespace coroactors::detail {

    struct void_t {};

    template<class T>
    struct maybe_void_impl { using type = T; };

    template<class T>
    struct lvalue_impl { using type = T&; };

    template<class T>
    struct rvalue_impl { using type = T&&; };

    template<>
    struct maybe_void_impl<void> { using type = void_t; };

    template<>
    struct lvalue_impl<void> { using type = void; };

    template<>
    struct rvalue_impl<void> { using type = void; };

    template<class T>
    using maybe_void = typename maybe_void_impl<T>::type;

    template<class T>
    using lvalue = typename lvalue_impl<T>::type;

    template<class T>
    using rvalue = typename rvalue_impl<T>::type;

} // namespace coroactors::detail

namespace coroactors {

    /**
     * Encapsulates an actor call result
     */
    template<class T>
    class actor_result {
    public:
        using value_type = T;

        void set_value()
            requires (std::same_as<T, void>)
        {
            result_.template emplace<1>();
        }

        template<class TArg>
        void set_value(TArg&& arg)
            requires (!std::same_as<T, void>)
        {
            result_.template emplace<1>(std::forward<TArg>(arg));
        }

        void set_exception(std::exception_ptr&& e) noexcept {
            result_.template emplace<2>(std::move(e));
        }

        bool has_value() const noexcept {
            return result_.index() == 1;
        }

        bool has_exception() const noexcept {
            return result_.index() == 2;
        }

        detail::lvalue<T> get() {
            switch (result_.index()) {
                case 1: {
                    if constexpr (std::same_as<T, void>) {
                        return;
                    } else {
                        return std::get<1>(result_);
                    }
                }
                case 2: {
                    std::rethrow_exception(std::get<2>(result_));
                }
            }
            throw std::logic_error("result has no value");
        }

        detail::rvalue<T> take() && {
            switch (result_.index()) {
                case 1: {
                    if constexpr (std::same_as<T, void>) {
                        return;
                    } else {
                        return std::get<1>(std::move(result_));
                    }
                }
                case 2: {
                    std::rethrow_exception(std::get<2>(std::move(result_)));
                }
            }
            throw std::logic_error("result has no value");
        }

        std::exception_ptr get_exception() const {
            return std::get<2>(result_);
        }

        std::exception_ptr take_exception() && {
            return std::get<2>(std::move(result_));
        }

    private:
        std::variant<std::monostate, detail::maybe_void<T>, std::exception_ptr> result_;
    };

} // namespace coroactors
