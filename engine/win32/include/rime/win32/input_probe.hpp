#pragma once

#include <cstdint>

namespace rime::win32 {

// Test-only latency probe for the inject -> hook-callback path (same rules
// as input_seam: observation only, never alters the unit under test). All
// timestamps are std::chrono::steady_clock nanoseconds, the same clock the
// bench uses, so probe deltas subtract directly against bench samples.
//
// Arming selects the FIRST self-injected key-down of `vk` that reaches each
// stage; later events (including the paired key-up of the same batch) do not
// overwrite. Stages are causally ordered by construction:
//   send_entry -> pre_inject -> post_inject -> hook_entry ->
//   enqueue_done -> deliver_ns (-> bench callback timestamp)
// which is exactly why the bench must NOT subtract per-iteration totals
// across threads: on multicore the down callback routinely lands BEFORE the
// SendInput call returns, so overlapping intervals subtract to nonsense.
// These six stamps never overlap by construction.
namespace input_probe {

struct Stages {
  std::uint64_t send_entry{0};
  std::uint64_t pre_inject{0};
  std::uint64_t post_inject{0};
  std::uint64_t hook_entry{0};
  std::uint64_t enqueue_done{0};
  std::uint64_t deliver_ns{0};
};

// Arms the probe for one pass of `vk` (e.g. VK_F24). Returns false when a
// pass is already armed (read it first).
bool arm(std::uint32_t vk);
// Copies the armed pass and disarms. Returns false when nothing was armed;
// fields for stages never reached stay 0 - the reader must tolerate that.
bool read(Stages& out);
// Disarms without reading. Idempotent.
void clear();

namespace internal {

// Stage stamps for input.cpp. Each is a no-op (one relaxed atomic read)
// while no pass is armed, so the disarmed hot path pays no mutex.
void on_send_entry();
void on_pre_inject();
void on_post_inject();
void on_hook_event(std::uint32_t vk, bool down, bool self);
void on_enqueue_done();
void on_deliver(std::uint32_t vk, bool down, bool self);

}  // namespace internal

}  // namespace input_probe

}  // namespace rime::win32
