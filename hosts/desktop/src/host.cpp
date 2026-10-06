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
    if (state_ == HostState::Stopping) {
      return {rime::core::Error::Code::InvalidState, "desktop host stop already in progress"};
    }
    state_ = HostState::Stopping;
  }
  const auto result = runtime_.stop();
  {
    std::lock_guard lock(mutex_);
    // A failed runtime stop must not strand the host in Stopping: restore
    // Running so a later stop() (after the pump drains) can succeed and the
    // state stays observable and repeatable.
    state_ = result.ok() ? HostState::Stopped : HostState::Running;
  }
  return result;
}

HostState DesktopHost::state() const {
  std::lock_guard lock(mutex_);
  return state_;
}

}  // namespace rime::desktop
