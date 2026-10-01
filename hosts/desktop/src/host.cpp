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
  // TODO(shutdown): no busy/concurrent-stop path yet (e.g. stop() while a
  // previous stop is in Stopping, or stop() racing start()). Not adding a
  // ShutdownSequence member here to avoid a cross-module ownership chain;
  // a second stop() during Stopping currently falls through to runtime_.stop().
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
