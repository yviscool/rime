#pragma once

#include <windows.h>

#include <utility>

namespace rime::win32 {

// Minimal RAII owner for a raw Win32 HANDLE: closes the handle on scope exit
// (and on reset / move assignment), so failure paths cannot leak one. nullptr
// and INVALID_HANDLE_VALUE are "no handle" states, which every API reports
// instead of a live handle. Copy is disabled - ownership transfers by move.
class HandleGuard final {
 public:
  HandleGuard() = default;
  explicit HandleGuard(HANDLE raw) : handle_(raw) {}

  HandleGuard(const HandleGuard&) = delete;
  HandleGuard& operator=(const HandleGuard&) = delete;

  HandleGuard(HandleGuard&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
  HandleGuard& operator=(HandleGuard&& other) noexcept {
    if (this != &other) reset();
    handle_ = std::exchange(other.handle_, nullptr);
    return *this;
  }

  ~HandleGuard() { reset(); }

  [[nodiscard]] HANDLE get() const { return handle_; }

  [[nodiscard]] explicit operator bool() const {
    return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
  }

  void reset() {
    if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    handle_ = nullptr;
  }

 private:
  HANDLE handle_{nullptr};
};

}  // namespace rime::win32
