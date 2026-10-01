#include "rime/win32/input.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace rime::win32 {
namespace {

constexpr UINT kQuitMessage = WM_APP + 77;
constexpr UINT kWakeMessage = WM_APP + 78;
constexpr std::size_t kPendingCapacity = 2048;
constexpr auto kInstallTimeout = std::chrono::seconds(2);

bool async_key_down(const int virtual_key) {
  return (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
}

}  // namespace

struct InputService::Impl {
  struct Entry {
    std::uint64_t id{0};
    Callback callback;
    bool closed{false};
    int in_flight{0};
    std::condition_variable idle;
  };

  // Shared state; the mutex also orders subscribe/unsubscribe against
  // delivery in-flight counters.
  std::mutex mutex;
  std::condition_variable condition;
  InputServiceState state{InputServiceState::Created};
  bool stopping{false};
  bool ready{false};
  bool install_ok{false};
  DWORD thread_id{0};
  std::thread thread;

  std::vector<std::shared_ptr<Entry>> entries;
  std::vector<InputEvent> pending;
  std::uint64_t next_sequence{1};
  std::uint64_t next_id{1};
  std::uint64_t dropped{0};

  // Hook-thread only.
  HHOOK keyboard_hook{nullptr};
  HHOOK mouse_hook{nullptr};

  // The single running service owns the low-level hook callbacks (they
  // receive no per-hook user pointer).
  static std::atomic<Impl*> owner;

  void enqueue(InputEvent event) {
    {
      std::lock_guard lock(mutex);
      event.sequence = next_sequence++;
      if (pending.size() >= kPendingCapacity) {
        pending.erase(pending.begin());
        ++dropped;
      }
      pending.push_back(std::move(event));
    }
    // The delivery loop drains pending only when GetMessage returns; wake it.
    if (thread_id != 0) PostThreadMessageW(thread_id, kWakeMessage, 0, 0);
  }

  void deliver_events() {
    std::vector<InputEvent> batch;
    std::vector<std::shared_ptr<Entry>> targets;
    {
      std::lock_guard lock(mutex);
      batch.swap(pending);
      targets.assign(entries.begin(), entries.end());
    }
    for (const auto& event : batch) {
      for (const auto& entry : targets) {
        bool run = false;
        {
          std::lock_guard lock(mutex);
          if (!entry->closed) {
            entry->in_flight++;
            run = true;
          }
        }
        if (!run) continue;
        try {
          entry->callback(event);
        } catch (...) {
          // Subscribers must not throw across the hook boundary.
        }
        {
          std::lock_guard lock(mutex);
          entry->in_flight--;
          entry->idle.notify_all();
        }
      }
    }
  }

  void cleanup_hooks() {
    if (mouse_hook) {
      UnhookWindowsHookEx(mouse_hook);
      mouse_hook = nullptr;
    }
    if (keyboard_hook) {
      UnhookWindowsHookEx(keyboard_hook);
      keyboard_hook = nullptr;
    }
  }

  void thread_main();
  static LRESULT CALLBACK keyboard_proc(int code, WPARAM wparam, LPARAM lparam);
  static LRESULT CALLBACK mouse_proc(int code, WPARAM wparam, LPARAM lparam);
};

std::atomic<InputService::Impl*> InputService::Impl::owner{nullptr};

void InputService::Impl::thread_main() {
  {
    std::lock_guard lock(mutex);
    thread_id = GetCurrentThreadId();
  }
  keyboard_hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_proc, GetModuleHandleW(nullptr), 0);
  mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, mouse_proc, GetModuleHandleW(nullptr), 0);
  const bool installed = keyboard_hook != nullptr && mouse_hook != nullptr;
  {
    std::lock_guard lock(mutex);
    install_ok = installed;
    ready = true;
  }
  condition.notify_all();
  if (!installed) {
    cleanup_hooks();
    Impl* expected = this;
    owner.compare_exchange_strong(expected, nullptr);
    return;
  }

  MSG message;
  for (;;) {
    const BOOL received = GetMessageW(&message, nullptr, 0, 0);
    if (received <= 0 || message.message == kQuitMessage) break;
    {
      std::lock_guard lock(mutex);
      if (stopping) break;
    }
    deliver_events();
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  cleanup_hooks();
  {
    std::lock_guard lock(mutex);
    for (auto& entry : entries) entry->closed = true;
    entries.clear();
    pending.clear();
  }
  Impl* expected = this;
  owner.compare_exchange_strong(expected, nullptr);
}

LRESULT CALLBACK InputService::Impl::keyboard_proc(const int code, const WPARAM wparam,
                                                   const LPARAM lparam) {
  if (code == HC_ACTION) {
    Impl* self = owner.load(std::memory_order_acquire);
    if (self) {
      const auto* data = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
      const bool down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
      const bool up = wparam == WM_KEYUP || wparam == WM_SYSKEYUP;
      if (down || up) {
        InputEvent event;
        event.kind = InputEventKind::Key;
        event.timestamp_ms = data->time;
        event.injected = (data->flags & LLKHF_INJECTED) != 0;
        event.key_down = down;
        event.vk = data->vkCode;
        event.scan = data->scanCode;
        event.alt = (data->flags & LLKHF_ALTDOWN) != 0;
        event.control = async_key_down(VK_CONTROL);
        event.shift = async_key_down(VK_SHIFT);
        event.super = async_key_down(VK_LWIN) || async_key_down(VK_RWIN);
        self->enqueue(event);
      }
    }
  }
  return CallNextHookEx(nullptr, code, wparam, lparam);
}

LRESULT CALLBACK InputService::Impl::mouse_proc(const int code, const WPARAM wparam,
                                                const LPARAM lparam) {
  if (code == HC_ACTION) {
    Impl* self = owner.load(std::memory_order_acquire);
    if (self) {
      const auto* data = reinterpret_cast<const MSLLHOOKSTRUCT*>(lparam);
      InputEvent event;
      event.kind = InputEventKind::Mouse;
      event.timestamp_ms = data->time;
      event.injected = (data->flags & LLMHF_INJECTED) != 0;
      event.x = data->pt.x;
      event.y = data->pt.y;
      bool valid = false;
      switch (wparam) {
        case WM_MOUSEMOVE:
          event.mouse_action = MouseAction::Move;
          valid = true;
          break;
        case WM_LBUTTONDOWN:
          event.mouse_action = MouseAction::Down;
          event.button = 1;
          valid = true;
          break;
        case WM_LBUTTONUP:
          event.mouse_action = MouseAction::Up;
          event.button = 1;
          valid = true;
          break;
        case WM_RBUTTONDOWN:
          event.mouse_action = MouseAction::Down;
          event.button = 2;
          valid = true;
          break;
        case WM_RBUTTONUP:
          event.mouse_action = MouseAction::Up;
          event.button = 2;
          valid = true;
          break;
        case WM_MBUTTONDOWN:
          event.mouse_action = MouseAction::Down;
          event.button = 3;
          valid = true;
          break;
        case WM_MBUTTONUP:
          event.mouse_action = MouseAction::Up;
          event.button = 3;
          valid = true;
          break;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
          event.mouse_action = MouseAction::Wheel;
          event.wheel_delta = static_cast<std::int32_t>(GET_WHEEL_DELTA_WPARAM(wparam));
          valid = true;
          break;
        default:
          break;
      }
      if (valid) self->enqueue(event);
    }
  }
  return CallNextHookEx(nullptr, code, wparam, lparam);
}

InputService::InputService() : impl_(std::make_unique<Impl>()) {}
InputService::~InputService() { (void)stop(); }

rime::core::Error InputService::start() {
  std::unique_lock lock(impl_->mutex);
  if (impl_->state != InputServiceState::Created) {
    return {rime::core::Error::Code::InvalidState, "input service can only start once"};
  }
  Impl* expected = nullptr;
  if (!Impl::owner.compare_exchange_strong(expected, impl_.get())) {
    return {rime::core::Error::Code::ExecutionFailed,
            "another input service is already running"};
  }
  impl_->state = InputServiceState::Running;
  impl_->stopping = false;
  impl_->ready = false;
  impl_->install_ok = false;
  try {
    impl_->thread = std::thread(&Impl::thread_main, impl_.get());
  } catch (...) {
    Impl::owner.store(nullptr);
    impl_->state = InputServiceState::Stopped;
    return {rime::core::Error::Code::ExecutionFailed, "cannot start the input hook thread"};
  }
  // Hooks install inside the thread; wait for its verdict.
  if (!impl_->condition.wait_for(lock, kInstallTimeout,
                                 [this] { return impl_->ready; })) {
    impl_->stopping = true;
    const DWORD thread_id = impl_->thread_id;
    lock.unlock();
    if (thread_id != 0) PostThreadMessageW(thread_id, kQuitMessage, 0, 0);
    if (impl_->thread.joinable()) impl_->thread.join();
    lock.lock();
    impl_->state = InputServiceState::Stopped;
    return {rime::core::Error::Code::ExecutionFailed, "input hook installation timed out"};
  }
  if (!impl_->install_ok) {
    lock.unlock();
    if (impl_->thread.joinable()) impl_->thread.join();
    lock.lock();
    impl_->state = InputServiceState::Stopped;
    return {rime::core::Error::Code::ExecutionFailed,
            "cannot install low-level keyboard/mouse hooks"};
  }
  return rime::core::Error::none();
}

rime::core::Error InputService::stop() {
  std::unique_lock lock(impl_->mutex);
  if (impl_->state == InputServiceState::Stopped) return rime::core::Error::none();
  if (impl_->state == InputServiceState::Created) {
    impl_->state = InputServiceState::Stopped;
    return rime::core::Error::none();
  }
  if (!impl_->ready) {
    // stop() raced the install phase: wait it out on the same lock.
    impl_->condition.wait(lock, [this] { return impl_->ready; });
  }
  if (impl_->state == InputServiceState::Stopped) return rime::core::Error::none();
  impl_->stopping = true;
  impl_->state = InputServiceState::Stopping;
  std::thread worker = std::move(impl_->thread);
  const DWORD thread_id = impl_->thread_id;
  lock.unlock();
  if (thread_id != 0) PostThreadMessageW(thread_id, kQuitMessage, 0, 0);
  if (worker.joinable()) worker.join();
  lock.lock();
  impl_->state = InputServiceState::Stopped;
  return rime::core::Error::none();
}

InputServiceState InputService::state() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->state;
}

std::uint64_t InputService::subscribe(Callback callback) {
  if (!callback) return 0;
  std::lock_guard lock(impl_->mutex);
  if (impl_->state != InputServiceState::Running || impl_->stopping) return 0;
  auto entry = std::make_shared<Impl::Entry>();
  entry->id = impl_->next_id++;
  entry->callback = std::move(callback);
  impl_->entries.push_back(entry);
  return entry->id;
}

bool InputService::unsubscribe(const std::uint64_t id) {
  std::unique_lock lock(impl_->mutex);
  auto found = impl_->entries.end();
  for (auto it = impl_->entries.begin(); it != impl_->entries.end(); ++it) {
    if ((*it)->id == id) {
      found = it;
      break;
    }
  }
  if (found == impl_->entries.end()) return false;
  const auto entry = *found;
  entry->closed = true;
  if (entry->in_flight > 0 && impl_->thread_id != GetCurrentThreadId()) {
    entry->idle.wait(lock, [&entry] { return entry->in_flight == 0; });
  }
  impl_->entries.erase(found);
  return true;
}

std::size_t InputService::subscription_count() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->entries.size();
}

std::uint64_t InputService::dropped_events() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->dropped;
}

}  // namespace rime::win32
