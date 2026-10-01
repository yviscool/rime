#include "rime/js/abi.hpp"

#include "quickjs.h"

#include <sstream>
#include <utility>

namespace rime::js {

const char* host_abi_state_name(const HostAbiState state) {
  switch (state) {
    case HostAbiState::Created:
      return "created";
    case HostAbiState::Loaded:
      return "loaded";
    case HostAbiState::Executed:
      return "executed";
    case HostAbiState::Failed:
      return "failed";
    case HostAbiState::Unloaded:
      return "unloaded";
  }
  return "unknown";
}

HostAbi::HostAbi() = default;

HostAbi::~HostAbi() { (void)unload(); }

rime::core::Error HostAbi::load(const std::string_view source, const std::string_view filename) {
  if (state_ != HostAbiState::Created) {
    return {rime::core::Error::Code::InvalidState,
            std::string("load requires state created but host is ") +
                host_abi_state_name(state_)};
  }
  if (source.empty()) {
    return {rime::core::Error::Code::InvalidContract, "script source is empty"};
  }
  source_ = source;
  filename_ = filename.empty() ? "<script>" : std::string(filename);
  state_ = HostAbiState::Loaded;
  return rime::core::Error::none();
}

rime::core::Error HostAbi::execute(const std::string_view source,
                                   const std::string_view filename) {
  if (state_ != HostAbiState::Loaded && state_ != HostAbiState::Executed) {
    return {rime::core::Error::Code::InvalidState,
            std::string("execute requires a loaded script but host is ") +
                host_abi_state_name(state_)};
  }
  if (!source.empty()) source_ = source;
  if (!filename.empty()) filename_ = std::string(filename);

  rime::core::Error result = rime::core::Error::none();
  if (JS_DetectModule(source_.data(), source_.size())) {
    result = host_.eval_module(source_, filename_);
  } else {
    result = host_.eval(source_, filename_);
  }
  host_.drain();

  if (!result.ok()) {
    state_ = HostAbiState::Failed;
    if (on_error_) on_error_(filename_, result.message);
    return result;
  }
  state_ = HostAbiState::Executed;
  return rime::core::Error::none();
}

std::string HostAbi::inspect(const std::string_view request_json) {
  std::string request(request_json);
  return host_.inspect(request);
}

std::size_t HostAbi::drain() { return host_.drain(); }

rime::core::Error HostAbi::unload() {
  if (state_ == HostAbiState::Unloaded) return rime::core::Error::none();
  if (state_ == HostAbiState::Created) {
    state_ = HostAbiState::Unloaded;
    return rime::core::Error::none();
  }

  std::ostringstream reasons;
  bool busy = false;
  const auto subscriptions = host_.subscriptions().list();
  if (!subscriptions.empty()) {
    busy = true;
    reasons << subscriptions.size() << " subscription(s): ";
    for (std::size_t index = 0; index < subscriptions.size(); ++index) {
      if (index > 0) reasons << ", ";
      reasons << subscriptions[index];
    }
    reasons << "; ";
  }
  const std::size_t callbacks = host_.callback_count();
  if (callbacks > 0) {
    busy = true;
    reasons << callbacks << " JS callback(s) still held; ";
  }
  const std::size_t promises = host_.pending_async();
  if (promises > 0) {
    busy = true;
    reasons << promises << " unresolved promise(s); ";
  }
  const std::size_t timers = host_.timers().pending();
  if (timers > 0) {
    busy = true;
    reasons << timers << " armed timer(s); ";
  }
  if (const auto queue = host_.event_queue(); queue && !queue->empty()) {
    busy = true;
    reasons << "pending host event(s); ";
  }

  if (busy) {
    return {rime::core::Error::Code::InvalidState,
            "unload refused while host is still active: " + reasons.str()};
  }

  host_.subscriptions().close();
  host_.timers().stop();
  state_ = HostAbiState::Unloaded;
  return rime::core::Error::none();
}

void HostAbi::exit(const int code) {
  // NOTE: exit() only records the code and fires on_exit; it is not an
  // unload — subscriptions/callbacks/timers stay alive until unload().
  exit_code_ = code;
  if (on_exit_) on_exit_(code);
}

void HostAbi::set_error_handler(ErrorHandler handler) { on_error_ = std::move(handler); }
void HostAbi::set_exit_handler(ExitHandler handler) { on_exit_ = std::move(handler); }

}  // namespace rime::js
