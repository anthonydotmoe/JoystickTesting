#pragma once

#include <Unknwn.h>
#include <utility>
#include <type_traits>

template<typename T>
class ComPtr {
    static_assert(std::is_base_of_v<IUnknown, T>, "ComPtr<T> requires T to derive from IUnknown");

public:
    // Default ctor: null pointer
    ComPtr() noexcept = default;

    // Nullptr ctor
    ComPtr(std::nullptr_t) noexcept : ptr_(nullptr) {}

    // Adopt an existing pointer WITHOUT AddRef
    explicit ComPtr(T* ptr) noexcept : ptr_(ptr) {}

    // Copy: AddRef
    ComPtr(const ComPtr& other) noexcept
        : ptr_(other.ptr)
    {
        internal_addref();
    }

    // Move: transfer ownership, null out source
    ComPtr(ComPtr&& other) noexcept
        : ptr_(other.ptr_)
    {
        other.ptr_ = nullptr;
    }

    // Destructor: Release if non-null
    ~ComPtr() {
        internal_release();
    }

    // Copy assignment
    ComPtr& operator=(const ComPtr& other) noexcept {
        if (this != &other) {
            // AddRef before Release in case we are assigning to ourselves
            T* new_ptr = other.ptr_;
            if (new_ptr) {
                new_ptr->AddRef();
            }
            internal_release();
            ptr_ = new_ptr;
        }
        return *this;
    }

    // Move assignment
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            internal_release();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    // Assignment from raw pointer (adopt, no AddRef)
    ComPtr& operator=(T* ptr) noexcept {
        if (ptr_ != ptr) {
            internal_release();
            ptr_ = ptr;
        }
        return *this;
    }

    // Observers

    T* get() const noexcept { return ptr_; }
    T& operator*() const noexcept { return *ptr_; }
    T* operator->() const noexcept { return ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    // Modifiers

    // Release current pointer (if any), set to nullptr
    void reset() noexcept {
        internal_release();
        ptr_ = nullptr;
    }

    // Reset to a new raw pointer, adopting its reference.
    void reset(T* ptr) noexcept {
        if (ptr_ != ptr) {
            internal_release();
            ptr_ = ptr;
        }
    }

    // Detach without calling Release; caller takes ownership.
    [[nodiscard]] T* detach() noexcept {
        T* tmp = ptr_;
        ptr_ = nullptr;
        return tmp;
    }

    // Attach: adopt raw pointer without AddRef, like ATL's Attach
    void attach(T* ptr) noexcept {
        internal_release();
        ptr_ = ptr;
    }

    // Out-parameter helper:
    // Releases current pointer (if any) and returns address for COM out-param.
    // Example:
    //   ComPtr<IMyInterface> p;
    //   hr = obj->QueryInterface(__uuidof(IMyInterface), (void**)p.put());
    T** put() noexcept {
        internal_release();
        ptr_ = nullptr;
        return &ptr_;
    }

    // For rare cases where you need address without releasing first:
    // (Use sparingly. usually `put()` is what you want.)
    T** get_address_of() noexcept {
        return &ptr_;
    }

    // Swap
    void swap(ComPtr& other) noexcept {
        std::swap(ptr_, other.ptr_);
    }

    // Helpers

    // QueryInterface into another ComPtr<U>
    template<typename U>
    HRESULT As(ComPtr<U>& out) const noexcept {
        static_assert(std::is_base_of_v<IUnknown, U>, "ComPtr<U>::As<U> requires U to derive from IUnknown");

        if (!ptr_) {
            out.reset();
            return E_POINTER;
        }
        U* tmp = nullptr;
        HRESULT hr = ptr_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(&tmp));

        if (SUCCEEDED(hr)) {
            out.reset(tmp); // adopt returned reference
        } else {
            out.reset();
        }
        return hr;
    }

private:
    void internal_addref() noexcept {
        if (ptr_) {
            ptr_->AddRef();
        }
    }

    void internal_release() noexcept {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }


private:
    T* ptr_ = nullptr;
};

// Non-member swap for ADL
template<typename T>
void swap(ComPtr<T>& a, ComPtr<T>& b) noexcept {
    a.swap(b);
}
