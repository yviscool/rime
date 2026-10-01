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
// release. Ordered batches are injected in a single SendInput call.
struct SendKeyEvent {
  std::uint32_t vk{0};
  bool down{false};
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
  // step list. Batches serialize against each other; the hook thread only
  // queues, so injection never blocks low-level input delivery.
  rime::core::Error send(const std::vector<SendKeyEvent>& keys);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rime::win32
