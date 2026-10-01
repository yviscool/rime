#pragma once

#include "rime/core/types.hpp"

#include <array>
#include <mutex>
#include <thread>

namespace rime::core {

// Thread affinity lanes. Cross-lane access is a contract violation: QuickJS
// objects live on the JS lane, HWND/DirectComposition on the UI lane and COM
// interfaces on the Automation lane.
enum class Lane : std::uint8_t { Js = 0, Ui, Automation, Count };

const char* lane_name(Lane lane);

class LaneRegistry final {
 public:
  static LaneRegistry& instance();

  // Claims a lane for the calling thread. Re-claiming from the same thread
  // is idempotent; claiming from another thread fails.
  [[nodiscard]] Error claim(Lane lane);
  void release(Lane lane);
  [[nodiscard]] bool is_current(Lane lane) const;
  // Test support: drops every claim.
  void reset();

 private:
  mutable std::mutex mutex_;
  std::array<bool, static_cast<std::size_t>(Lane::Count)> claimed_{};
  std::array<std::thread::id, static_cast<std::size_t>(Lane::Count)> owners_{};
};

// Returns InvalidState with a diagnostic when the calling thread does not
// own the lane.
[[nodiscard]] Error require_lane(Lane lane);

}  // namespace rime::core
