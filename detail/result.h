#pragma once
#include <exception>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace coroactors::detail {

    /**
     * Encapsulates a result of computation which may fail with an exception
     */
    template<class T>
    class result {
    public:
        using value_type = T;

        void set_value()
            requires (std::is_void_v<T>)
        {
            result_.template emplace<1>();
        }

        template<class TArg>
        void set_value(TArg&& arg)
            requires (!std::is_void_v<T>)
        {
            result_.template emplace<1>(std::forward<TArg>(arg));
        }

        void set_exception(std::exception_ptr&& e) noexcept {
            result_.template emplace<2>(std::move(e));
        }

        explicit operator bool() const {
            return result_.index() != 0;
        }

        bool has_value() const noexcept {
            return result_.index() == 1;
        }

        bool has_exception() const noexcept {
            return result_.index() == 2;
        }

        std::add_lvalue_reference_t<T> get() {
            switch (result_.index()) {
                case 1: {
                    if constexpr (std::is_void_v<T>) {
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

        std::add_rvalue_reference_t<T> take() && {
            switch (result_.index()) {
                case 1: {
                    if constexpr (std::is_void_v<T>) {
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
        struct TVoid {};
        using TValue = std::conditional_t<std::is_void_v<T>, TVoid, T>;
        std::variant<std::monostate, TValue, std::exception_ptr> result_;
    };

} // namespace coroactors::detail
