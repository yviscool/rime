#include "rime/win32/input_probe.hpp"

#include <atomic>
#include <chrono>
#include <mutex>

namespace rime::win32::input_probe {

namespace {

std::mutex& mutex() {
  static std::mutex instance;
  return instance;
}

// Lock-free fast path: every stage stamp reads this first, so the disarmed
// hot path (every keystroke system-wide) pays one relaxed load, no mutex.
std::atomic<bool>& armed_flag() {
  static std::atomic<bool> flag{false};
  return flag;
}

struct State {
  bool armed{false};
  bool hook_done{false};
  bool enqueue_done{false};
  bool deliver_done{false};
  std::uint32_t vk{0};
  Stages stages{};
};

State& state() {
  static State instance;
  return instance;
}

std::uint64_t now_ns() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

}  // namespace

bool arm(std::uint32_t vk) {
  std::lock_guard lock(mutex());
  if (state().armed) return false;
  state() = State{};
  state().armed = true;
  state().vk = vk;
  armed_flag().store(true, std::memory_order_release);
  return true;
}

bool read(Stages& out) {
  std::lock_guard lock(mutex());
  if (!state().armed) return false;
  out = state().stages;
  state().armed = false;
  armed_flag().store(false, std::memory_order_release);
  return true;
}

void clear() {
  std::lock_guard lock(mutex());
  state().armed = false;
  armed_flag().store(false, std::memory_order_release);
}

namespace internal {

void on_send_entry() {
  if (!armed_flag().load(std::memory_order_acquire)) return;
  std::lock_guard lock(mutex());
  if (state().armed) state().stages.send_entry = now_ns();
}

void on_pre_inject() {
  if (!armed_flag().load(std::memory_order_acquire)) return;
  std::lock_guard lock(mutex());
  if (state().armed) state().stages.pre_inject = now_ns();
}

void on_post_inject() {
  if (!armed_flag().load(std::memory_order_acquire)) return;
  std::lock_guard lock(mutex());
  if (state().armed) state().stages.post_inject = now_ns();
}

// First self-injected key-down of the armed vk wins (the batch down beats
// its paired up everywhere: same order on the wire, the chain and the pump).
void on_hook_event(std::uint32_t vk, bool down, bool self) {
  if (!armed_flag().load(std::memory_order_acquire)) return;
  std::lock_guard lock(mutex());
  if (!state().armed || state().hook_done || !down || !self || vk != state().vk) return;
  state().stages.hook_entry = now_ns();
  state().hook_done = true;
}

void on_enqueue_done() {
  if (!armed_flag().load(std::memory_order_acquire)) return;
  std::lock_guard lock(mutex());
  // Gated on hook_done (the armed down was already seen): a foreign event
  // slipping between arm and our down must not steal the enqueue stamp.
  if (!state().armed || state().enqueue_done || !state().hook_done) return;
  state().stages.enqueue_done = now_ns();
  state().enqueue_done = true;
}

void on_deliver(std::uint32_t vk, bool down, bool self) {
  if (!armed_flag().load(std::memory_order_acquire)) return;
  std::lock_guard lock(mutex());
  if (!state().armed || state().deliver_done || !down || !self || vk != state().vk) return;
  state().stages.deliver_ns = now_ns();
  state().deliver_done = true;
}

}  // namespace internal

}  // namespace rime::win32::input_probe
