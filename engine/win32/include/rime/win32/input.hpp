#pragma once

#include "rime/core/types.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace rime::win32 {

enum class InputEventKind : std::uint8_t { Key, Mouse };

enum class MouseAction : std::uint8_t { Move, Down, Up, Wheel };

// dwExtraInfo marker written for input this process injects via send(). The
// hook compares it verbatim: a match means "self", while other injectors'
// LLKHF_INJECTED events stay foreign. 64-bit ASCII "RimeInpt" keeps accidental
// collisions with other tools negligible.
inline constexpr std::uint64_t k_self_injected_marker = 0x52696D65'496E7074ull;

// Immutable snapshot of one low-level input event. Produced on the hook
// thread, consumed wherever the subscriber delivers it.
struct InputEvent {
  InputEventKind kind{InputEventKind::Key};
  std::uint64_t sequence{0};
  // GetMessageTime-style milliseconds (32-bit system clock).
  std::uint64_t timestamp_ms{0};
  bool injected{false};
  // True only when `injected` and dwExtraInfo carries k_self_injected_marker:
  // input this process sent through send(), not other injectors' input.
  bool self_injected{false};

  // Key events.
  bool key_down{false};
  std::uint32_t vk{0};
  std::uint32_t scan{0};
  bool alt{false};
  bool control{false};
  bool shift{false};
  bool super{false};

  // Mouse events.
  MouseAction mouse_action{MouseAction::Move};
  std::int32_t x{0};
  std::int32_t y{0};
  // 1 left, 2 right, 3 middle (down/up only).
  std::uint32_t button{0};
  std::int32_t wheel_delta{0};
};

// One send() step: a key transition. vk is 1..254, down selects press or
// release. Ordered batches are injected in a single SendInput call. When
// unicode is set, vk carries a UTF-16 code unit (0..65535) injected with
// KEYEVENTF_UNICODE instead of a virtual key (AHK SendUnicodeChar).
struct SendKeyEvent {
  std::uint32_t vk{0};
  bool down{false};
  bool unicode{false};
};

// One send_mouse() step. Move is an absolute point in screen coordinates
// (primary monitor, AHK MOUSE_COORD_TO_ABS), RelMove is a raw delta applied
// in batch order, Down/Up press or release buttons 1 left / 2 right /
// 3 middle (logical; the SM_SWAPBUTTON translation happens at injection).
enum class SendMouseAction : std::uint8_t { Move, RelMove, Down, Up };

struct SendMouseStep {
  SendMouseAction action{SendMouseAction::Move};
  std::int32_t x{0};
  std::int32_t y{0};
  std::uint32_t button{1};
};

// Live per-side modifier + CapsLock snapshot behind input.modifiers().
struct ModifierState {
  bool lcontrol{false};
  bool rcontrol{false};
  bool lshift{false};
  bool rshift{false};
  bool lalt{false};
  bool ralt{false};
  bool lwin{false};
  bool rwin{false};
  bool caps_lock{false};
};

// Reads the per-side modifier state with GetAsyncKeyState (global physical
// state, one query per side so left/right stay distinguishable) and the
// CapsLock LED with GetKeyState's toggle bit. Free function: callable from
// any thread and directly unit-testable, no service instance required.
ModifierState read_modifier_state();

// One key's state behind getKeyState/keyWait (AHK GetKeyState's P/L/T
// modes). Toggle reads the GetKeyState toggle bit (the CapsLock/NumLock LED,
// probed to track reality even on non-pumping threads). Logical reads
// GetAsyncKeyState: the JS thread runs without a Win32 message pump, so
// GetKeyState's thread-queued down bit reads stale there (probed: a fresh
// non-pumping thread reports "up" while a key is held). Physical behaves
// like Logical in this free function; InputService::physical_key_down
// overrides it with the hook-maintained snapshot while the service runs.
enum class KeyStateType : std::uint8_t { Physical, Logical, Toggle };

bool read_key_state(std::uint32_t vk, KeyStateType type);

// One recorded key-history row (AHK KeyHistoryItem without the target-window
// column: the runtime has no per-event foreground tracking on the hook
// path). elapsed_ms is the delta to the previous recorded event.
struct KeyHistoryEntry {
  std::uint32_t vk{0};
  std::uint32_t scan{0};
  bool down{false};
  bool injected{false};
  bool self_injected{false};
  std::uint64_t timestamp_ms{0};
  std::uint64_t elapsed_ms{0};
};

enum class InputServiceState : std::uint8_t { Created, Running, Stopping, Stopped };

// Owns a dedicated hook thread that pumps WH_KEYBOARD_LL/WH_MOUSE_LL and
// delivers events to subscriptions. The hook callback itself only queues;
// delivery happens on the hook thread between messages. Subscriber
// callbacks MUST return immediately (e.g. push into a cross-thread queue):
// blocking stalls low-level input for the whole desktop.
class InputService final {
 public:
  using Callback = std::function<void(const InputEvent&)>;

  InputService();
  ~InputService();
  InputService(const InputService&) = delete;
  InputService& operator=(const InputService&) = delete;

  // Installs both hooks and waits for the install result. Start-once.
  rime::core::Error start();
  // Unhooks, joins the hook thread and closes every subscription. Idempotent.
  rime::core::Error stop();
  [[nodiscard]] InputServiceState state() const;

  // InstallKeybdHook/InstallMouseHook: runtime control of one low-level hook.
  // The request is applied on the hook thread and the call waits (bounded)
  // for the verdict, so the returned state is the effective installed state.
  // install=false without force keeps the hook while subscriptions still
  // need it; force removes it regardless (events then stop until a later
  // install - AHK's force-uninstall contract). Returns false when the
  // service is not running or the hook could not be (re)installed.
  bool set_keyboard_hook(bool install, bool force);
  bool set_mouse_hook(bool install, bool force);
  [[nodiscard]] bool keyboard_hook_installed() const;
  [[nodiscard]] bool mouse_hook_installed() const;

  // Registers a subscription while running. Returns 0 when not running.
  // Closing semantics: after unsubscribe returns, the callback is not
  // running (unless unsubscribe was called from inside that callback).
  std::uint64_t subscribe(Callback callback);
  bool unsubscribe(std::uint64_t id);
  [[nodiscard]] std::size_t subscription_count() const;
  // Events dropped because the pending queue was full (diagnostics).
  [[nodiscard]] std::uint64_t dropped_events() const;

  // Injects one ordered key batch through SendInput, tagged with
  // k_self_injected_marker so the hook reports it as self input. Refuses with
  // InvalidState while the service is not running (the hooks would never
  // observe the batch) and with InvalidContract for an empty or out-of-range
  // step list: non-unicode steps require vk 1..254, unicode steps a UTF-16
  // code unit 0..65535. Batches serialize against each other; the hook
  // thread only queues, so injection never blocks low-level input delivery.
  rime::core::Error send(const std::vector<SendKeyEvent>& keys);

  // Injects one ordered mouse batch through SendInput under the same marker,
  // serialization and running-state contract as send(). Absolute moves use
  // AHK's MOUSE_COORD_TO_ABS conversion against the primary screen; logical
  // buttons translate through SM_SWAPBUTTON like AHK v2. Refuses with
  // InvalidContract for an empty batch or buttons outside 1..3.
  rime::core::Error send_mouse(const std::vector<SendMouseStep>& steps);

  // Hook-maintained physical down snapshot (AHK g_PhysicalKeyState): seeded
  // from GetAsyncKeyState at hook install, then updated by every key and
  // mouse-button event the hook sees - blocked events included, because the
  // hook records before it swallows. Falls back to GetAsyncKeyState while
  // the service is not running. Any thread.
  [[nodiscard]] bool physical_key_down(std::uint32_t vk) const;

  // BlockInput switch: while blocked, the hooks swallow every non-self event
  // after recording it (history, subscriptions and the physical snapshot
  // still observe it; the OS and other processes never do). Self-injected
  // batches bypass the block so the script keeps working. stop() always
  // clears the flag, so a shutdown can never leave the desktop blocked.
  void set_blocked(bool blocked);
  [[nodiscard]] bool blocked() const;

  // KeyHistory ring (newest kept, capacity 0 disables recording). Resizing
  // trims immediately, mirroring AHK's KeyHistory argument (0..500; the JS
  // layer enforces the range, the service clamps defensively). Any thread.
  [[nodiscard]] std::vector<KeyHistoryEntry> key_history() const;
  void set_key_history_capacity(std::size_t capacity);
  [[nodiscard]] std::size_t key_history_capacity() const;

 private:
  struct Impl;
  // Shared body of set_keyboard_hook/set_mouse_hook.
  bool control_hook(bool keyboard, bool install, bool force);
  std::unique_ptr<Impl> impl_;
};

}  // namespace rime::win32
