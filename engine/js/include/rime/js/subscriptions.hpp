#pragma once

#include "rime/core/types.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace rime::js {

// Tracks runtime-owned callback subscriptions (JS functions, hooks, window
// procedures). Unload must fail while entries remain — see HostAbi::unload.
class SubscriptionRegistry final {
 public:
  // Registers an entry; fails once the registry is closed.
  [[nodiscard]] rime::core::Error add(std::string kind, std::uint64_t id);
  [[nodiscard]] rime::core::Error remove(std::uint64_t id);
  // Closes the registry: further adds are rejected.
  void close();

  [[nodiscard]] std::vector<std::string> list() const;
  [[nodiscard]] std::size_t size() const;
  [[nodiscard]] bool closed() const;

 private:
  mutable std::mutex mutex_;
  std::vector<std::pair<std::string, std::uint64_t>> entries_;
  bool closed_{false};
};

}  // namespace rime::js
