#pragma once

#include "rime/core/types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rime::automation {

// Immutable value snapshot of one UIA element. Snapshots are plain strings
// and integers so no COM pointer ever crosses the automation MTA thread.
struct ElementSnapshot {
  std::uint64_t id{0};
  std::string name;
  // Lowercase name from the shared control-type table ("button", "edit",
  // ...); empty when the element reports an id we do not map.
  std::string control_type;
  std::string automation_id;
  bool enabled{false};
  std::int32_t x{0};
  std::int32_t y{0};
  std::int32_t width{0};
  std::int32_t height{0};
};

// One find() request. At least one of name / control_type / automation_id
// must be set (otherwise the query would mean "everything"); from_id 0
// searches the whole desktop, a positive id searches that element's
// subtree; max_results caps the answer.
struct FindQuery {
  std::string name;
  std::string control_type;
  std::string automation_id;
  std::uint64_t from_id{0};
  std::uint32_t max_results{8};
};

enum class UiaServiceState : std::uint8_t { Created, Running, Stopping, Stopped };

// Owns a dedicated COM MTA thread: every IUIAutomation interface, element
// reference and registry entry lives and dies on that thread, so callers
// only exchange ids and value snapshots across the boundary. Each method
// enqueues a job and waits for it - executors call it from the timer
// thread, never from the JS thread (queries run through the Action queue)
// and never from the MTA thread itself (that would deadlock).
class UiaService final {
 public:
  UiaService();
  ~UiaService();
  UiaService(const UiaService&) = delete;
  UiaService& operator=(const UiaService&) = delete;

  // Spawns the MTA thread and creates the UIA client. Start-once; fails
  // with ExecutionFailed when COM or the UIA client cannot start.
  rime::core::Error start();
  // Quits the job queue (after draining it), joins the thread and releases
  // every element reference. Idempotent; valid from any state.
  rime::core::Error stop();
  [[nodiscard]] UiaServiceState state() const;

  // Finds elements under from_id (0 = desktop root) matching every set
  // criterion. Matches are registered and returned as snapshots.
  // InvalidContract for an empty query, an unknown control type or a zero
  // max_results; TargetGone when from_id names an element that is gone.
  rime::core::Error find(const FindQuery& query, std::vector<ElementSnapshot>& out);
  // Reads a registered element. TargetGone when the id is unknown or the
  // element died (window destroyed) - a dead entry is dropped either way.
  rime::core::Error read(std::uint64_t id, ElementSnapshot& out);
  // Invokes the UIA invoke pattern (e.g. a button click). TargetGone when
  // the id is unknown or stale; Unsupported when the element has no invoke
  // pattern; ExecutionFailed when the element is disabled or COM fails.
  rime::core::Error invoke(std::uint64_t id);
  // Drops a registered element reference. False when the id is unknown or
  // the service is not running.
  bool release(std::uint64_t id);

  // Registered element count (diagnostics and tests).
  [[nodiscard]] std::size_t element_count() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rime::automation
