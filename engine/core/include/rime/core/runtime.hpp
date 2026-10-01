#pragma once

#include "rime/core/cancellation.hpp"
#include "rime/core/event_queue.hpp"
#include "rime/core/trace.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace rime::core {

enum class RuntimeState : std::uint8_t { Created, Running, Stopping, Stopped };

using EventHandler = std::function<void(const Event&, CancellationToken)>;

class Runtime final {
 public:
  explicit Runtime(std::size_t queue_capacity, std::shared_ptr<TraceSink> trace = {});
  explicit Runtime(SchedulerPolicy policy, std::shared_ptr<TraceSink> trace = {});
  ~Runtime();

  // NOTE: destroying a Runtime from inside its own event handler is
  // forbidden (stop() would deadlock waiting for the pump to drain).
  [[nodiscard]] Error start();
  // NOTE: Deduped/Coalesced deliveries report success (Error::none()); see
  // Runtime::post for the "deduplication is success" contract.
  [[nodiscard]] Error post(Event event);
  // Dispatches pending events on the calling thread. Nested calls on the
  // pump thread (modal loops) dispatch through the same scheduler and keep
  // FIFO order; calls from other threads are ignored while a pump is active.
  std::size_t pump(std::size_t budget = 1);
  // NOTE: stop() blocks until in-flight pump() calls drain. Handlers must
  // cooperatively observe the shutdown token and return; a handler that
  // never returns will block stop() forever. Must not be called from
  // inside an event handler on the pump thread (returns InvalidState).
  Error stop();

  void set_handler(EventHandler handler);
  [[nodiscard]] RuntimeState state() const;
  [[nodiscard]] CancellationToken shutdown_token() const { return shutdown_.token(); }
  [[nodiscard]] const SchedulerPolicy& policy() const { return queue_.policy(); }

 private:
  void trace(TraceKind kind, std::string subject, std::string detail);

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  RuntimeState state_{RuntimeState::Created};
  EventQueue queue_;
  CancellationSource shutdown_;
  EventHandler handler_;
  std::shared_ptr<TraceSink> trace_;
  std::atomic<Sequence> next_sequence_{1};
  std::atomic<Sequence> next_trace_sequence_{1};
  int pump_depth_{0};
  std::thread::id pump_thread_{};
};

}  // namespace rime::core
