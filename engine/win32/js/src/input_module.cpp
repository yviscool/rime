#include "rime/win32/js_input.hpp"

#include "rime/core/json.hpp"
#include "rime/js/host.hpp"
#include "rime/js/runtime.hpp"

#include "quickjs.h"

#include <atomic>
#include <memory>
#include <string>

namespace rime::win32 {
namespace {

namespace json = rime::core::json;

rime::js::Host* host_of(JSContext* context) {
  return static_cast<rime::js::Host*>(JS_GetContextOpaque(context));
}

InputModuleBinding* binding_of(JSContext* context) {
  auto* host = host_of(context);
  if (!host) return nullptr;
  return static_cast<InputModuleBinding*>(host->module_data("rime:input"));
}

std::string event_json(const InputEvent& event) {
  json::Value value = json::Value::object();
  value.set("sequence", json::Value::number(static_cast<double>(event.sequence)));
  value.set("timestamp", json::Value::number(static_cast<double>(event.timestamp_ms)));
  value.set("injected", json::Value::boolean(event.injected));
  if (event.kind == InputEventKind::Key) {
    value.set("kind", json::Value::string("key"));
    value.set("down", json::Value::boolean(event.key_down));
    value.set("vk", json::Value::number(static_cast<double>(event.vk)));
    value.set("scan", json::Value::number(static_cast<double>(event.scan)));
    value.set("alt", json::Value::boolean(event.alt));
    value.set("control", json::Value::boolean(event.control));
    value.set("shift", json::Value::boolean(event.shift));
    value.set("super", json::Value::boolean(event.super));
    return json::stringify(value);
  }
  value.set("kind", json::Value::string("mouse"));
  const char* action = "move";
  switch (event.mouse_action) {
    case MouseAction::Down:
      action = "down";
      break;
    case MouseAction::Up:
      action = "up";
      break;
    case MouseAction::Wheel:
      action = "wheel";
      break;
    case MouseAction::Move:
      break;
  }
  value.set("action", json::Value::string(action));
  value.set("x", json::Value::number(static_cast<double>(event.x)));
  value.set("y", json::Value::number(static_cast<double>(event.y)));
  value.set("button", json::Value::number(static_cast<double>(event.button)));
  value.set("wheelDelta", json::Value::number(static_cast<double>(event.wheel_delta)));
  return json::stringify(value);
}

JSValue input_subscribe(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                        void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  auto* host = host_of(context);
  if (!host || !binding || !binding->service) {
    return JS_ThrowInternalError(context, "rime:input is not wired");
  }
  if (argc < 1 || !JS_IsFunction(context, argv[0])) {
    return JS_ThrowTypeError(context, "subscribe(handler)");
  }

  const auto queue = host->event_queue();
  auto callback_id = std::make_shared<std::atomic<std::uint64_t>>(0);
  const std::uint64_t subscription_id = binding->service->subscribe(
      [queue, callback_id](const InputEvent& event) {
        const std::uint64_t id = callback_id->load(std::memory_order_acquire);
        if (id != 0) (void)queue->push(id, event_json(event));
      });
  if (subscription_id == 0) {
    return JS_ThrowInternalError(context, "input service is not running");
  }

  std::uint64_t host_callback_id = 0;
  if (const auto error = host->add_callback(JS_DupValue(context, argv[0]), host_callback_id);
      !error.ok()) {
    (void)binding->service->unsubscribe(subscription_id);
    return JS_ThrowInternalError(context, "%s", error.message.c_str());
  }
  callback_id->store(host_callback_id, std::memory_order_release);
  binding->callbacks[subscription_id] = host_callback_id;
  return JS_NewInt64(context, static_cast<std::int64_t>(subscription_id));
}

JSValue input_unsubscribe(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                          void* opaque) {
  auto* binding = static_cast<InputModuleBinding*>(opaque);
  auto* host = host_of(context);
  if (!host || !binding || !binding->service) {
    return JS_ThrowInternalError(context, "rime:input is not wired");
  }
  if (argc < 1) return JS_ThrowTypeError(context, "unsubscribe(subscriptionId)");
  std::int64_t raw_id = 0;
  if (JS_ToInt64(context, &raw_id, argv[0])) return JS_EXCEPTION;
  if (raw_id <= 0) return JS_NewBool(context, 0);
  const std::uint64_t subscription_id = static_cast<std::uint64_t>(raw_id);

  bool removed = binding->service->unsubscribe(subscription_id);
  const auto found = binding->callbacks.find(subscription_id);
  if (found != binding->callbacks.end()) {
    (void)host->remove_callback(found->second);
    binding->callbacks.erase(found);
    removed = true;
  }
  return JS_NewBool(context, removed ? 1 : 0);
}

int input_module_init(JSContext* context, JSModuleDef* module) {
  auto* binding = binding_of(context);
  if (!binding || !binding->service) {
    JS_ThrowInternalError(context, "rime:input requires an input module binding");
    return -1;
  }
  JSValue input = JS_NewObject(context);
  auto add = [&](const char* name, JSCClosure* function, int length) -> bool {
    JSValue value = JS_NewCClosure(context, function, name, nullptr, length, 0, binding);
    if (JS_IsException(value)) {
      JS_FreeValue(context, input);
      return false;
    }
    JS_SetPropertyStr(context, input, name, value);
    return true;
  };
  if (!add("subscribe", input_subscribe, 1) || !add("unsubscribe", input_unsubscribe, 1)) {
    return -1;
  }
  return JS_SetModuleExport(context, module, "input", input);
}

JSModuleDef* create_input_module(JSContext* context) {
  JSModuleDef* module = JS_NewCModule(context, "rime:input", input_module_init);
  if (!module) return nullptr;
  if (JS_AddModuleExport(context, module, "input") < 0) return nullptr;
  return module;
}

rime::core::Error check_binding(const InputModuleBinding* binding) {
  if (!binding || !binding->service) {
    return {rime::core::Error::Code::InvalidContract,
            "rime:input requires an input service"};
  }
  return rime::core::Error::none();
}

}  // namespace

rime::core::Error register_input_module(rime::js::Host& host, InputModuleBinding* binding) {
  if (const auto error = check_binding(binding); !error.ok()) return error;
  host.set_module_data("rime:input", binding);
  host.modules().add_native("rime:input",
                            [](JSContext* context) { return create_input_module(context); });
  return rime::core::Error::none();
}

rime::core::Error register_input_module(rime::js::Runtime& runtime,
                                        InputModuleBinding* binding) {
  if (const auto error = check_binding(binding); !error.ok()) return error;
  return runtime.add_native_module(
      "rime:input", [](JSContext* context) { return create_input_module(context); }, binding);
}

}  // namespace rime::win32
