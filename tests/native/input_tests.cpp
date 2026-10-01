// Needs an interactive desktop, exclusive run: injects real keys/mouse and hooks global input.
#include "rime/win32/input.hpp"

#include <windows.h>

#include <cassert>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using rime::win32::InputEvent;
using rime::win32::InputEventKind;
using rime::win32::InputService;
using rime::win32::MouseAction;

void send_vk(const WORD virtual_key) {
  INPUT inputs[2]{};
  inputs[0].type = INPUT_KEYBOARD;
  inputs[0].ki.wVk = virtual_key;
  inputs[1].type = INPUT_KEYBOARD;
  inputs[1].ki.wVk = virtual_key;
  inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
  // NOTE: hoisted out of assert() so the SendInput side effect still runs
  // under NDEBUG where assert() is compiled out.
  const UINT sent = SendInput(2, inputs, sizeof(INPUT));
  assert(sent == 2);
}

void send_mouse_to(const int x, const int y) {
  const int width = GetSystemMetrics(SM_CXSCREEN);
  const int height = GetSystemMetrics(SM_CYSCREEN);
  assert(width > 1 && height > 1);
  INPUT input{};
  input.type = INPUT_MOUSE;
  input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
  input.mi.dx = static_cast<LONG>(x * 65535 / (width - 1));
  input.mi.dy = static_cast<LONG>(y * 65535 / (height - 1));
  // NOTE: hoisted out of assert() so the SendInput side effect still runs
  // under NDEBUG where assert() is compiled out.
  const UINT sent = SendInput(1, &input, sizeof(INPUT));
  assert(sent == 1);
}

template <typename Predicate>
bool wait_for(Predicate predicate, const std::chrono::milliseconds timeout = 3s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(10ms);
  }
  return predicate();
}

}  // namespace

using rime::win32::InputEvent;
using rime::win32::InputEventKind;
using rime::win32::InputService;
using rime::win32::MouseAction;

int main() {
  InputService service;

  // Work before start is rejected.
  assert(service.subscribe([](const InputEvent&) {}) == 0);
  assert(!service.send({{VK_F24, true}}).ok());
  assert(service.start().ok());
  assert(!service.start().ok());  // start-once
  assert(service.state() == rime::win32::InputServiceState::Running);

  std::mutex mutex;
  std::vector<InputEvent> events;
  const auto subscription = service.subscribe([&](const InputEvent& event) {
    std::lock_guard lock(mutex);
    events.push_back(event);
  });
  assert(subscription != 0);
  assert(service.subscription_count() == 1);
  assert(service.subscribe(nullptr) == 0);

  const auto has_key = [&](const std::uint32_t vk, const bool down) {
    std::lock_guard lock(mutex);
    for (const auto& event : events) {
      if (event.kind == InputEventKind::Key && event.vk == vk && event.key_down == down) {
        return true;
      }
    }
    return false;
  };

  const auto has_self_key = [&](const std::uint32_t vk, const bool down) {
    std::lock_guard lock(mutex);
    for (const auto& event : events) {
      if (event.kind == InputEventKind::Key && event.vk == vk && event.key_down == down &&
          event.self_injected) {
        return true;
      }
    }
    return false;
  };

  // A synthetic F24 press arrives as key down/up snapshots with sequences.
  send_vk(VK_F24);
  assert(wait_for([&] { return has_key(VK_F24, true) && has_key(VK_F24, false); }));
  {
    std::lock_guard lock(mutex);
    std::uint64_t previous = 0;
    bool ordered = false;
    for (const auto& event : events) {
      if (event.kind == InputEventKind::Key && event.vk == VK_F24) {
        assert(event.sequence > previous);
        previous = event.sequence;
        ordered = true;
      }
    }
    assert(ordered);
  }

  // send() enforces its contract and marks its own batch: the raw
  // send_vk() above stays injected-but-foreign, while the tagged batch is
  // observed as self input (chords must not re-trigger on it).
  {
    const auto empty = service.send({});
    assert(empty.code == rime::core::Error::Code::InvalidContract);
  }
  assert(service.send({{VK_F24, true}, {VK_F24, false}}).ok());
  assert(wait_for([&] { return has_self_key(VK_F24, true) && has_self_key(VK_F24, false); }));
  {
    std::lock_guard lock(mutex);
    bool foreign = false;
    bool self = false;
    for (const auto& event : events) {
      if (event.kind == InputEventKind::Key && event.vk == VK_F24 && event.injected) {
        if (event.self_injected) {
          self = true;
        } else {
          foreign = true;
        }
      }
    }
    assert(self && foreign);
  }

  // A synthetic absolute move arrives with the exact coordinates.
  POINT original{};
  // NOTE: hoisted out of assert(): GetCursorPos has a side effect (writes
  // `original`) that must run even when NDEBUG compiles assert() out.
  const BOOL got_pos = GetCursorPos(&original);
  if (got_pos == FALSE) return 1;
  send_mouse_to(321, 123);
  assert(wait_for([&] {
    std::lock_guard lock(mutex);
    for (const auto& event : events) {
      if (event.kind == InputEventKind::Mouse && event.mouse_action == MouseAction::Move &&
          event.x == 321 && event.y == 123) {
        return true;
      }
    }
    return false;
  }));
  // NOTE: hoisted out of assert(): SetCursorPos has a side effect (moves the
  // cursor) that must run even when NDEBUG compiles assert() out.
  const BOOL restored = SetCursorPos(original.x, original.y);
  if (restored == FALSE) return 1;

  // Unsubscribe closes the subscription; later events are not recorded.
  assert(service.unsubscribe(subscription));
  assert(service.subscription_count() == 0);
  assert(!service.unsubscribe(subscription));      // closed
  assert(!service.unsubscribe(9'000'000));          // unknown
  std::size_t recorded = 0;
  {
    std::lock_guard lock(mutex);
    recorded = events.size();
  }
  send_vk(VK_F23);
  std::this_thread::sleep_for(300ms);
  {
    std::lock_guard lock(mutex);
    assert(events.size() == recorded);
  }

  // Stop is repeatable; new subscriptions are refused afterwards.
  assert(service.stop().ok());
  assert(service.stop().ok());
  assert(service.state() == rime::win32::InputServiceState::Stopped);
  assert(service.subscribe([](const InputEvent&) {}) == 0);
  assert(!service.send({{VK_F24, true}}).ok());

  // A second service takes over after the first stopped.
  InputService second;
  assert(second.start().ok());

  // While it runs, a concurrent service is refused.
  InputService third;
  const auto refused = third.start();
  assert(!refused.ok());
  assert(third.state() == rime::win32::InputServiceState::Created);

  assert(second.stop().ok());
  assert(third.start().ok());
  assert(third.stop().ok());

  return 0;
}
