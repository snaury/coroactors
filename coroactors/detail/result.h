#pragma once
#include <exception>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace coroactors::detail {

    class result_error : public std::logic_error {
    public:
        result_error(const char* message)
            : logic_error(message)
        {}
    };

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

        template<class Value>
        void set_value(Value&& value)
            requires (!std::is_void_v<T> && std::is_convertible_v<Value&&, T>)
        {
            result_.template emplace<1>(std::forward<Value>(value));
        }

        template<class... Args>
        void emplace_value(Args&&... args) {
            result_.template emplace<1>(std::forward<Args>(args)...);
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

        std::add_lvalue_reference_t<T> get_value() {
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
            throw result_error("result has neither value nor exception");
        }

        std::add_lvalue_reference_t<const T> get_value() const {
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
            throw result_error("result has neither value nor exception");
        }

        std::add_rvalue_reference_t<T> take_value() {
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
            throw result_error("result has neither value nor exception");
        }

        std::exception_ptr get_exception() const {
            if (result_.index() != 2) {
                [[unlikely]];
                throw result_error("result does not have an exception");
            }
            return std::get<2>(result_);
        }

        std::exception_ptr take_exception() {
            if (result_.index() != 2) {
                [[unlikely]];
                throw result_error("result does not have an exception");
            }
            return std::get<2>(std::move(result_));
        }

    private:
        struct Void {};
        using Result = std::conditional_t<std::is_void_v<T>, Void, T>;
        std::variant<std::monostate, Result, std::exception_ptr> result_;
    };

} // namespace coroactors::detail
