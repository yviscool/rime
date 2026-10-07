#pragma once

#include <windows.h>

#include <objbase.h>

namespace rime::win32 {

// A single owning COM pointer: Release() on reset and on destruction, no
// implicit copy. This is the only way COM interfaces enter or leave this
// code base - nothing hands a raw pointer to JS.
template <typename T>
class ComPtr final {
 public:
  ComPtr() = default;
  ComPtr(const ComPtr&) = delete;
  ComPtr& operator=(const ComPtr&) = delete;
  // Ownership moves, it never copies - which is what lets a search struct or
  // an error path hand its interfaces on to the next scope.
  ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
  ComPtr& operator=(ComPtr&& other) noexcept {
    if (this != &other) {
      reset();
      ptr_ = other.ptr_;
      other.ptr_ = nullptr;
    }
    return *this;
  }
  ~ComPtr() { reset(); }

  [[nodiscard]] T* get() const { return ptr_; }
  [[nodiscard]] T* operator->() const { return ptr_; }
  [[nodiscard]] explicit operator bool() const { return ptr_ != nullptr; }

  [[nodiscard]] T** put() {
    reset();
    return &ptr_;
  }

  // Hands the owning reference to the caller; the destructor no longer
  // releases it. Used when a Win32 out-parameter has to be adopted.
  [[nodiscard]] T* detach() {
    T* detached = ptr_;
    ptr_ = nullptr;
    return detached;
  }

  // Releases what is held and, optionally, adopts an owning reference the
  // caller just obtained - the void*-out convention that Activate() and
  // friends use. `adopted` is taken as-is, never AddRef'ed.
  void reset(T* adopted = nullptr) {
    if (ptr_ == adopted) return;
    if (ptr_ != nullptr) ptr_->Release();
    ptr_ = adopted;
  }

 private:
  T* ptr_{nullptr};
};

// Balances CoInitializeEx for the calling scope: a first-time or same-model
// initialization is released on exit, while RPC_E_CHANGED_MODE (COM already
// present in the other apartment) is adopted without an unbalance.
//
// COM objects are created and released inside one scope on the thread that
// owns that scope, so nothing is ever marshalled across threads: a worker
// body takes COINIT_MULTITHREADED, the UI-thread selectors take
// COINIT_APARTMENTTHREADED. Callers check ok() before they use the HRESULT
// to tell "COM unavailable" from "the call failed".
class ComApartment final {
 public:
  explicit ComApartment(const DWORD model) {
    hr_ = CoInitializeEx(nullptr, model);
    owns_ = (hr_ == S_OK || hr_ == S_FALSE);
    available_ = SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE;
  }

  ComApartment(const ComApartment&) = delete;
  ComApartment& operator=(const ComApartment&) = delete;

  ~ComApartment() {
    if (owns_) CoUninitialize();
  }

  [[nodiscard]] bool ok() const { return available_; }
  [[nodiscard]] HRESULT hr() const { return hr_; }

 private:
  HRESULT hr_{S_OK};
  bool owns_{false};
  bool available_{false};
};

}  // namespace rime::win32
