#pragma once

#include "rime/core/types.hpp"
#include "rime/win32/window.hpp"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace rime::win32 {

class WindowService;

// One #HotIf condition. Window kinds are evaluated on the UI lane by the
// watcher thread (so match semantics are exactly WindowService's own);
// Function criteria are evaluated on the JS thread against the snapshot's
// foreground window.
enum class CriterionKind : std::uint8_t { Active, Exists, NotActive, NotExists, Function };

struct CriterionSpec {
  std::uint64_t id{0};
  CriterionKind kind{CriterionKind::Exists};
  // Used by the window kinds; ignored for Function.
  WindowQuery query{};
};

// Immutable result of one watcher pass (sequence-numbered, swapped in as a
// whole). The JS thread reads it at match time and never mutates it.
struct ContextSnapshot {
  std::uint64_t seq{0};
  bool has_foreground{false};
  WindowInfo foreground{};
  // criterion id -> met (Function entries are placeholders: the JS thread
  // re-evaluates them with the foreground payload).
  std::vector<std::pair<std::uint64_t, bool>> results;
};

// Captures HotIf context on its own thread: every `kRefreshMs` (and
// immediately when the criterion set changes) it evaluates the registered
// criteria through the WindowService UI lane and publishes a fresh
// immutable snapshot. The hook thread never runs here; the JS thread only
// reads published snapshots.
class ContextWatcher final {
 public:
  // Fixed refresh cadence. Central to this stage: HotIf window state can be
  // up to this old when a hotkey evaluates its criterion.
  static constexpr std::uint64_t kRefreshMs = 50;
  // Per UI-lane call bound during a capture.
  static constexpr std::int64_t kCaptureTimeoutMs = 500;

  explicit ContextWatcher(WindowService& windows);
  ~ContextWatcher();
  ContextWatcher(const ContextWatcher&) = delete;
  ContextWatcher& operator=(const ContextWatcher&) = delete;

  // JS thread: swap the criterion set. Starts the watcher on the first
  // non-empty set, wakes an immediate capture, and stops the thread when the
  // set becomes empty. Blocking join only happens when the thread exits.
  void set_criteria(std::shared_ptr<const std::vector<CriterionSpec>> criteria);
  // Latest snapshot; null before the first capture completes.
  std::shared_ptr<const ContextSnapshot> current() const;
  // Repeatable: tells the watcher to exit and joins it.
  void stop();
  [[nodiscard]] bool running() const;

 private:
  void run();
  std::shared_ptr<ContextSnapshot> capture(
      const std::vector<CriterionSpec>& criteria) const;

  WindowService& windows_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::shared_ptr<const std::vector<CriterionSpec>> criteria_;
  std::shared_ptr<const ContextSnapshot> current_;
  bool capture_pending_{false};
  bool stop_requested_{false};
  std::thread thread_;
};

}  // namespace rime::win32
