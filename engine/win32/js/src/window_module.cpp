#include "rime/win32/js_window.hpp"

#include "rime/core/json.hpp"
#include "rime/js/host.hpp"
#include "rime/js/runtime.hpp"

#include "async_task.hpp"
#include "quickjs.h"

#include <string>
#include <utility>
#include <vector>

namespace rime::win32 {
namespace {

namespace json = rime::core::json;

WindowModuleBinding* binding_of(JSContext* context) {
  auto* host = static_cast<rime::js::Host*>(JS_GetContextOpaque(context));
  if (!host) return nullptr;
  return static_cast<WindowModuleBinding*>(host->module_data("rime:window"));
}

JSValue windows_list(JSContext* context, JSValueConst, int, JSValueConst*, int, void*) {
  WindowModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service) {
    return JS_ThrowInternalError(context, "rime:window is not wired");
  }
  WindowService* service = binding->service;
  return start_async(context, [service]() -> std::pair<bool, std::string> {
    std::vector<WindowInfo> windows;
    if (const auto error = service->list(windows); !error.ok()) return {false, error.message};
    json::Value array = json::Value::array();
    for (const auto& window : windows) array.push(window_info_json(window));
    return {true, json::stringify(array)};
  });
}

JSValue windows_active(JSContext* context, JSValueConst, int, JSValueConst*, int,
                       void*) {
  WindowModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service) {
    return JS_ThrowInternalError(context, "rime:window is not wired");
  }
  WindowService* service = binding->service;
  return start_async(context, [service]() -> std::pair<bool, std::string> {
    std::optional<WindowInfo> active;
    if (const auto error = service->active(active); !error.ok()) return {false, error.message};
    if (!active.has_value()) return {true, "null"};
    return {true, json::stringify(window_info_json(*active))};
  });
}

JSValue windows_info(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                     void*) {
  WindowModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service) {
    return JS_ThrowInternalError(context, "rime:window is not wired");
  }
  if (argc < 1) return JS_ThrowTypeError(context, "info(windowId)");
  int64_t raw_id = 0;
  if (JS_ToInt64(context, &raw_id, argv[0])) return JS_EXCEPTION;
  if (raw_id <= 0) return JS_ThrowTypeError(context, "info(windowId): id must be positive");
  const std::uint64_t window_id = static_cast<std::uint64_t>(raw_id);
  WindowService* service = binding->service;
  return start_async(context, [service, window_id]() -> std::pair<bool, std::string> {
    WindowInfo info;
    if (const auto error = service->info(window_id, info); !error.ok()) {
      return {false, error.message};
    }
    return {true, json::stringify(window_info_json(info))};
  });
}

JSValue windows_move(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                     void*) {
  WindowModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel || !binding->next_action_id) {
    return JS_ThrowInternalError(context, "rime:window is not wired");
  }
  if (argc < 2) return JS_ThrowTypeError(context, "move(target, position)");

  std::string target_text;
  if (JS_IsString(argv[0])) {
    const char* text = JS_ToCString(context, argv[0]);
    if (!text) return JS_EXCEPTION;
    target_text = text;
    JS_FreeCString(context, text);
    if (target_text != "active" && target_text.find_first_not_of("0123456789") != std::string::npos) {
      return JS_ThrowTypeError(context, "move(target, position): target must be an id or 'active'");
    }
    if (target_text != "active" && target_text.empty()) {
      return JS_ThrowTypeError(context, "move(target, position): target id must be positive");
    }
  } else if (JS_IsNumber(argv[0])) {
    int64_t raw_id = 0;
    if (JS_ToInt64(context, &raw_id, argv[0])) return JS_EXCEPTION;
    if (raw_id <= 0) {
      return JS_ThrowTypeError(context, "move(target, position): target id must be positive");
    }
    target_text = std::to_string(raw_id);
  } else {
    return JS_ThrowTypeError(context, "move(target, position): target must be an id or 'active'");
  }

  if (!JS_IsString(argv[1])) {
    return JS_ThrowTypeError(context, "move(target, position): position must be a string");
  }
  const char* position_text = JS_ToCString(context, argv[1]);
  if (!position_text) return JS_EXCEPTION;
  std::string placement = position_text;
  JS_FreeCString(context, position_text);
  if (placement.empty()) {
    return JS_ThrowTypeError(context, "move(target, position): position must not be empty");
  }

  json::Value payload = json::Value::object();
  payload.set("position", json::Value::string(placement));

  rime::action::Kernel* kernel = binding->kernel;
  auto action = make_action(*binding->next_action_id, "rime:window", "window.move",
                            "window.write", {"window", std::move(target_text)},
                            json::stringify(payload));
  return run_action(context, *kernel, std::move(action));
}

int window_module_init(JSContext* context, JSModuleDef* module) {
  WindowModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel || !binding->next_action_id) {
    JS_ThrowInternalError(context, "rime:window requires a window module binding");
    return -1;
  }
  JSValue windows = JS_NewObject(context);
  auto add = [&](const char* name, JSCClosure* function, int length) -> bool {
    JSValue value = JS_NewCClosure(context, function, name, nullptr, length, 0, binding);
    if (JS_IsException(value)) {
      JS_FreeValue(context, windows);
      return false;
    }
    JS_SetPropertyStr(context, windows, name, value);
    return true;
  };
  if (!add("list", windows_list, 0) || !add("active", windows_active, 0) ||
      !add("info", windows_info, 1) || !add("move", windows_move, 2)) {
    return -1;
  }
  return JS_SetModuleExport(context, module, "windows", windows);
}

JSModuleDef* create_window_module(JSContext* context) {
  JSModuleDef* module = JS_NewCModule(context, "rime:window", window_module_init);
  if (!module) return nullptr;
  if (JS_AddModuleExport(context, module, "windows") < 0) return nullptr;
  return module;
}

rime::core::Error check_binding(const WindowModuleBinding* binding) {
  if (!binding || !binding->service || !binding->kernel || !binding->next_action_id) {
    return {rime::core::Error::Code::InvalidContract,
            "rime:window requires a window service, kernel and action id source"};
  }
  return rime::core::Error::none();
}

}  // namespace

rime::core::Error register_window_module(rime::js::Host& host, WindowModuleBinding* binding) {
  if (const auto error = check_binding(binding); !error.ok()) return error;
  host.set_module_data("rime:window", binding);
  host.modules().add_native("rime:window",
                            [](JSContext* context) { return create_window_module(context); });
  return rime::core::Error::none();
}

rime::core::Error register_window_module(rime::js::Runtime& runtime,
                                         WindowModuleBinding* binding) {
  if (const auto error = check_binding(binding); !error.ok()) return error;
  return runtime.add_native_module(
      "rime:window", [](JSContext* context) { return create_window_module(context); }, binding);
}

}  // namespace rime::win32
