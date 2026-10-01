#pragma once

#include "rime/core/runtime.hpp"

#include <mutex>

namespace rime::desktop {

enum class HostState : std::uint8_t { Created, Running, Stopping, Stopped };

class DesktopHost final {
 public:
  explicit DesktopHost(rime::core::Runtime& runtime) : runtime_(runtime) {}

  rime::core::Error start();
  rime::core::Error stop();
  [[nodiscard]] HostState state() const;

 private:
  rime::core::Runtime& runtime_;
  mutable std::mutex mutex_;
  HostState state_{HostState::Created};
};

}  // namespace rime::desktop
