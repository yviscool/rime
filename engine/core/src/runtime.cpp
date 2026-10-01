#include "rime/core/runtime.hpp"

#include <utility>

namespace rime::core {

Runtime::Runtime(const std::size_t queue_capacity, std::shared_ptr<TraceSink> trace)
    : Runtime(SchedulerPolicy::bounded(queue_capacity), std::move(trace)) {}

Runtime::Runtime(SchedulerPolicy policy, std::shared_ptr<TraceSink> trace)
    : queue_(policy), trace_(std::move(trace)) {}

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

  std::size_t handled = 0;
  while (handled < budget) {
    {
      std::lock_guard lock(mutex_);
      if (state_ != RuntimeState::Running) break;
    }
    auto event = queue_.try_pop();
    if (!event) break;
    trace(TraceKind::ActionStarted, event->name, "dispatch");
    try {
      handler(*event, shutdown_.token());
      trace(TraceKind::ActionFinished, event->name, "dispatch");
    } catch (const std::exception& exception) {
      trace(TraceKind::ActionFinished, event->name, exception.what());
    } catch (...) {
      trace(TraceKind::ActionFinished, event->name, "handler threw an unknown exception");
    }
    ++handled;
  }
  {
    std::lock_guard lock(mutex_);
    --pump_depth_;
    if (pump_depth_ == 0) pump_thread_ = {};
  }
  if (!nested) condition_.notify_all();
  return handled;
}

Error Runtime::stop() {
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

void Runtime::trace(const TraceKind kind, std::string subject, std::string detail) {
  if (trace_) {
    trace_->record({next_trace_sequence_.fetch_add(1, std::memory_order_relaxed), kind,
                    std::move(subject), std::move(detail)});
  }
}

}  // namespace rime::core
