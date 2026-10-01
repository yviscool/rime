#pragma once

#include "rime/core/types.hpp"

#include <cstdint>
#include <functional>
#include <memory>

namespace rime::win32 {

enum class InputEventKind : std::uint8_t { Key, Mouse };

enum class MouseAction : std::uint8_t { Move, Down, Up, Wheel };

// Immutable snapshot of one low-level input event. Produced on the hook
// thread, consumed wherever the subscriber delivers it.
struct InputEvent {
  InputEventKind kind{InputEventKind::Key};
  std::uint64_t sequence{0};
  // GetMessageTime-style milliseconds (32-bit system clock).
  std::uint64_t timestamp_ms{0};
  bool injected{false};

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

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rime::win32
