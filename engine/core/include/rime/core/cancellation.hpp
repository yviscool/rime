#pragma once

#include <atomic>
#include <memory>

namespace rime::core {

class CancellationToken {
 public:
  CancellationToken() = default;
  [[nodiscard]] bool cancelled() const noexcept {
    return state_ && state_->load(std::memory_order_acquire);
  }

 private:
  friend class CancellationSource;
  explicit CancellationToken(std::shared_ptr<std::atomic_bool> state)
      : state_(std::move(state)) {}
  std::shared_ptr<std::atomic_bool> state_;
};

class CancellationSource {
 public:
  CancellationSource()
      : state_(std::make_shared<std::atomic_bool>(false)) {}

  [[nodiscard]] CancellationToken token() const { return CancellationToken(state_); }
  void cancel() const noexcept { state_->store(true, std::memory_order_release); }

 private:
  std::shared_ptr<std::atomic_bool> state_;
};

}  // namespace rime::core
