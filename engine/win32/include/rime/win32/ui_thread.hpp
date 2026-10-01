#pragma once

#include "rime/core/cancellation.hpp"
#include "rime/core/types.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

namespace rime::win32 {

enum class UiThreadState : std::uint8_t { Created, Starting, Running, Stopping, Stopped, Failed };

const char* ui_thread_state_name(UiThreadState state);

// Owns the Win32 message pump thread. The UI lane is claimed for the pump
// thread; HWND, hook and window-procedure work may only run through call().
class UiThread final {
 public:
  UiThread();
  ~UiThread();
  UiThread(const UiThread&) = delete;
  UiThread& operator=(const UiThread&) = delete;

  rime::core::Error start();
  // Runs `task` on the UI thread. The deadline bounds the queued phase; a
  // task that already started is awaited to completion (Win32 calls are
  // bounded). Tasks still queued are skipped when `cancellation` fires.
  rime::core::Error call(const std::function<void()>& task,
                         std::chrono::milliseconds timeout = std::chrono::seconds(5),
                         rime::core::CancellationToken cancellation = {});
  // Repeatable: rejects new work, quits the pump, joins and releases the
  // UI lane.
  rime::core::Error stop();
   [[nodiscard]] UiThreadState state() const;
   [[nodiscard]] bool on_ui_thread() const;

   // Internal: invoked by the message-window procedure (a free function that
   // cannot name the private Impl). Takes void* to avoid pulling windows.h
   // into this header.
   static void dispatch_task_message(void* userdata);

  private:
   // Opaque pump state; defined in ui_thread.cpp.
   struct Impl;

  private:
   std::unique_ptr<Impl> impl_;
};

}  // namespace rime::win32
