#pragma once

#include "rime/core/types.hpp"

#include <string>

namespace rime::core {

enum class EventKind : std::uint8_t {
  Input,
  ActionCompletion,
  Shutdown,
};

struct Event {
  Sequence sequence{0};
  EventKind kind{EventKind::Input};
  std::string name;
  std::string payload;
  // Non-empty key enables coalescing/deduplication under SchedulerPolicy.
  std::string key;
};

}  // namespace rime::core
