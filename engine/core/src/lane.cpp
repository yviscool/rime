#include "rime/core/lane.hpp"

#include <string>

namespace rime::core {

const char* lane_name(const Lane lane) {
  switch (lane) {
    case Lane::Js:
      return "js";
    case Lane::Ui:
      return "ui";
    case Lane::Automation:
      return "automation";
    case Lane::Count:
      break;
  }
  return "unknown";
}

LaneRegistry& LaneRegistry::instance() {
  static LaneRegistry registry;
  return registry;
}

Error LaneRegistry::claim(const Lane lane) {
  if (lane >= Lane::Count) return {Error::Code::InvalidContract, "unknown lane"};
  const auto index = static_cast<std::size_t>(lane);
  std::lock_guard lock(mutex_);
  const std::thread::id self = std::this_thread::get_id();
  if (claimed_[index]) {
    if (owners_[index] == self) return Error::none();
    return {Error::Code::InvalidState,
            std::string("lane '") + lane_name(lane) + "' is already owned by another thread"};
  }
  claimed_[index] = true;
  owners_[index] = self;
  return Error::none();
}

void LaneRegistry::release(const Lane lane) {
  if (lane >= Lane::Count) return;
  const auto index = static_cast<std::size_t>(lane);
  std::lock_guard lock(mutex_);
  claimed_[index] = false;
  owners_[index] = {};
}

bool LaneRegistry::is_current(const Lane lane) const {
  if (lane >= Lane::Count) return false;
  const auto index = static_cast<std::size_t>(lane);
  std::lock_guard lock(mutex_);
  return claimed_[index] && owners_[index] == std::this_thread::get_id();
}

void LaneRegistry::reset() {
  std::lock_guard lock(mutex_);
  claimed_.fill(false);
  owners_.fill({});
}

Error require_lane(const Lane lane) {
  if (LaneRegistry::instance().is_current(lane)) return Error::none();
  return {Error::Code::InvalidState,
          std::string("this operation requires the '") + lane_name(lane) + "' thread lane"};
}

}  // namespace rime::core
