#include "rime/core/runtime.hpp"

#include <chrono>
#include <utility>

namespace rime::core {
namespace {

// RAII guard ensuring pump_depth_ is decremented and waiters are notified
// even if dispatch or tracing throws.
template <typename F>
class ScopeGuard final {
 public:
  explicit ScopeGuard(F&& on_exit) : on_exit_(std::move(on_exit)), active_(true) {}
  ScopeGuard(const ScopeGuard&) = delete;
  ScopeGuard& operator=(const ScopeGuard&) = delete;
  void dismiss() noexcept { active_ = false; }
  ~ScopeGuard() noexcept {
    if (active_) {
      try {
        on_exit_();
      } catch (...) {
      }
    }
  }

 private:
  F on_exit_;
  bool active_;
};

// Handler-segment length in milliseconds, measured in the Clock's monotonic
// domain (SystemClock in production, ManualClock in tests). Clamped at 0 so a
// manual clock override can never report a negative duration.
std::uint64_t elapsed_ms(const Clock& clock, const Clock::time_point& since) {
  const auto delta =
      std::chrono::duration_cast<std::chrono::milliseconds>(clock.now() - since);
  return delta.count() > 0 ? static_cast<std::uint64_t>(delta.count()) : 0;
}

}  // namespace

Runtime::Runtime(const std::size_t queue_capacity, std::shared_ptr<TraceSink> trace,
                 const Clock* clock)
    : Runtime(SchedulerPolicy::bounded(queue_capacity), std::move(trace), clock) {}

Runtime::Runtime(SchedulerPolicy policy, std::shared_ptr<TraceSink> trace, const Clock* clock)
    : queue_(policy),
      trace_(std::move(trace)),
      clock_(clock ? clock : &SystemClock::instance()) {}

Runtime::~Runtime() { stop(); }

Error Runtime::start() {
  {
    std::lock_guard lock(mutex_);
    if (state_ != RuntimeState::Created) {
      return {Error::Code::InvalidState, "runtime can only start from Created"};
    }
    state_ = RuntimeState::Running;
  }
  trace(TraceKind::StateChanged, "runtime", "running");
  return Error::none();
}

Error Runtime::post(Event event) {
  {
    std::lock_guard lock(mutex_);
    if (state_ != RuntimeState::Running) {
      return {Error::Code::InvalidState, "runtime is not running"};
    }
    event.sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed);
  }
  const std::string subject = event.name;
  const std::string key = event.key;
  const QueueStatus status = queue_.push(std::move(event));
  // Contract: deduplication is success. Deduped/Coalesced outcomes collapse
  // to Error::none() so callers must not retry; the trace detail ("deduped
  // key=..."/"coalesced key=...") distinguishes them from Accepted.
  switch (status) {
    case QueueStatus::Accepted:
      trace(TraceKind::EventAccepted, subject, "accepted");
      return Error::none();
    case QueueStatus::Coalesced:
      trace(TraceKind::EventAccepted, subject, "coalesced key=" + key);
      return Error::none();
    case QueueStatus::Deduped:
      trace(TraceKind::EventAccepted, subject, "deduped key=" + key);
      return Error::none();
    case QueueStatus::Full:
      return {Error::Code::QueueFull, "event queue is full"};
    case QueueStatus::Closed:
      return {Error::Code::QueueClosed, "event queue is closed"};
  }
  return {Error::Code::QueueFull, "event queue is full"};
}

std::size_t Runtime::pump(const std::size_t budget) {
  EventHandler handler;
  bool nested = false;
  {
    std::lock_guard lock(mutex_);
    if (state_ != RuntimeState::Running || !handler_) return 0;
    if (pump_depth_ > 0) {
      if (pump_thread_ != std::this_thread::get_id()) return 0;
      nested = true;
    } else {
      pump_thread_ = std::this_thread::get_id();
    }
    ++pump_depth_;
    handler = handler_;
  }

  // Guarantees --pump_depth_ + notify runs even if try_pop/trace/handler
  // throws. The guard's cleanup locks internally, so no mutex is held here.
  ScopeGuard depth_guard([this, nested] {
    bool notify = false;
    {
      std::lock_guard lock(mutex_);
      --pump_depth_;
      if (pump_depth_ == 0) pump_thread_ = {};
      notify = !nested;
    }
    if (notify) condition_.notify_all();
  });

  std::size_t handled = 0;
  while (handled < budget) {
    {
      std::lock_guard lock(mutex_);
      if (state_ != RuntimeState::Running) break;
    }
    std::optional<Event> event;
    try {
      event = queue_.try_pop();
    } catch (...) {
      break;
    }
    if (!event) break;
    // Segment timing: dispatch start -> handler return. Both reads go
    // through the injected Clock so a ManualClock test gets a deterministic
    // EventDispatchFinished.duration_ms instead of wall-clock jitter.
    const Clock::time_point dispatch_started = clock_->now();
    try {
      trace(TraceKind::EventDispatchStarted, event->name, "dispatch");
    } catch (...) {
      // Tracing must never break dispatch; detail loss is acceptable.
    }
    const auto finish = [&](const std::string& detail) {
      trace(TraceKind::EventDispatchFinished, event->name, detail,
            elapsed_ms(*clock_, dispatch_started));
    };
    try {
      handler(*event, shutdown_.token());
      try {
        finish("dispatch");
      } catch (...) {
      }
    } catch (const std::exception& exception) {
      try {
        finish(exception.what());
      } catch (...) {
      }
    } catch (...) {
      try {
        finish("handler threw an unknown exception");
      } catch (...) {
      }
    }
    ++handled;
  }
  return handled;
}

Error Runtime::stop() {
  // Handlers must cooperatively check CancellationToken; otherwise the
  // condition_.wait below blocks until they return. Never destroy the
  // Runtime (or call stop()) from inside a handler on the pump thread.
  {
    std::unique_lock lock(mutex_);
    if (state_ == RuntimeState::Stopped) return Error::none();
    if (state_ == RuntimeState::Created) {
      state_ = RuntimeState::Stopped;
      return Error::none();
    }
    if (pump_depth_ > 0 && pump_thread_ == std::this_thread::get_id()) {
      return {Error::Code::InvalidState, "stop cannot wait from inside an event handler"};
    }
    state_ = RuntimeState::Stopping;
  }
  shutdown_.cancel();
  queue_.close();
  {
    std::unique_lock lock(mutex_);
    condition_.wait(lock, [this] { return pump_depth_ == 0; });
    handler_ = {};
    state_ = RuntimeState::Stopped;
  }
  trace(TraceKind::StateChanged, "runtime", "stopped");
  return Error::none();
}

void Runtime::set_handler(EventHandler handler) {
  std::lock_guard lock(mutex_);
  handler_ = std::move(handler);
}

RuntimeState Runtime::state() const {
  std::lock_guard lock(mutex_);
  return state_;
}

void Runtime::trace(const TraceKind kind, std::string subject, std::string detail,
                    const std::uint64_t duration_ms) {
  // TraceSink::record may throw (user-provided sink); tracing must never
  // propagate into dispatch/shutdown paths, so failures are swallowed.
  try {
    if (trace_) {
      trace_->record({0, kind, std::move(subject), std::move(detail), 0, {}, {}, duration_ms});
    }
  } catch (...) {
  }
}

}  // namespace rime::core
