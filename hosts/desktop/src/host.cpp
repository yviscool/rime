#include "rime/desktop/host.hpp"

namespace rime::desktop {

rime::core::Error DesktopHost::start() {
  std::lock_guard lock(mutex_);
  if (state_ != HostState::Created) {
    return {rime::core::Error::Code::InvalidState, "desktop host can only start once"};
  }
  const auto result = runtime_.start();
  if (result.ok()) state_ = HostState::Running;
  return result;
}

rime::core::Error DesktopHost::stop() {
  {
    std::lock_guard lock(mutex_);
    if (state_ == HostState::Stopped) return rime::core::Error::none();
    state_ = HostState::Stopping;
  }
  const auto result = runtime_.stop();
  {
    std::lock_guard lock(mutex_);
    if (result.ok()) state_ = HostState::Stopped;
  }
  return result;
}

HostState DesktopHost::state() const {
  std::lock_guard lock(mutex_);
  return state_;
}

}  // namespace rime::desktop
