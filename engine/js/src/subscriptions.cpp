#include "rime/js/subscriptions.hpp"

#include <algorithm>

namespace rime::js {

rime::core::Error SubscriptionRegistry::add(std::string kind, const std::uint64_t id) {
  if (kind.empty() || id == 0) {
    return {rime::core::Error::Code::InvalidContract, "subscription requires a kind and id"};
  }
  std::lock_guard lock(mutex_);
  if (closed_) {
    return {rime::core::Error::Code::InvalidState, "runtime is shutting down"};
  }
  const auto duplicate =
      std::find_if(entries_.begin(), entries_.end(),
                   [id](const auto& entry) { return entry.second == id; });
  if (duplicate != entries_.end()) {
    return {rime::core::Error::Code::InvalidState, "subscription id already registered"};
  }
  entries_.emplace_back(std::move(kind), id);
  return rime::core::Error::none();
}

rime::core::Error SubscriptionRegistry::remove(const std::uint64_t id) {
  std::lock_guard lock(mutex_);
  const auto found = std::find_if(entries_.begin(), entries_.end(),
                                  [id](const auto& entry) { return entry.second == id; });
  if (found == entries_.end()) {
    return {rime::core::Error::Code::InvalidState, "subscription does not exist"};
  }
  entries_.erase(found);
  return rime::core::Error::none();
}

void SubscriptionRegistry::close() {
  std::lock_guard lock(mutex_);
  // NOTE: entries are intentionally NOT cleared here. close() only rejects
  // future adds; remaining entries stay visible so unload() can diagnose
  // leaks. Return type intentionally stays void.
  closed_ = true;
}

std::vector<std::string> SubscriptionRegistry::list() const {
  std::lock_guard lock(mutex_);
  std::vector<std::string> result;
  result.reserve(entries_.size());
  for (const auto& [kind, id] : entries_) {
    result.push_back(kind + "#" + std::to_string(id));
  }
  return result;
}

std::size_t SubscriptionRegistry::size() const {
  std::lock_guard lock(mutex_);
  return entries_.size();
}

bool SubscriptionRegistry::closed() const {
  std::lock_guard lock(mutex_);
  return closed_;
}

}  // namespace rime::js
