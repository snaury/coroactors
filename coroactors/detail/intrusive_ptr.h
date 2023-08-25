#pragma once
#include <memory>

namespace coroactors::detail {

    template<class T>
    concept intrusive_object = requires(T* obj) {
        // We can call add_ref
        { obj->add_ref() } noexcept;
        // We can call release_ref and compare result to zero
        { obj->release_ref() == 0 } noexcept;
    };

    /**
     * An intrusive pointer where refcount is managed by the object
     */
    template<intrusive_object T, class Deleter = std::default_delete<T>>
    class intrusive_ptr {
    public:
        intrusive_ptr() noexcept
            : data()
        {}

        intrusive_ptr(std::nullptr_t) noexcept
            : data()
        {}

        explicit intrusive_ptr(T* p) noexcept
            : data()
        {
            ptr() = p;
            if (p) {
                p->add_ref();
            }
        }

        template<class D>
        explicit intrusive_ptr(T* p, D&& d) noexcept
            : data(p, std::forward<D>(d))
        {
            if (p) {
                p->add_ref();
            }
        }

        ~intrusive_ptr() {
            if (T* p = ptr(); p && p->release_ref() == 0) {
                deleter()(p);
            }
        }

        intrusive_ptr(const intrusive_ptr& rhs) noexcept
            : data(rhs.data)
        {
            if (T* p = ptr()) {
                p->add_ref();
            }
        }

        intrusive_ptr(intrusive_ptr&& rhs) noexcept
            : data(std::move(rhs.data))
        {
            rhs.ptr() = nullptr;
        }

        intrusive_ptr& operator=(const intrusive_ptr& rhs) {
            T* prev = ptr();
            data = rhs.data;
            if (T* p = ptr()) {
                p->add_ref();
            }
            if (prev && prev->release_ref() == 0) {
                deleter()(prev);
            }
            return *this;
        }

        intrusive_ptr& operator=(intrusive_ptr&& rhs) {
            if (this != &rhs) {
                T* prev = ptr();
                data = std::move(rhs.data);
                rhs.ptr() = nullptr;
                if (prev && prev->release_ref() == 0) {
                    deleter()(prev);
                }
            }
            return *this;
        }

        void reset() {
            if (T* p = ptr()) {
                ptr() = nullptr;
                if (p->release_ref() == 0) {
                    deleter()(p);
                }
            }
        }

        void reset(std::nullptr_t) {
            reset();
        }

        void reset(T* p) {
            T* prev = ptr();
            ptr() = p;
            if (p) {
                p->add_ref();
            }
            if (prev && prev->release_ref() == 0) {
                deleter()(prev);
            }
        }

        T* release() noexcept {
            T* p = ptr();
            if (p) {
                ptr() = nullptr;
                p->release_ref();
            }
            return p;
        }

        explicit operator bool() const noexcept {
            return bool(ptr());
        }

        T* get() const noexcept {
            return ptr();
        }

        T* operator->() const noexcept {
            return ptr();
        }

        T& operator*() const noexcept {
            return *ptr();
        }

        void swap(intrusive_ptr& rhs) noexcept {
            auto tmp = data;
            data = rhs.data;
            rhs.data = tmp;
        }

        friend void swap(intrusive_ptr& a, intrusive_ptr& b) noexcept {
            a.swap(b);
        }

        friend bool operator==(const intrusive_ptr& a, const intrusive_ptr& b) noexcept {
            return a.ptr() == b.ptr();
        }

    private:
        T*& ptr() noexcept { return std::get<0>(data); }
        T* ptr() const noexcept { return std::get<0>(data); }
        Deleter& deleter() noexcept { return std::get<1>(data); }
        const Deleter& deleter() const noexcept { return std::get<1>(data); }

    private:
        // Deleter may be a function pointer, so cannot inherit from it
        // And tuple seems to have an optimization for empty classes
        std::tuple<T*, Deleter> data;
    };

} // namespace coroactors::detail
