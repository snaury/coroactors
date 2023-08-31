#pragma once
#include <atomic>
#include <utility>

namespace coroactors {

    /**
     * A common base class for reference counted objects
     */
    template<class Derived>
    class intrusive_atomic_base {
    public:
        // the default constructor initializes refcount to zero
        intrusive_atomic_base() noexcept = default;

        // copy constructor will not copy the refcount
        intrusive_atomic_base(const intrusive_atomic_base&) noexcept {}

        // copy assignment will not copy the refcount
        intrusive_atomic_base& operator=(const intrusive_atomic_base&) noexcept {
            return *this;
        }

    protected:
        ~intrusive_atomic_base() noexcept = default;

    private:
        // ADL customization point, compatible with boost
        friend void intrusive_ptr_add_ref(const intrusive_atomic_base* self) noexcept {
            self->refcount.fetch_add(1, std::memory_order_relaxed);
        }

        // ADL customization point, compatible with boost
        friend void intrusive_ptr_release(const intrusive_atomic_base* self) noexcept {
            if (self->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                static_assert(sizeof(Derived) >= 0);
                delete static_cast<const Derived*>(self);
            }
        }

    private:
        mutable std::atomic<size_t> refcount{ 0 };
    };

    /**
     * A default reference count manager type
     */
    template<class T>
    struct intrusive_ptr_default_manager {
        static void add_ref(T* ptr) noexcept {
            intrusive_ptr_add_ref(ptr);
        }

        static void release(T* ptr) noexcept {
            intrusive_ptr_release(ptr);
        }
    };

    /**
     * An intrusive pointer where refcount is managed by the object
     */
    template<class T, class Manager = intrusive_ptr_default_manager<T>>
    class intrusive_ptr
        : private Manager
    {
    public:
        intrusive_ptr() noexcept
            : ptr(nullptr)
        {}

        intrusive_ptr(std::nullptr_t) noexcept
            : ptr(nullptr)
        {}

        explicit intrusive_ptr(T* p, bool add_ref = true) noexcept
            : ptr(p)
        {
            if (ptr && add_ref) {
                manager().add_ref(ptr);
            }
        }

        ~intrusive_ptr() {
            if (ptr) {
                manager().release(ptr);
            }
        }

        intrusive_ptr(const intrusive_ptr& rhs) noexcept
            : ptr(rhs.ptr)
        {
            if (ptr) {
                manager().add_ref(ptr);
            }
        }

        template<class U>
        intrusive_ptr(const intrusive_ptr<U, Manager>& rhs) noexcept
            requires std::is_convertible_v<U*, T*>
            : ptr(rhs.ptr)
        {
            if (ptr) {
                manager().add_ref(ptr);
            }
        }

        intrusive_ptr(intrusive_ptr&& rhs) noexcept
            : ptr(rhs.ptr)
        {
            rhs.ptr = nullptr;
        }

        template<class U>
        intrusive_ptr(intrusive_ptr<U, Manager>&& rhs) noexcept
            requires std::is_convertible_v<U*, T*>
            : ptr(rhs.ptr)
        {
            rhs.ptr = nullptr;
        }

        intrusive_ptr& operator=(const intrusive_ptr& rhs) noexcept {
            T* prev = ptr;
            ptr = rhs.ptr;
            if (ptr) {
                manager().add_ref(ptr);
            }
            if (prev) {
                manager().release(prev);
            }
            return *this;
        }

        intrusive_ptr& operator=(intrusive_ptr&& rhs) noexcept {
            if (this != &rhs) [[likely]] {
                T* prev = ptr;
                ptr = rhs.ptr;
                rhs.ptr = nullptr;
                if (prev) {
                    manager().release(prev);
                }
            }
            return *this;
        }

        void reset() noexcept {
            if (T* p = ptr) {
                ptr = nullptr;
                manager().release(p);
            }
        }

        void reset(std::nullptr_t) noexcept {
            reset();
        }

        void reset(T* p, bool add_ref = true) noexcept {
            T* prev = ptr;
            ptr = p;
            if (ptr && add_ref) {
                manager().add_ref(ptr);
            }
            if (prev) {
                manager().release(prev);
            }
        }

        explicit operator bool() const noexcept {
            return bool(ptr);
        }

        T* get() const noexcept {
            return ptr;
        }

        T* detach() noexcept {
            T* prev = ptr;
            ptr = nullptr;
            return prev;
        }

        T* operator->() const noexcept {
            return ptr;
        }

        T& operator*() const noexcept {
            return *ptr;
        }

        void swap(intrusive_ptr& rhs) noexcept {
            auto tmp = ptr;
            ptr = rhs.ptr;
            rhs.ptr = tmp;
        }

        friend void swap(intrusive_ptr& a, intrusive_ptr& b) noexcept {
            a.swap(b);
        }

        friend bool operator<(const intrusive_ptr& a, const intrusive_ptr& b) noexcept {
            return a.ptr < b.ptr;
        }

        friend bool operator==(const intrusive_ptr& a, const intrusive_ptr& b) noexcept {
            return a.ptr == b.ptr;
        }

        template<class H>
        H AbslHashValue(H h, const intrusive_ptr& p) {
            return H::combine(std::move(h), p.ptr);
        }

    private:
        Manager& manager() noexcept {
            return *this;
        }

    private:
        T* ptr;
    };

} // namespace coroactors

namespace std {

    template<class T, class Manager>
    struct hash<::coroactors::intrusive_ptr<T, Manager>> {
        size_t operator()(const ::coroactors::intrusive_ptr<T, Manager>& p) const noexcept {
            return ::std::hash<T*>()(p.get());
        }
    };

} // namespace std
