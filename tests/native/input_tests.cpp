// Realism: L5 - real SendInput keystrokes/mouse and a real global hook in
// an interactive desktop session; the test owns its target window and
// asserts the observed input, not just the absence of errors. SendLevel
// coverage adds the dwExtraInfo level codec (0..100 round-trip on both the
// 64-bit keyboard view and the truncated 32-bit mouse view) plus live
// level-2 key and level-7 mouse batches decoded back off the hooks with
// their self-injected flag intact.

// Needs an interactive desktop, exclusive run: injects real keys/mouse and hooks global input.
#include "rime/win32/input.hpp"

#include "rime/win32/input_seam.hpp"

#include <windows.h>

#include <cassert>
#include <chrono>
#include <cstdio>
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

// OS seam substitutes (input_seam.hpp): swapped in for the real API, so the
// failure surfaces from the real start()/send()/send_mouse() paths. WINAPI
// calling convention must match the seam typedefs exactly.
HHOOK WINAPI failing_hook_installer(int, HOOKPROC, HINSTANCE, DWORD) { return nullptr; }

UINT WINAPI partial_send_input(const UINT count, LPINPUT, int) {
  return count > 0 ? count - 1 : 0;
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
  assert(service.send_level() == 0);
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
  // interactive (the physical cursor can move under us), so every attempt
  // re-reads the origin right before injecting and then polls the delta
  // against a deadline: a jittery desktop earns one more probe, a genuine
  // injection fault never satisfies the predicate. Attempts are capped at the
  // AGENTS isolated-rerun budget (<=3, recorded environment jitter only) and
  // exhaustion fails explicitly - no attempt degrades into a pass. Each
  // retry is announced so a rerun can be attributed in the flaky ledger.
  bool relative_ok = false;
  for (int attempt = 0; attempt < 3 && !relative_ok; ++attempt) {
    POINT before_relative{};
    const BOOL got_relative = GetCursorPos(&before_relative);
    if (got_relative == FALSE) return 1;
    assert(service.send_mouse({{rime::win32::SendMouseAction::RelMove, 5, -3, 1}}).ok());
    relative_ok = wait_for(
        [&] {
          POINT now{};
          if (GetCursorPos(&now) == FALSE) return false;
          return now.x >= before_relative.x + 4 && now.x <= before_relative.x + 6 &&
                 now.y >= before_relative.y - 4 && now.y <= before_relative.y - 2;
        },
        2s);
    if (!relative_ok && attempt < 2) {
      std::fprintf(stderr, "relative-move probe %d saw an externally moved cursor; retrying\n",
                   attempt + 1);
    }
  }
  assert(relative_ok && "relative mouse move never landed in the 3-probe budget");
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

  // --- SendLevel (AHK SendLevel, defines.h:880-887) ----------------------
  // The level travels in dwExtraInfo as `marker - level` - the same shape AHK
  // uses for KEY_IGNORE_LEVEL (keyboard_mouse.h:267) - so the codec is pinned
  // first: every level 0..100 round-trips through the 64-bit keyboard view
  // and the truncated 32-bit mouse view, level 0 stays bit-identical to the
  // untagged marker, and anything outside the window is never mistaken for
  // self input and decodes to 0 (documented deviation: AHK reports 101 for
  // input it did not inject, hotkey.h:76-81; foreign input here reads 0).
  assert(rime::win32::k_send_level_max == 100);
  assert(rime::win32::self_injected_marker_for(0) == rime::win32::k_self_injected_marker);
  assert(rime::win32::self_injected_marker_for(101) ==
         rime::win32::self_injected_marker_for(100));
  assert(rime::win32::self_injected_marker_for(~0u) == rime::win32::self_injected_marker_for(100));
  for (std::uint32_t level = 0; level <= rime::win32::k_send_level_max; ++level) {
    const std::uint64_t tagged = rime::win32::self_injected_marker_for(level);
    const auto tagged_low = static_cast<std::uint32_t>(tagged);
    assert(rime::win32::is_self_injected_marker(tagged));        // keyboard hook view
    assert(rime::win32::is_self_injected_marker32(tagged_low));  // mouse hook view
    assert(rime::win32::decode_send_level(tagged_low) == level);
    // Subtracting the level never borrows out of the low word, so the high
    // "Rime" half still matches the plain marker at every valid level.
    assert((tagged >> 32) == (rime::win32::k_self_injected_marker >> 32));
  }
  {
    const auto base_low = static_cast<std::uint32_t>(rime::win32::k_self_injected_marker);
    assert(!rime::win32::is_self_injected_marker32(0));             // raw SendInput taps
    assert(!rime::win32::is_self_injected_marker32(0xDEADBEEFu));   // probe value
    assert(!rime::win32::is_self_injected_marker32(base_low + 1));  // above the base
    assert(!rime::win32::is_self_injected_marker32(base_low - (rime::win32::k_send_level_max + 1)));
    assert(rime::win32::decode_send_level(0) == 0);
    assert(rime::win32::decode_send_level(0xDEADBEEFu) == 0);
    assert(rime::win32::decode_send_level(base_low + 1) == 0);
    // Wrong "Rime" half: rejected even when the low half sits in the window.
    assert(!rime::win32::is_self_injected_marker(rime::win32::self_injected_marker_for(2) +
                                                  (1ull << 32)));
  }

  // Everything observed so far - the raw foreign taps above and this
  // process's own default-level batches alike - decodes to level 0, and at
  // least one self batch exists to make that a real check.
  {
    std::lock_guard lock(mutex);
    bool saw_self = false;
    for (const auto& event : events) {
      assert(event.send_level == 0);
      if (event.self_injected) saw_self = true;
    }
    assert(saw_self);
  }

  // Service state: default 0, round-trips, and the native setter clamps to
  // 100 exactly like set_key_history_capacity clamps to 500 (the JS layer
  // rejects out-of-range input outright, this is the defensive side).
  assert(service.send_level() == 0);
  service.set_send_level(3);
  assert(service.send_level() == 3);
  service.set_send_level(101);
  assert(service.send_level() == 100);
  service.set_send_level(~0u);
  assert(service.send_level() == 100);
  service.set_send_level(0);
  assert(service.send_level() == 0);

  std::size_t events_before_level = 0;
  {
    std::lock_guard lock(mutex);
    events_before_level = events.size();
  }

  // Live keyboard round-trip: a level-2 batch reaches the hook with its
  // level decoded back off dwExtraInfo while still counting as self input
  // (the BlockInput bypass and the chord skip both depend on that flag).
  service.set_send_level(2);
  {
    const auto tagged_batch = service.send({{VK_F24, true}, {VK_F24, false}});
    assert(tagged_batch.ok());
  }
  assert(wait_for([&] {
    std::lock_guard lock(mutex);
    for (std::size_t index = events_before_level; index < events.size(); ++index) {
      const auto& event = events[index];
      if (event.kind == InputEventKind::Key && event.vk == VK_F24 && event.self_injected &&
          event.send_level == 2) {
        return true;
      }
    }
    return false;
  }));

  // Live mouse round-trip: WH_MOUSE_LL only reports the low 32 bits, so the
  // same window has to hold on that path too. Absolute move to the cleared
  // point the batch test already used, cursor restored right after - the
  // desktop must end exactly where it started.
  POINT level_origin{};
  // NOTE: hoisted out of assert(): GetCursorPos writes level_origin even
  // when NDEBUG compiles the assertion out.
  const BOOL got_level_origin = GetCursorPos(&level_origin);
  if (got_level_origin == FALSE) return 1;
  service.set_send_level(7);
  {
    const auto mouse_batch =
        service.send_mouse({{rime::win32::SendMouseAction::Move, 456, 650, 1}});
    assert(mouse_batch.ok());
  }
  assert(wait_for([&] {
    std::lock_guard lock(mutex);
    for (std::size_t index = events_before_level; index < events.size(); ++index) {
      const auto& event = events[index];
      if (event.kind == InputEventKind::Mouse && event.mouse_action == MouseAction::Move &&
          event.self_injected && event.send_level == 7 && event.x >= 455 && event.x <= 457 &&
          event.y >= 649 && event.y <= 651) {
        return true;
      }
    }
    return false;
  }));
  // NOTE: hoisted out of assert(): SetCursorPos has a side effect (moves the
  // cursor) that must run even when NDEBUG compiles assert() out.
  const BOOL restored_level = SetCursorPos(level_origin.x, level_origin.y);
  if (restored_level == FALSE) return 1;

  // Back to the default: a level-0 batch after two non-zero ones still reads
  // level 0 and still self - so the baseline above is not an artefact of the
  // level never having been raised.
  service.set_send_level(0);
  assert(service.send_level() == 0);
  {
    const auto untagged_batch = service.send({{VK_F24, true}, {VK_F24, false}});
    assert(untagged_batch.ok());
  }
  assert(wait_for([&] {
    std::lock_guard lock(mutex);
    for (std::size_t index = events_before_level; index < events.size(); ++index) {
      const auto& event = events[index];
      if (event.kind == InputEventKind::Key && event.vk == VK_F24 && event.self_injected &&
          event.send_level == 0) {
        return true;
      }
    }
    return false;
  }));

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
  // Control key: "no new row" must not pass because the pipeline died. A
  // live subscription is fed by the same hook -> pending -> deliver path that
  // fed the closed one, so it has to observe this tap first (positive,
  // deadline-bounded) and the closed subscription is then watched for growth
  // across a bounded window instead of one sample after a fixed 300ms sleep
  // (anti-cheat #5). unsubscribe() above returned true only after erasing the
  // entry, so the closed callback cannot run again - the window is what
  // catches a regression of that contract.
  std::vector<InputEvent> control_events;
  const auto control = service.subscribe([&](const InputEvent& event) {
    std::lock_guard lock(mutex);
    control_events.push_back(event);
  });
  assert(control != 0);
  send_vk(VK_F23);
  assert(wait_for([&] {
    bool down = false;
    bool up = false;
    std::lock_guard lock(mutex);
    for (const auto& event : control_events) {
      if (event.kind == InputEventKind::Key && event.vk == VK_F23) {
        if (event.key_down) {
          down = true;
        } else {
          up = true;
        }
      }
    }
    return down && up;
  }));
  assert(!wait_for(
      [&] {
        std::lock_guard lock(mutex);
        return events.size() != recorded;
      },
      200ms));
  assert(service.unsubscribe(control));
  assert(service.subscription_count() == 0);

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
  assert(second.send_level() == 0);
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

  // --- Fault: hook install failure (OS seam) ----------------------------
  // SetWindowsHookExW fails only on a broken desktop, so the seam substitutes
  // that one call; the whole real start() path runs and must refuse with
  // ExecutionFailed instead of reporting Running without hooks.
  rime::win32::input_seam::set_hook_installer(&failing_hook_installer);
  InputService faulted;
  const auto install_failed = faulted.start();
  rime::win32::input_seam::set_hook_installer(nullptr);
  assert(!install_failed.ok());
  assert(install_failed.code == rime::core::Error::Code::ExecutionFailed);
  assert(install_failed.message.find("cannot install low-level keyboard/mouse hooks") !=
         std::string::npos);
  assert(faulted.state() == rime::win32::InputServiceState::Stopped);

  // --- Fault: partial SendInput batch (OS seam) -------------------------
  // The seam consumes fewer inputs than offered; send() and send_mouse()
  // must report the injected/total counts instead of pretending the batch
  // landed. The seam stays armed across both calls and is restored before
  // the service stops (stop() clears block/force state; the seam is test
  // state and only this test owns it).
  rime::win32::input_seam::set_send_input(&partial_send_input);
  InputService partial;
  const auto partial_started = partial.start();
  assert(partial_started.ok());
  const auto partial_keys = partial.send({{VK_F24, true}, {VK_F24, false}});
  const auto partial_mouse =
      partial.send_mouse({{rime::win32::SendMouseAction::Move, 0, 0, 1}});
  rime::win32::input_seam::set_send_input(nullptr);
  assert(!partial_keys.ok());
  assert(partial_keys.code == rime::core::Error::Code::ExecutionFailed);
  assert(partial_keys.message.find("SendInput injected 1 of 2 key steps") != std::string::npos);
  assert(!partial_mouse.ok());
  assert(partial_mouse.code == rime::core::Error::Code::ExecutionFailed);
  assert(partial_mouse.message.find("SendInput injected 0 of 1 mouse steps") !=
         std::string::npos);
  assert(partial.stop().ok());

  return 0;
}
