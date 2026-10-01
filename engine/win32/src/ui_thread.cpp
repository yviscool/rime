#include "rime/win32/ui_thread.hpp"

#include "rime/core/lane.hpp"

#include <windows.h>

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>

namespace rime::win32 {
namespace {

constexpr UINT kTaskMessage = WM_APP + 1;
constexpr wchar_t kWindowClass[] = L"RimeUiThreadMessageWindow";

struct UiTask {
  std::function<void()> fn;
  rime::core::CancellationToken cancellation;
  bool claimed{false};
  bool ran{false};
  bool done{false};
  bool abandoned{false};
  std::mutex mutex;
  std::condition_variable condition;
};

void finish_task(const std::shared_ptr<UiTask>& task, const bool ran) {
  std::lock_guard lock(task->mutex);
  task->ran = ran;
  task->done = true;
  task->condition.notify_all();
}

void run_task(const std::shared_ptr<UiTask>& task) {
  {
    std::lock_guard lock(task->mutex);
    if (task->abandoned || task->cancellation.cancelled()) {
      task->done = true;
      task->condition.notify_all();
      return;
    }
    task->claimed = true;
  }
  try {
    task->fn();
  } catch (...) {
    // Tasks must not throw across the message pump boundary.
  }
  finish_task(task, true);
}

}  // namespace

struct UiThread::Impl {
  mutable std::mutex mutex;
  std::condition_variable condition;
  UiThreadState state{UiThreadState::Created};
  rime::core::Error startup_error{};
  bool closing{false};
  DWORD thread_id{0};
  std::thread thread;
  HWND hidden_window{nullptr};
  std::deque<std::shared_ptr<UiTask>> queue;

  void drain_queue() {
    std::deque<std::shared_ptr<UiTask>> pending;
    bool closed = false;
    {
      std::lock_guard lock(mutex);
      pending.swap(queue);
      closed = closing;
    }
    for (const auto& task : pending) {
      if (closed) {
        // Shutdown cancels work that has not started yet.
        std::lock_guard lock(task->mutex);
        if (!task->done) {
          task->done = true;
          task->condition.notify_all();
        }
        continue;
      }
      run_task(task);
    }
  }
};

const char* ui_thread_state_name(const UiThreadState state) {
  switch (state) {
    case UiThreadState::Created:
      return "created";
    case UiThreadState::Starting:
      return "starting";
    case UiThreadState::Running:
      return "running";
    case UiThreadState::Stopping:
      return "stopping";
    case UiThreadState::Stopped:
      return "stopped";
    case UiThreadState::Failed:
      return "failed";
  }
  return "unknown";
}

namespace {

LRESULT CALLBACK ui_thread_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == kTaskMessage) {
    auto* impl = reinterpret_cast<UiThread::Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (impl) impl->drain_queue();
    return 0;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

bool ensure_window_class() {
  static std::once_flag once;
  static bool registered = false;
  std::call_once(once, [] {
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = ui_thread_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = kWindowClass;
    registered = RegisterClassExW(&window_class) != 0 ||
                 GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
  });
  return registered;
}

}  // namespace

UiThread::UiThread() : impl_(std::make_unique<Impl>()) {}

UiThread::~UiThread() { stop(); }

rime::core::Error UiThread::start() {
  std::unique_lock lock(impl_->mutex);
  if (impl_->state != UiThreadState::Created) {
    return {rime::core::Error::Code::InvalidState, "UI thread can only start once"};
  }
  impl_->state = UiThreadState::Starting;
  impl_->thread = std::thread([this] {
    if (const auto lane_error = rime::core::LaneRegistry::instance().claim(rime::core::Lane::Ui);
        !lane_error.ok()) {
      std::lock_guard lock(impl_->mutex);
      impl_->state = UiThreadState::Failed;
      impl_->startup_error = lane_error;
      impl_->condition.notify_all();
      return;
    }

    HWND hidden = nullptr;
    if (ensure_window_class()) {
      hidden = CreateWindowExW(0, kWindowClass, L"RimeUiThread", 0, 0, 0, 0, 0, HWND_MESSAGE,
                               nullptr, GetModuleHandleW(nullptr), nullptr);
    }

    bool run_pump = false;
    {
      std::lock_guard lock(impl_->mutex);
      if (hidden) {
        SetWindowLongPtrW(hidden, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl_.get()));
        impl_->hidden_window = hidden;
        impl_->thread_id = GetCurrentThreadId();
      }
      if (!hidden) {
        impl_->state = UiThreadState::Failed;
        impl_->startup_error = {rime::core::Error::Code::InvalidState,
                                "UI thread could not create its message window"};
      } else if (impl_->state == UiThreadState::Starting) {
        impl_->state = UiThreadState::Running;
        run_pump = true;
      } else if (impl_->state == UiThreadState::Running) {
        run_pump = true;
      }
      impl_->condition.notify_all();
    }

    if (run_pump) {
      MSG message;
      BOOL status = 0;
      while ((status = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
      }
      (void)status;
    }

    impl_->drain_queue();
    {
      std::lock_guard lock(impl_->mutex);
      if (impl_->hidden_window) {
        DestroyWindow(impl_->hidden_window);
        impl_->hidden_window = nullptr;
      }
      impl_->thread_id = 0;
      if (impl_->state != UiThreadState::Failed) impl_->state = UiThreadState::Stopped;
      impl_->condition.notify_all();
    }
    // Re-drain: a caller may have queued work between pump exit and the
    // state change above; closing is already set, so those tasks resolve as
    // cancelled instead of waiting for their deadline.
    impl_->drain_queue();
    rime::core::LaneRegistry::instance().release(rime::core::Lane::Ui);
  });
  impl_->condition.wait(lock, [this] { return impl_->state != UiThreadState::Starting; });
  return impl_->startup_error;
}

rime::core::Error UiThread::call(const std::function<void()>& task,
                                 const std::chrono::milliseconds timeout,
                                 const rime::core::CancellationToken cancellation) {
  if (!task) return {rime::core::Error::Code::InvalidContract, "UI task is empty"};
  if (on_ui_thread()) {
    {
      std::lock_guard lock(impl_->mutex);
      if (impl_->state != UiThreadState::Running || impl_->closing) {
        return {rime::core::Error::Code::InvalidState, "UI thread is not running"};
      }
    }
    if (cancellation.cancelled()) {
      return {rime::core::Error::Code::Cancelled, "UI task was cancelled"};
    }
    task();
    return rime::core::Error::none();
  }

  auto queued = std::make_shared<UiTask>();
  queued->fn = task;
  queued->cancellation = cancellation;
  HWND target = nullptr;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->state != UiThreadState::Running || impl_->closing) {
      return {rime::core::Error::Code::InvalidState, "UI thread is not running"};
    }
    target = impl_->hidden_window;
    impl_->queue.push_back(queued);
  }
  if (!target || !PostMessageW(target, kTaskMessage, 0, 0)) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->queue.empty() && impl_->queue.back() == queued) impl_->queue.pop_back();
    return {rime::core::Error::Code::InvalidState, "UI thread rejected the task"};
  }

  std::unique_lock lock(queued->mutex);
  const bool finished = queued->condition.wait_for(lock, timeout, [&] { return queued->done; });
  if (!finished) {
    if (!queued->claimed) {
      queued->abandoned = true;
      return {rime::core::Error::Code::ExecutionFailed, "UI call timed out while queued"};
    }
    queued->condition.wait(lock, [&] { return queued->done; });
  }
  if (!queued->ran) {
    return {rime::core::Error::Code::Cancelled, "UI task was cancelled before it ran"};
  }
  return rime::core::Error::none();
}

rime::core::Error UiThread::stop() {
  {
    std::unique_lock lock(impl_->mutex);
    if (impl_->state == UiThreadState::Stopped) return rime::core::Error::none();
    if (impl_->state == UiThreadState::Created) {
      impl_->state = UiThreadState::Stopped;
      return rime::core::Error::none();
    }
    if (impl_->state != UiThreadState::Failed) impl_->state = UiThreadState::Stopping;
    impl_->closing = true;
    impl_->condition.wait(lock, [this] {
      return impl_->thread_id != 0 || impl_->state == UiThreadState::Stopped ||
             impl_->state == UiThreadState::Failed;
    });
    if (impl_->thread_id != 0) {
      PostThreadMessageW(impl_->thread_id, WM_QUIT, 0, 0);
    }
  }
  if (impl_->thread.joinable()) impl_->thread.join();
  std::lock_guard lock(impl_->mutex);
  impl_->state = UiThreadState::Stopped;
  impl_->condition.notify_all();
  return rime::core::Error::none();
}

UiThreadState UiThread::state() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->state;
}

bool UiThread::on_ui_thread() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->thread_id != 0 && impl_->thread_id == GetCurrentThreadId();
}

}  // namespace rime::win32
