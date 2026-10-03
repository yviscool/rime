#include "rime/win32/context_watcher.hpp"

#include "rime/win32/window.hpp"

#include <chrono>

namespace rime::win32 {

ContextWatcher::ContextWatcher(WindowService& windows) : windows_(windows) {}

ContextWatcher::~ContextWatcher() { stop(); }

void ContextWatcher::set_criteria(
    std::shared_ptr<const std::vector<CriterionSpec>> criteria) {
  bool join_thread = false;
  {
    std::lock_guard lock(mutex_);
    criteria_ = std::move(criteria);
    capture_pending_ = true;
    const bool empty = !criteria_ || criteria_->empty();
    if (empty && thread_.joinable()) {
      stop_requested_ = true;
      join_thread = true;
    } else if (!empty && !thread_.joinable()) {
      stop_requested_ = false;
      thread_ = std::thread(&ContextWatcher::run, this);
    }
  }
  condition_.notify_all();
  if (join_thread && thread_.joinable()) thread_.join();
}

std::shared_ptr<const ContextSnapshot> ContextWatcher::current() const {
  std::lock_guard lock(mutex_);
  return current_;
}

void ContextWatcher::stop() {
  std::thread worker;
  {
    std::lock_guard lock(mutex_);
    stop_requested_ = true;
    if (thread_.joinable()) worker = std::move(thread_);
  }
  condition_.notify_all();
  if (worker.joinable()) worker.join();
}

bool ContextWatcher::running() const {
  std::lock_guard lock(mutex_);
  return thread_.joinable();
}

std::shared_ptr<ContextSnapshot> ContextWatcher::capture(
    const std::vector<CriterionSpec>& criteria) const {
  auto snapshot = std::make_shared<ContextSnapshot>();
  const auto timeout = std::chrono::milliseconds(kCaptureTimeoutMs);
  bool need_foreground = false;
  for (const auto& criterion : criteria) {
    if (criterion.kind == CriterionKind::Function) {
      need_foreground = true;
      break;
    }
  }
  if (need_foreground) {
    std::optional<WindowInfo> foreground;
    if (windows_.active(foreground, timeout).ok() && foreground.has_value()) {
      snapshot->has_foreground = true;
      snapshot->foreground = std::move(*foreground);
    }
  }
  for (const auto& criterion : criteria) {
    bool met = false;
    bool evaluated = true;
    switch (criterion.kind) {
      case CriterionKind::Exists: {
        bool found = false;
        evaluated = windows_.exists(criterion.query, found, timeout).ok();
        met = found;
        break;
      }
      case CriterionKind::Active: {
        bool matched = false;
        evaluated = windows_.matches_active(criterion.query, matched, timeout).ok();
        met = matched;
        break;
      }
      case CriterionKind::NotActive: {
        bool matched = false;
        evaluated = windows_.matches_active(criterion.query, matched, timeout).ok();
        met = !matched;
        break;
      }
      case CriterionKind::NotExists: {
        bool found = false;
        evaluated = windows_.exists(criterion.query, found, timeout).ok();
        met = !found;
        break;
      }
      case CriterionKind::Function:
        // Placeholder: the JS thread invokes the function with the
        // foreground window of this snapshot.
        met = true;
        break;
    }
    if (!evaluated) met = false;  // fail closed when the UI lane refused
    snapshot->results.emplace_back(criterion.id, met);
  }
  return snapshot;
}

void ContextWatcher::run() {
  std::uint64_t next_seq = 1;
  for (;;) {
    std::shared_ptr<const std::vector<CriterionSpec>> criteria;
    {
      std::unique_lock lock(mutex_);
      // Wakes on set_criteria (predicate) or on the refresh tick (timeout).
      condition_.wait_for(lock, std::chrono::milliseconds(kRefreshMs),
                          [this] { return stop_requested_ || capture_pending_; });
      if (stop_requested_) return;
      capture_pending_ = false;
      criteria = criteria_;
    }
    if (!criteria || criteria->empty()) continue;
    auto snapshot = capture(*criteria);
    snapshot->seq = next_seq++;
    {
      std::lock_guard lock(mutex_);
      if (stop_requested_) return;
      current_ = std::move(snapshot);
    }
  }
}

}  // namespace rime::win32
