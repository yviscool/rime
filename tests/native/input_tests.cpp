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

// One raw transition with no paired counterpart: the block tests need to
// hold a key down across polls. Raw SendInput carries no self marker, so
// the hook treats it as foreign input.
void send_key_state(const WORD virtual_key, const bool down) {
  INPUT input{};
  input.type = INPUT_KEYBOARD;
  input.ki.wVk = virtual_key;
  if (!down) input.ki.dwFlags = KEYEVENTF_KEYUP;
  // NOTE: hoisted out of assert() so the SendInput side effect still runs
  // under NDEBUG where assert() is compiled out.
  const UINT sent = SendInput(1, &input, sizeof(INPUT));
  assert(sent == 1);
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
  assert(!service.send_mouse({{rime::win32::SendMouseAction::Move, 0, 0, 1}}).ok());
  // State surfaces before start: the physical snapshot falls back to
  // GetAsyncKeyState, the history ring starts at AHK's default capacity of
  // 40 (empty), and nothing is blocked.
  assert(service.physical_key_down(VK_F24) ==
         ((GetAsyncKeyState(VK_F24) & 0x8000) != 0));
  assert(service.key_history_capacity() == 40);
  assert(service.key_history().empty());
  assert(!service.blocked());
  assert(service.start().ok());
  assert(!service.start().ok());  // start-once
  assert(service.state() == rime::win32::InputServiceState::Running);

  // read_modifier_state(): caps_lock mirrors the LED toggle bit and an
  // injected hold shows up on the correct side, then clears on release.
  assert(rime::win32::read_modifier_state().caps_lock ==
         ((GetKeyState(VK_CAPITAL) & 1) != 0));
  assert(service.send({{VK_LCONTROL, true}}).ok());
  assert(wait_for([&] { return rime::win32::read_modifier_state().lcontrol; }));
  assert(service.send({{VK_LCONTROL, false}}).ok());
  assert(wait_for([&] { return !rime::win32::read_modifier_state().lcontrol; }));

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

  // KEYEVENTF_UNICODE batches arrive as self-injected key events. Windows
  // reports them to the low-level hook as vk 231 (0xE7) with the UTF-16 code
  // unit in scanCode - probed on this build for ASCII, Latin-1, CJK and
  // surrogate units, all identical - so the batch is matched on vk + scan.
  assert(service.send({{0x4f60, true, true}, {0x4f60, false, true}}).ok());
  assert(wait_for([&] {
    std::lock_guard lock(mutex);
    bool pressed = false;
    bool released = false;
    for (const auto& event : events) {
      if (event.kind == InputEventKind::Key && event.vk == 231 && event.scan == 0x4f60 &&
          event.self_injected) {
        if (event.key_down) pressed = true;
        if (!event.key_down) released = true;
      }
    }
    return pressed && released;
  }));
  {
    const auto vk_range = service.send({{0, false, false}});
    assert(vk_range.code == rime::core::Error::Code::InvalidContract);
    const auto unicode_range = service.send({{65536, true, true}});
    assert(unicode_range.code == rime::core::Error::Code::InvalidContract);
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

  // send_mouse(): contract enforcement plus a real batch - absolute move,
  // relative move and a down/up pair - all tagged as self input. The move
  // targets tolerate the 65535/(extent-1) absolute rounding (<= 1px).
  {
    const auto empty = service.send_mouse({});
    assert(empty.code == rime::core::Error::Code::InvalidContract);
    const auto bad_button =
        service.send_mouse({{rime::win32::SendMouseAction::Down, 0, 0, 4}});
    assert(bad_button.code == rime::core::Error::Code::InvalidContract);
  }
  POINT batch_origin{};
  // NOTE: hoisted out of assert(): GetCursorPos writes batch_origin even
  // when NDEBUG compiles the assertion out.
  const BOOL got_origin = GetCursorPos(&batch_origin);
  if (got_origin == FALSE) return 1;
  // NOTE: the target must stay inside the smallest supported desktop: CI runs
  // 1024x768 (taskbar top at y=720) so y=650 clears both the tray and any
  // overscan clamp on taller dev machines.
  assert(
      service.send_mouse({{rime::win32::SendMouseAction::Move, 456, 650, 1}}).ok());
  assert(wait_for([&] {
    std::lock_guard lock(mutex);
    for (const auto& event : events) {
      if (event.kind == InputEventKind::Mouse && event.mouse_action == MouseAction::Move &&
          event.self_injected && event.x >= 455 && event.x <= 457 && event.y >= 649 &&
          event.y <= 651) {
        return true;
      }
    }
    return false;
  }));
  // Relative moves assert on the observed cursor delta. The desktop is
  // interactive (the physical cursor can move under us), so re-read the origin
  // and retry a few times: a genuine injection fault still fails every attempt.
  bool relative_ok = false;
  for (int attempt = 0; attempt < 5 && !relative_ok; ++attempt) {
    POINT before_relative{};
    const BOOL got_relative = GetCursorPos(&before_relative);
    if (got_relative == FALSE) return 1;
    assert(service.send_mouse({{rime::win32::SendMouseAction::RelMove, 5, -3, 1}}).ok());
    relative_ok = wait_for([&] {
      POINT now{};
      if (GetCursorPos(&now) == FALSE) return false;
      return now.x >= before_relative.x + 4 && now.x <= before_relative.x + 6 &&
             now.y >= before_relative.y - 4 && now.y <= before_relative.y - 2;
    });
  }
  assert(relative_ok);
  assert(service.send_mouse({{rime::win32::SendMouseAction::Down, 0, 0, 1}}).ok());
  assert(wait_for([&] {
    std::lock_guard lock(mutex);
    for (const auto& event : events) {
      if (event.kind == InputEventKind::Mouse && event.mouse_action == MouseAction::Down &&
          event.button == 1 && event.self_injected) {
        return true;
      }
    }
    return false;
  }));
  assert(service.send_mouse({{rime::win32::SendMouseAction::Up, 0, 0, 1}}).ok());
  assert(wait_for([&] {
    std::lock_guard lock(mutex);
    for (const auto& event : events) {
      if (event.kind == InputEventKind::Mouse && event.mouse_action == MouseAction::Up &&
          event.button == 1 && event.self_injected) {
        return true;
      }
    }
    return false;
  }));
  // NOTE: hoisted out of assert(): SetCursorPos has a side effect (moves the
  // cursor) that must run even when NDEBUG compiles the assertion out.
  const BOOL restored_batch = SetCursorPos(batch_origin.x, batch_origin.y);
  if (restored_batch == FALSE) return 1;

  // Physical snapshot: seeded from GetAsyncKeyState at install (compared
  // above), then updated by every event the hook sees - self batches
  // included - and cleared again on release.
  assert(service.physical_key_down(VK_F24) ==
         ((GetAsyncKeyState(VK_F24) & 0x8000) != 0));
  assert(service.send({{VK_F24, true}}).ok());
  assert(wait_for([&] { return service.physical_key_down(VK_F24); }));
  assert(service.send({{VK_F24, false}}).ok());
  assert(wait_for([&] { return !service.physical_key_down(VK_F24); }));

  // The free-function modes read their documented Win32 sources; the
  // out-of-range guard rejects vk 0 and anything past 254.
  assert(rime::win32::read_key_state(VK_F24, rime::win32::KeyStateType::Logical) ==
         ((GetAsyncKeyState(VK_F24) & 0x8000) != 0));
  assert(rime::win32::read_key_state(VK_F24, rime::win32::KeyStateType::Physical) ==
         ((GetAsyncKeyState(VK_F24) & 0x8000) != 0));
  assert(rime::win32::read_key_state(VK_CAPITAL, rime::win32::KeyStateType::Toggle) ==
         ((GetKeyState(VK_CAPITAL) & 1) != 0));
  assert(!rime::win32::read_key_state(0, rime::win32::KeyStateType::Logical));
  assert(!rime::win32::read_key_state(0x1FF, rime::win32::KeyStateType::Physical));

  // KeyHistory ring: resizing to 4 trims immediately; three F23 taps (six
  // events) then keep only the newest four, which proves both recording and
  // eviction (every remaining row is the F23 tap, nothing older survives).
  service.set_key_history_capacity(4);
  assert(service.key_history_capacity() == 4);
  assert(service.key_history().size() <= 4);
  for (int tap = 0; tap < 3; ++tap) {
    assert(service.send({{VK_F23, true}, {VK_F23, false}}).ok());
  }
  assert(wait_for([&] {
    const auto history = service.key_history();
    return history.size() == 4 && history.back().vk == VK_F23 && !history.back().down;
  }));
  for (const auto& entry : service.key_history()) {
    assert(entry.vk == VK_F23);
    assert(entry.injected);
    assert(entry.self_injected);
    assert(entry.timestamp_ms > 0);
  }
  // Capacity 0 turns recording off and clears the ring; 501 clamps to
  // AHK's 500-row ceiling; 40 restores the default.
  service.set_key_history_capacity(0);
  assert(service.key_history().empty());
  service.set_key_history_capacity(501);
  assert(service.key_history_capacity() == 500);
  service.set_key_history_capacity(40);
  assert(service.key_history_capacity() == 40);

  // BlockInput: foreign input is recorded by the hook (snapshot and
  // subscription both see it) but never reaches the OS - probed on this
  // build: a swallowed key leaves GetAsyncKeyState up and stops the hook
  // chain - while self-injected batches bypass the block.
  std::size_t events_before_block = 0;
  {
    std::lock_guard lock(mutex);
    events_before_block = events.size();
  }
  service.set_blocked(true);
  assert(service.blocked());
  send_key_state(VK_F24, true);
  // The hook saw the blocked press...
  assert(wait_for([&] { return service.physical_key_down(VK_F24); }));
  // ...the subscription recorded it (enqueue before swallow)...
  assert(wait_for([&] {
    std::lock_guard lock(mutex);
    if (events.size() <= events_before_block) return false;
    for (std::size_t index = events_before_block; index < events.size(); ++index) {
      const auto& event = events[index];
      if (event.kind == InputEventKind::Key && event.vk == VK_F24 && event.key_down) {
        return true;
      }
    }
    return false;
  }));
  // ...and the OS never did. SendInput has already returned, so the hook
  // verdict is settled: the state must read up while the key is held.
  {
    const short blocked_state = GetAsyncKeyState(VK_F24);
    assert((blocked_state & 0x8000) == 0);
  }
  // Self-injected input passes the block in both directions.
  assert(service.send({{VK_F24, true}}).ok());
  assert(wait_for([&] { return (GetAsyncKeyState(VK_F24) & 0x8000) != 0; }));
  assert(service.send({{VK_F24, false}}).ok());
  assert(wait_for([&] { return (GetAsyncKeyState(VK_F24) & 0x8000) == 0; }));
  // The foreign release is swallowed too, so the snapshot clears.
  send_key_state(VK_F24, false);
  assert(wait_for([&] { return !service.physical_key_down(VK_F24); }));
  // Releasing the block restores delivery.
  service.set_blocked(false);
  assert(!service.blocked());
  send_key_state(VK_F24, true);
  assert(wait_for([&] { return (GetAsyncKeyState(VK_F24) & 0x8000) != 0; }));
  send_key_state(VK_F24, false);
  assert(wait_for([&] { return (GetAsyncKeyState(VK_F24) & 0x8000) == 0; }));

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

  // Stop is repeatable; new subscriptions are refused afterwards. A stop
  // also clears the block flag unconditionally, so shutdown can never
  // leave the desktop without keyboard/mouse input.
  service.set_blocked(true);
  assert(service.blocked());
  assert(service.stop().ok());
  assert(service.stop().ok());
  assert(!service.blocked());
  assert(service.state() == rime::win32::InputServiceState::Stopped);
  assert(service.subscribe([](const InputEvent&) {}) == 0);
  assert(!service.send({{VK_F24, true}}).ok());

  // A second service takes over after the first stopped. Fresh instance:
  // no stale block, history back at the default capacity, and the seeded
  // snapshot agrees with the live OS state.
  InputService second;
  assert(second.start().ok());
  assert(!second.blocked());
  assert(second.key_history_capacity() == 40);
  assert(second.key_history().empty());
  assert(second.physical_key_down(VK_F24) ==
         ((GetAsyncKeyState(VK_F24) & 0x8000) != 0));

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
