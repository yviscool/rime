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
  // Runs on the UI thread for every message that reaches the message window
  // (WM_CLIPBOARDUPDATE, WM_USER+n, OnMessage test traffic...). Returning is
  // void by design: observers are async event sources (delivery hops to the
  // JS thread through the host event queue), never synchronous reply hooks,
  // so no observer can block or replace DefWindowProc processing.
  using MessageObserver = std::function<void(unsigned int message, std::uintptr_t wparam,
                                             std::uintptr_t lparam, std::uintptr_t hwnd)>;

  UiThread();
  ~UiThread();
  UiThread(const UiThread&) = delete;
  UiThread& operator=(const UiThread&) = delete;

  [[nodiscard]] rime::core::Error start();
  // Runs `task` on the UI thread. The deadline bounds the queued phase; a
  // task that already started is awaited to completion (Win32 calls are
  // bounded). Tasks still queued are skipped when `cancellation` fires.
  rime::core::Error call(const std::function<void()>& task,
                         std::chrono::milliseconds timeout = std::chrono::seconds(5),
                         rime::core::CancellationToken cancellation = {});
  // Repeatable: rejects new work, quits the pump, joins and releases the
  // UI lane.
  [[nodiscard]] rime::core::Error stop();
   [[nodiscard]] UiThreadState state() const;
   [[nodiscard]] bool on_ui_thread() const;

   // Thread-safe: registers a message observer (id 0 when the pump is not
   // running). The observer stays registered until remove_message_observer;
   // a copy taken per message keeps removal safe during delivery.
   std::uint64_t add_message_observer(MessageObserver observer);
   bool remove_message_observer(std::uint64_t id);
   [[nodiscard]] std::size_t message_observer_count() const;
   // Raw message-window handle for native callers (tests post their probe
   // messages here). Exposed as uintptr_t so this header stays free of
   // windows.h; 0 before start/after stop. Never handed to JS.
   [[nodiscard]] std::uintptr_t message_window() const;

  // Internal: invoked by the message-window procedure (a free function that
  // cannot name the private Impl). Takes void* to avoid pulling windows.h
  // into this header.
  static void dispatch_task_message(void* userdata);
  // Internal: relays one non-task message to the registered observers
  // (OnMessage, clipboard-update relay) before DefWindowProc; uintptr_t
  // parameters keep windows.h out of this header.
  static void dispatch_message_observers(void* userdata, std::uintptr_t message,
                                         std::uintptr_t wparam, std::uintptr_t lparam,
                                         std::uintptr_t hwnd);

  private:
   // Opaque pump state; defined in ui_thread.cpp.
   struct Impl;

  private:
   std::unique_ptr<Impl> impl_;
};

}  // namespace rime::win32
