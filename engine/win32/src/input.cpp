#include "rime/win32/input.hpp"

#include "rime/win32/input_probe.hpp"
#include "rime/win32/input_seam.hpp"

#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace rime::win32 {
namespace {

constexpr UINT kQuitMessage = WM_APP + 77;
constexpr UINT kWakeMessage = WM_APP + 78;
constexpr UINT kHookControlMessage = WM_APP + 79;
constexpr std::size_t kPendingCapacity = 2048;
constexpr auto kInstallTimeout = std::chrono::seconds(2);

// Test-only OS substitution (input_seam.hpp): the default is the real API,
// a test swaps in a failure and swaps back with nullptr. Read through
// atomic loads at the call sites so a swap is visible across threads.
std::atomic<input_seam::HookInstaller> g_hook_installer{::SetWindowsHookExW};
std::atomic<input_seam::SendInputFn> g_send_input{::SendInput};

bool async_key_down(const int virtual_key) {
  return (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
}

// Extended-key list per SendInput docs: without KEYEVENTF_EXTENDEDKEY these
// arrive with the wrong scan semantics (arrows collapse to the numeric pad,
// right-hand modifiers to their left twins).
bool is_extended_vk(const std::uint32_t vk) {
  switch (vk) {
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_LEFT:
    case VK_UP:
    case VK_RIGHT:
    case VK_DOWN:
    case VK_NUMLOCK:
    case VK_SCROLL:
    case VK_RCONTROL:
    case VK_RMENU:
    case VK_LWIN:
    case VK_RWIN:
    case VK_APPS:
      return true;
    default:
      return false;
  }
}

// AHK MOUSE_COORD_TO_ABS (keyboard_mouse.cpp, MouseMove conversion comment):
//   ((65536 * coord) / extent) + (coord < 0 ? -1 : 1)
// Applied against SM_CXSCREEN/SM_CYSCREEN like AHK's primary-only SendInput
// path; the +-1 avoids snapping pixel 0 to a rounded-down boundary. The
// 64-bit intermediate keeps extreme coordinates free of signed overflow
// (the in-range result matches the 32-bit AHK expression).
int mouse_coord_to_abs(const int coord, const int extent) {
  if (extent <= 0) return 0;
  const auto scaled = (65536ll * coord) / extent;
  return static_cast<int>(scaled) + (coord < 0 ? -1 : 1);
}

}  // namespace

namespace input_seam {

void set_hook_installer(const HookInstaller installer) {
  g_hook_installer.store(installer ? installer : ::SetWindowsHookExW,
                         std::memory_order_release);
}

void set_send_input(const SendInputFn sender) {
  g_send_input.store(sender ? sender : ::SendInput, std::memory_order_release);
}

}  // namespace input_seam

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

  // Physical down snapshot (one atomic per vk): the hook thread writes it,
  // any thread reads it. Seeded from GetAsyncKeyState in thread_main right
  // after the hooks install, before start() reports Running.
  std::array<std::atomic<bool>, 256> physical{};
  // BlockInput flag: read on the hook path, set from the JS thread, cleared
  // unconditionally by stop() so shutdown can never leave the desktop
  // blocked.
  std::atomic<bool> blocked{false};
  // Lock-key force (AHK g_ForceKeyLock family, script2.cpp:1768 SetToggleState):
  // 0 neutral, +1 always-on, -1 always-off. Set from the JS thread, read inside
  // the low-level keyboard hook; only VK_CAPITAL/VK_NUMLOCK/VK_SCROLL are ever
  // armed (the JS module validates first). stop() clears it like blocked so
  // shutdown can never leave a key force-locked.
  std::array<std::atomic<std::int8_t>, 256> force_toggle{};
  // KeyHistory ring, guarded by mutex (recorded inside enqueue, copied by
  // key_history(); capacity 0 disables recording).
  std::deque<KeyHistoryEntry> history;
  std::size_t history_capacity{40};  // AHK g_MaxHistoryKeys default

  // Send level stamped into every injected batch (AHK g.SendLevel, 0..100):
  // written from the JS thread, read while send()/send_mouse() compile a
  // batch, decoded back off dwExtraInfo on the hook path. Left alone by
  // stop() - it shapes this process's own tags, it cannot leave the desktop
  // in a bad state the way blocked/force_toggle can.
  std::atomic<std::uint32_t> send_level{0};

  // Serializes send() batches against each other; stop() joins an in-flight
  // batch before unhooking so a refused-later send can never inject into a
  // desktop the hooks no longer observe.
  std::mutex send_mutex;

  // Hook-thread only.
  HHOOK keyboard_hook{nullptr};
  HHOOK mouse_hook{nullptr};

  // InstallKeybdHook/InstallMouseHook state: `wanted` is the requested
  // state (any thread writes, hook thread applies), `installed` mirrors the
  // hook-thread reality (any thread reads). Waiters use their own mutex so
  // a control wait never holds `mutex` (enqueue runs inside the hook
  // callback and must not stall the desktop).
  std::atomic<bool> keyboard_wanted{true};
  std::atomic<bool> mouse_wanted{true};
  std::atomic<bool> keyboard_installed{false};
  std::atomic<bool> mouse_installed{false};
  struct HookControl {
    bool done{false};
  };
  std::mutex control_mutex;
  std::condition_variable control_condition;
  std::vector<std::shared_ptr<HookControl>> hook_controls;  // guarded by control_mutex

  // The single running service owns the low-level hook callbacks (they
  // receive no per-hook user pointer).
  static std::atomic<Impl*> owner;

  // Seeds the physical down snapshot after a (re)install so keys held while
  // the hook was away are not reported as newly pressed.
  void seed_physical() {
    for (int vk = 1; vk <= 0xFE; ++vk) {
      physical[static_cast<std::size_t>(vk)].store((GetAsyncKeyState(vk) & 0x8000) != 0);
    }
  }

  // Hook thread: reconciles installed hooks with the wanted flags. A failed
  // install leaves wanted=true but installed=false (reported to waiters).
  void apply_hooks() {
    const auto install = g_hook_installer.load(std::memory_order_acquire);
    const bool want_keyboard = keyboard_wanted.load(std::memory_order_acquire);
    if (want_keyboard && !keyboard_hook) {
      keyboard_hook = install(WH_KEYBOARD_LL, keyboard_proc, GetModuleHandleW(nullptr), 0);
      if (keyboard_hook) seed_physical();
    } else if (!want_keyboard && keyboard_hook) {
      UnhookWindowsHookEx(keyboard_hook);
      keyboard_hook = nullptr;
    }
    const bool want_mouse = mouse_wanted.load(std::memory_order_acquire);
    if (want_mouse && !mouse_hook) {
      mouse_hook = install(WH_MOUSE_LL, mouse_proc, GetModuleHandleW(nullptr), 0);
    } else if (!want_mouse && mouse_hook) {
      UnhookWindowsHookEx(mouse_hook);
      mouse_hook = nullptr;
    }
    keyboard_installed.store(keyboard_hook != nullptr, std::memory_order_release);
    mouse_installed.store(mouse_hook != nullptr, std::memory_order_release);
  }

  // Hook thread: releases every waiter (control message or shutdown).
  void finish_hook_controls() {
    std::lock_guard lock(control_mutex);
    for (auto& control : hook_controls) control->done = true;
    hook_controls.clear();
    control_condition.notify_all();
  }

  void enqueue(InputEvent event) {
    DWORD hook_thread = 0;
    {
      std::lock_guard lock(mutex);
      event.sequence = next_sequence++;
      if (pending.size() >= kPendingCapacity) {
        pending.erase(pending.begin());
        ++dropped;
      }
      // Record into the history ring before the move below: the ring keeps
      // its own bounded capacity, independent of the delivery queue.
      record_history(event);
      pending.push_back(std::move(event));
      // Snapshot thread_id under the same lock: it is written by thread_main
      // and cleared on shutdown, so an unlocked read races.
      hook_thread = thread_id;
    }
    // The delivery loop drains pending only when GetMessage returns; wake it.
    if (hook_thread != 0) PostThreadMessageW(hook_thread, kWakeMessage, 0, 0);
    rime::win32::input_probe::internal::on_enqueue_done();
  }

  // Appends one history row (mutex held): key events always, mouse buttons
  // as their VK_LBUTTON/VK_RBUTTON/VK_MBUTTON equivalents; moves and wheel
  // are not key history. The ring keeps the newest history_capacity rows.
  void record_history(const InputEvent& event) {
    if (history_capacity == 0) return;
    std::uint32_t vk = 0;
    bool down = false;
    if (event.kind == InputEventKind::Key) {
      vk = event.vk;
      down = event.key_down;
    } else if (event.mouse_action == MouseAction::Down || event.mouse_action == MouseAction::Up) {
      // Button numbering (1 left, 2 right, 3 middle) matches the VK codes.
      vk = event.button;
      down = event.mouse_action == MouseAction::Down;
    } else {
      return;
    }
    if (vk == 0 || vk > 255) return;
    KeyHistoryEntry entry;
    entry.vk = vk;
    entry.scan = event.scan;
    entry.down = down;
    entry.injected = event.injected;
    entry.self_injected = event.self_injected;
    entry.timestamp_ms = event.timestamp_ms;
    // 32-bit GetMessageTime-style clock: a backwards jump across the wrap
    // reports 0 instead of a huge forward delta.
    if (!history.empty() && event.timestamp_ms >= history.back().timestamp_ms) {
      entry.elapsed_ms = event.timestamp_ms - history.back().timestamp_ms;
    }
    history.push_back(entry);
    while (history.size() > history_capacity) history.pop_front();
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
      if (event.kind == InputEventKind::Key) {
        rime::win32::input_probe::internal::on_deliver(event.vk, event.key_down,
                                                       event.self_injected);
      }
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
  apply_hooks();
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
    // GetMessageW returns -1 on error and 0 for WM_QUIT; both exit the pump
    // the same way. The error detail (GetLastError) is deliberately not acted
    // on here to keep the pump shape unchanged; hook cleanup below still runs
    // on either path.
    if (received <= 0 || message.message == kQuitMessage) break;
    {
      std::lock_guard lock(mutex);
      if (stopping) break;
    }
    // InstallKeybdHook/InstallMouseHook: reconcile hooks before delivering,
    // so events queued after the control message observe the new state.
    if (message.message == kHookControlMessage) {
      apply_hooks();
      finish_hook_controls();
      continue;
    }
    deliver_events();
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  cleanup_hooks();
  keyboard_installed.store(false, std::memory_order_release);
  mouse_installed.store(false, std::memory_order_release);
  // A waiter blocked on a control message must not outlive the pump.
  finish_hook_controls();
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
        // The keyboard hook preserves all 64 bits, so the tag is matched in
        // full - with the level folded in as marker - level (the high "Rime"
        // half never changes for level 0..100).
        const auto extra = static_cast<std::uint64_t>(data->dwExtraInfo);
        event.self_injected = event.injected && is_self_injected_marker(extra);
        event.send_level =
            event.self_injected ? decode_send_level(static_cast<std::uint32_t>(extra)) : 0;
        event.key_down = down;
        event.vk = data->vkCode;
        event.scan = data->scanCode;
        event.alt = (data->flags & LLKHF_ALTDOWN) != 0;
        event.control = async_key_down(VK_CONTROL);
        event.shift = async_key_down(VK_SHIFT);
        event.super = async_key_down(VK_LWIN) || async_key_down(VK_RWIN);
        rime::win32::input_probe::internal::on_hook_event(event.vk, down, event.self_injected);
        self->enqueue(event);
        if (data->vkCode < 256) {
          self->physical[static_cast<std::size_t>(data->vkCode)].store(down);
        }
        // Lock-key force: while a direction is armed the key stays hidden
        // from the OS - the event is already recorded (history, snapshot and
        // subscriptions above), and both the press and the release are
        // swallowed like BlockInput, so a physical press can never move the
        // toggle. Self-injected batches pass: that is the path setLockState
        // uses to write the state (AHK pForceToggle -> SuppressThisKey,
        // hook.cpp:1904-1908).
        if (!event.self_injected && data->vkCode < 256 &&
            self->force_toggle[data->vkCode].load(std::memory_order_acquire) != 0) {
          return 1;  // swallowed like BlockInput: recorded above, invisible to the OS
        }
        // BlockInput: the event is already recorded (history, snapshot and
        // subscriptions above); returning 1 stops the hook chain so neither
        // the OS nor older hooks ever see it. Self-injected batches pass.
        if (self->blocked.load() && !event.self_injected) return 1;
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
      // WH_MOUSE_LL reports dwExtraInfo zero-extended from 32 bits on this
      // Windows (probed: 0xDEADBEEF00000042 arrives as 0x42) while the
      // keyboard hook preserves all 64 bits, so the tag is matched on its
      // low half here and in full on the keyboard path. The send level rides
      // inside that low half (marker - level), so it decodes identically on
      // both paths.
      const auto extra_low = static_cast<std::uint32_t>(data->dwExtraInfo);
      event.self_injected = event.injected && is_self_injected_marker32(extra_low);
      event.send_level = event.self_injected ? decode_send_level(extra_low) : 0;
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
      if (valid) {
        self->enqueue(event);
        // Button transitions maintain the physical snapshot (the button
        // numbering matches the VK_LBUTTON/VK_RBUTTON/VK_MBUTTON codes);
        // moves and wheel never change a key's down state.
        if (event.mouse_action == MouseAction::Down || event.mouse_action == MouseAction::Up) {
          if (event.button >= 1 && event.button <= 3) {
            self->physical[static_cast<std::size_t>(event.button)]
                .store(event.mouse_action == MouseAction::Down);
          }
        }
        // BlockInput: recorded above, then swallowed for moves, wheel and
        // buttons alike; self-injected input passes (same rule as keys).
        if (self->blocked.load() && !event.self_injected) return 1;
      }
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
  // Unconditional: even an early-return path must not leave the desktop
  // blocked (the hooks that enforce the flag are about to go away anyway).
  impl_->blocked.store(false);
  // Same rule for the lock-key force: only the three lock keys are ever
  // armed, so clearing just them is equivalent to a full sweep and leaves no
  // key forced after shutdown.
  for (const std::uint32_t vk : {0x14u, 0x90u, 0x91u}) {
    impl_->force_toggle[vk].store(0, std::memory_order_release);
  }
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
  // Wait out an in-flight send()/send_mouse() batch: it observed Running
  // before the state flip, so let it inject while the hooks are still
  // installed; every later batch observes Stopping and refuses.
  {
    std::lock_guard send_lock(impl_->send_mutex);
  }
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
  // Any subscription consumes both event streams, so a new one reinstalls
  // hooks a forced Install*Hook removed (AHK reinstalls on demand too).
  // Fire-and-forget: the next delivery observes the applied state; callers
  // that need a verdict use set_*_hook.
  impl_->keyboard_wanted.store(true, std::memory_order_release);
  impl_->mouse_wanted.store(true, std::memory_order_release);
  if (impl_->thread_id != 0) {
    PostThreadMessageW(impl_->thread_id, kHookControlMessage, 0, 0);
  }
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
  // impl_->thread_id is read under impl_->mutex here (the lock is held), and
  // enqueue() snapshots it under the same lock, so neither races thread_main.
  const bool called_on_hook_thread =
      impl_->thread_id != 0 && impl_->thread_id == GetCurrentThreadId();
  if (entry->in_flight > 0 && !called_on_hook_thread) {
    // Bounded wait: a wedged subscriber must not hang unsubscribe forever.
    // On timeout the entry is dropped and failure is reported so the caller
    // can diagnose the stuck callback.
    constexpr auto kDrainTimeout = std::chrono::seconds(5);
    const bool drained = entry->idle.wait_for(
        lock, kDrainTimeout, [&entry] { return entry->in_flight == 0; });
    if (!drained) {
      impl_->entries.erase(found);
      return false;
    }
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

bool InputService::control_hook(const bool keyboard, const bool install, const bool force) {
  std::atomic<bool>* wanted = keyboard ? &impl_->keyboard_wanted : &impl_->mouse_wanted;
  std::atomic<bool>* installed =
      keyboard ? &impl_->keyboard_installed : &impl_->mouse_installed;
  std::uint32_t thread_id = 0;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->state != InputServiceState::Running || impl_->stopping || !impl_->ready) {
      return installed->load(std::memory_order_acquire);
    }
    thread_id = impl_->thread_id;
    if (install) {
      wanted->store(true, std::memory_order_release);
    } else if (force || impl_->entries.empty()) {
      // Force (AHK's unconditional remove) or nothing left that needs input.
      wanted->store(false, std::memory_order_release);
    }
    // else: subscribers still need this stream; the hook stays (AHK keeps
    // hooks while hotkeys/hotstrings exist).
  }
  if (thread_id == 0) return installed->load(std::memory_order_acquire);

  const auto control = std::make_shared<Impl::HookControl>();
  {
    std::lock_guard lock(impl_->control_mutex);
    impl_->hook_controls.push_back(control);
  }
  if (!PostThreadMessageW(thread_id, kHookControlMessage, 0, 0)) {
    std::lock_guard lock(impl_->control_mutex);
    for (auto it = impl_->hook_controls.begin(); it != impl_->hook_controls.end(); ++it) {
      if (*it == control) {
        impl_->hook_controls.erase(it);
        break;
      }
    }
    return installed->load(std::memory_order_acquire);
  }
  // Wait on the control mutex, never on `mutex`: enqueue() runs inside the
  // low-level hook callback and must not stall behind this wait.
  std::unique_lock lock(impl_->control_mutex);
  (void)impl_->control_condition.wait_for(lock, kInstallTimeout,
                                          [&control] { return control->done; });
  return installed->load(std::memory_order_acquire);
}

bool InputService::set_keyboard_hook(const bool install, const bool force) {
  return control_hook(true, install, force);
}

bool InputService::set_mouse_hook(const bool install, const bool force) {
  return control_hook(false, install, force);
}

bool InputService::keyboard_hook_installed() const {
  return impl_->keyboard_installed.load(std::memory_order_acquire);
}

bool InputService::mouse_hook_installed() const {
  return impl_->mouse_installed.load(std::memory_order_acquire);
}

rime::core::Error InputService::send(const std::vector<SendKeyEvent>& keys) {
  rime::win32::input_probe::internal::on_send_entry();
  if (keys.empty()) {
    return {rime::core::Error::Code::InvalidContract, "send requires at least one key step"};
  }
  for (const auto& step : keys) {
    if (step.unicode) {
      if (step.vk > 0xFFFF) {
        return {rime::core::Error::Code::InvalidContract,
                "unicode key steps require a UTF-16 code unit in 0..65535"};
      }
    } else if (step.vk == 0 || step.vk > 0xFE) {
      return {rime::core::Error::Code::InvalidContract,
              "send key steps require a virtual key in 1..254"};
    }
  }
  std::lock_guard send_lock(impl_->send_mutex);
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->state != InputServiceState::Running || impl_->stopping) {
      return {rime::core::Error::Code::InvalidState, "input service is not running"};
    }
  }
  // One tag for the whole batch: the level is read once, so a concurrent
  // set_send_level() cannot split one batch across two levels.
  const ULONG_PTR extra_info = static_cast<ULONG_PTR>(
      self_injected_marker_for(impl_->send_level.load(std::memory_order_acquire)));
  std::vector<INPUT> inputs;
  inputs.reserve(keys.size());
  for (const auto& step : keys) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    if (step.unicode) {
      // KEYEVENTF_UNICODE packet (AHK SendUnicodeChar): the code unit rides
      // in wScan, no VK, no scan mapping, no extended-key bit. The hook then
      // reports vk 231 (0xE7, Windows' unicode-injection placeholder) with
      // scan = the code unit - probed on this build for ASCII, Latin-1, CJK
      // and surrogate units - which no chord can match (chords resolve real
      // virtual keys only).
      input.ki.wVk = 0;
      input.ki.wScan = static_cast<WORD>(step.vk);
      input.ki.dwFlags = static_cast<DWORD>(KEYEVENTF_UNICODE) |
                         (step.down ? 0u : static_cast<DWORD>(KEYEVENTF_KEYUP));
    } else {
      input.ki.wVk = static_cast<WORD>(step.vk);
      input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(step.vk, MAPVK_VK_TO_VSC));
      input.ki.dwFlags = (step.down ? 0u : static_cast<DWORD>(KEYEVENTF_KEYUP)) |
                         (is_extended_vk(step.vk) ? static_cast<DWORD>(KEYEVENTF_EXTENDEDKEY) : 0u);
    }
    input.ki.dwExtraInfo = extra_info;
    inputs.push_back(input);
  }
  rime::win32::input_probe::internal::on_pre_inject();
  const UINT sent = g_send_input.load(std::memory_order_acquire)(
      static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
  rime::win32::input_probe::internal::on_post_inject();
  if (sent != inputs.size()) {
    return {rime::core::Error::Code::ExecutionFailed,
            "SendInput injected " + std::to_string(sent) + " of " +
                std::to_string(inputs.size()) + " key steps"};
  }
  return rime::core::Error::none();
}

rime::core::Error InputService::send_mouse(const std::vector<SendMouseStep>& steps) {
  if (steps.empty()) {
    return {rime::core::Error::Code::InvalidContract, "send_mouse requires at least one step"};
  }
  for (const auto& step : steps) {
    if (step.action != SendMouseAction::Move && step.action != SendMouseAction::RelMove &&
        step.action != SendMouseAction::Down && step.action != SendMouseAction::Up) {
      return {rime::core::Error::Code::InvalidContract, "send_mouse step action is unknown"};
    }
    if ((step.action == SendMouseAction::Down || step.action == SendMouseAction::Up) &&
        (step.button < 1 || step.button > 3)) {
      return {rime::core::Error::Code::InvalidContract,
              "send_mouse buttons must be 1 (left), 2 (right) or 3 (middle)"};
    }
  }
  std::lock_guard send_lock(impl_->send_mutex);
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->state != InputServiceState::Running || impl_->stopping) {
      return {rime::core::Error::Code::InvalidState, "input service is not running"};
    }
  }
  const int width = GetSystemMetrics(SM_CXSCREEN);
  const int height = GetSystemMetrics(SM_CYSCREEN);
  // AHK v2 translates logical L/R into physical through SM_SWAPBUTTON
  // (keyboard_mouse.cpp:2190) before choosing the event flags; middle is
  // never swapped. We always inject in SendInput mode, so the swap applies
  // to every button batch (SendPlay would be exempt).
  const bool swap_buttons = GetSystemMetrics(SM_SWAPBUTTON) != 0;
  // One tag for the whole batch, exactly like send(): the level is read once
  // so a concurrent set_send_level() cannot split a batch across two levels.
  const ULONG_PTR extra_info = static_cast<ULONG_PTR>(
      self_injected_marker_for(impl_->send_level.load(std::memory_order_acquire)));
  std::vector<INPUT> inputs;
  inputs.reserve(steps.size());
  for (const auto& step : steps) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    switch (step.action) {
      case SendMouseAction::Move:
        input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
        input.mi.dx = mouse_coord_to_abs(step.x, width);
        input.mi.dy = mouse_coord_to_abs(step.y, height);
        break;
      case SendMouseAction::RelMove:
        // Raw deltas: SendInput applies them in batch order, so mixed
        // absolute/relative sequences need no cursor prediction here.
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        input.mi.dx = step.x;
        input.mi.dy = step.y;
        break;
      case SendMouseAction::Down:
      case SendMouseAction::Up: {
        std::uint32_t button = step.button;
        if (swap_buttons && (button == 1 || button == 2)) button = 3 - button;
        const bool up = step.action == SendMouseAction::Up;
        if (button == 1) {
          input.mi.dwFlags = up ? MOUSEEVENTF_LEFTUP : MOUSEEVENTF_LEFTDOWN;
        } else if (button == 2) {
          input.mi.dwFlags = up ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_RIGHTDOWN;
        } else {
          input.mi.dwFlags = up ? MOUSEEVENTF_MIDDLEUP : MOUSEEVENTF_MIDDLEDOWN;
        }
        break;
      }
      default:
        return {rime::core::Error::Code::InvalidContract, "send_mouse step action is unknown"};
    }
    input.mi.dwExtraInfo = extra_info;
    inputs.push_back(input);
  }
  const UINT sent = g_send_input.load(std::memory_order_acquire)(
      static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
  if (sent != inputs.size()) {
    return {rime::core::Error::Code::ExecutionFailed,
            "SendInput injected " + std::to_string(sent) + " of " +
                std::to_string(inputs.size()) + " mouse steps"};
  }
  return rime::core::Error::none();
}

ModifierState read_modifier_state() {
  ModifierState state;
  state.lcontrol = async_key_down(VK_LCONTROL);
  state.rcontrol = async_key_down(VK_RCONTROL);
  state.lshift = async_key_down(VK_LSHIFT);
  state.rshift = async_key_down(VK_RSHIFT);
  state.lalt = async_key_down(VK_LMENU);
  state.ralt = async_key_down(VK_RMENU);
  state.lwin = async_key_down(VK_LWIN);
  state.rwin = async_key_down(VK_RWIN);
  // Toggle bit of GetKeyState: the CapsLock LED state (0/1), not the
  // thread-queued down bit.
  state.caps_lock = (GetKeyState(VK_CAPITAL) & 1) != 0;
  return state;
}

bool read_key_state(const std::uint32_t vk, const KeyStateType type) {
  if (vk == 0 || vk > 0xFE) return false;
  if (type == KeyStateType::Toggle) {
    // Toggle bit: probed to track reality even on non-pumping threads.
    return (GetKeyState(static_cast<int>(vk)) & 1) != 0;
  }
  // Logical (and the no-service Physical fallback): GetAsyncKeyState, the
  // only down-bit source that is correct on threads without a message pump
  // (the JS thread) and the closest match to "what the OS currently has".
  return async_key_down(static_cast<int>(vk));
}

bool InputService::physical_key_down(const std::uint32_t vk) const {
  if (vk == 0 || vk > 0xFE) return false;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->state == InputServiceState::Running) {
      return impl_->physical[static_cast<std::size_t>(vk)].load();
    }
  }
  // Not running: no hook maintains the snapshot, read the OS directly.
  return async_key_down(static_cast<int>(vk));
}

void InputService::set_blocked(const bool blocked) { impl_->blocked.store(blocked); }

bool InputService::blocked() const { return impl_->blocked.load(); }

void InputService::set_force_toggle(const std::uint32_t vk, const std::int8_t state) {
  if (vk < 256) impl_->force_toggle[vk].store(state, std::memory_order_release);
}

std::vector<KeyHistoryEntry> InputService::key_history() const {
  std::lock_guard lock(impl_->mutex);
  return std::vector<KeyHistoryEntry>(impl_->history.begin(), impl_->history.end());
}

void InputService::set_key_history_capacity(const std::size_t capacity) {
  std::lock_guard lock(impl_->mutex);
  // The JS layer rejects out-of-range values with a TypeError; clamp here
  // so native callers cannot grow the ring past AHK's 500-row ceiling.
  impl_->history_capacity = capacity > 500 ? 500 : capacity;
  while (impl_->history.size() > impl_->history_capacity) impl_->history.pop_front();
}

std::size_t InputService::key_history_capacity() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->history_capacity;
}

std::uint32_t InputService::send_level() const {
  return impl_->send_level.load(std::memory_order_acquire);
}

void InputService::set_send_level(const std::uint32_t level) {
  // The JS layer rejects anything outside 0..k_send_level_max with a
  // TypeError; clamp here so a native caller cannot stamp a level the marker
  // window cannot carry (same contract as set_key_history_capacity).
  impl_->send_level.store(level > k_send_level_max ? k_send_level_max : level,
                          std::memory_order_release);
}

}  // namespace rime::win32
