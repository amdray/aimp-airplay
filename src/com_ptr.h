#pragma once
#include <utility>

// Owns one COM reference. put() is for SDK out parameters, never for borrowing.
template<class T> class ComPtr {
public:
    ComPtr() = default;
    explicit ComPtr(T* value) : value_(value) { if (value_) value_->AddRef(); }
    ComPtr(const ComPtr& other) : ComPtr(other.value_) {}
    ComPtr(ComPtr&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    ~ComPtr() { reset(); }
    ComPtr& operator=(ComPtr other) noexcept { std::swap(value_, other.value_); return *this; }
    T* get() const { return value_; }
    T* operator->() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }
    void reset() { if (auto* old = std::exchange(value_, nullptr)) old->Release(); }
    T** put() { reset(); return &value_; }
    void** out() { return reinterpret_cast<void**>(put()); }
private:
    T* value_ = nullptr;
};
