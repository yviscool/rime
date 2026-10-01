#include "rime/win32/js_clipboard.hpp"

#include "rime/core/json.hpp"
#include "rime/js/host.hpp"
#include "rime/js/runtime.hpp"

#include "async_task.hpp"
#include "quickjs.h"

#include <string>
#include <utility>

namespace rime::win32 {
namespace {

namespace json = rime::core::json;

ClipboardModuleBinding* binding_of(JSContext* context) {
  auto* host = static_cast<rime::js::Host*>(JS_GetContextOpaque(context));
  if (!host) return nullptr;
  return static_cast<ClipboardModuleBinding*>(host->module_data("rime:clipboard"));
}

JSValue clipboard_read(JSContext* context, JSValueConst, int, JSValueConst*, int, void*) {
  ClipboardModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service) {
    return JS_ThrowInternalError(context, "rime:clipboard is not wired");
  }
  ClipboardService* service = binding->service;
  return start_async(context, [service]() -> std::pair<bool, std::string> {
    std::string text;
    if (const auto error = service->read_text(text); !error.ok()) return {false, error.message};
    json::Value value = json::Value::object();
    value.set("text", json::Value::string(text));
    return {true, json::stringify(value)};
  });
}

JSValue clipboard_write(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                        void*) {
  ClipboardModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel || !binding->next_action_id) {
    return JS_ThrowInternalError(context, "rime:clipboard is not wired");
  }
  if (argc < 1) return JS_ThrowTypeError(context, "write(text)");
  if (!JS_IsString(argv[0])) {
    return JS_ThrowTypeError(context, "write(text): text must be a string");
  }
  const char* text = JS_ToCString(context, argv[0]);
  if (!text) return JS_EXCEPTION;
  std::string value(text);
  JS_FreeCString(context, text);

  json::Value payload = json::Value::object();
  payload.set("text", json::Value::string(value));

  rime::action::Kernel* kernel = binding->kernel;
  auto action = make_action(*binding->next_action_id, "rime:clipboard", "clipboard.write",
                            "clipboard.write", {"clipboard", "default"}, json::stringify(payload));
  return run_action(context, *kernel, std::move(action));
}

int clipboard_module_init(JSContext* context, JSModuleDef* module) {
  ClipboardModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel || !binding->next_action_id) {
    JS_ThrowInternalError(context, "rime:clipboard requires a clipboard module binding");
    return -1;
  }
  JSValue clipboard = JS_NewObject(context);
  auto add = [&](const char* name, JSCClosure* function, int length) -> bool {
    JSValue fn = JS_NewCClosure(context, function, name, nullptr, length, 0, binding);
    if (JS_IsException(fn)) {
      JS_FreeValue(context, clipboard);
      return false;
    }
    JS_SetPropertyStr(context, clipboard, name, fn);
    return true;
  };
  if (!add("read", clipboard_read, 0) || !add("write", clipboard_write, 1)) {
    return -1;
  }
  return JS_SetModuleExport(context, module, "clipboard", clipboard);
}

JSModuleDef* create_clipboard_module(JSContext* context) {
  JSModuleDef* module = JS_NewCModule(context, "rime:clipboard", clipboard_module_init);
  if (!module) return nullptr;
  if (JS_AddModuleExport(context, module, "clipboard") < 0) return nullptr;
  return module;
}

rime::core::Error check_binding(const ClipboardModuleBinding* binding) {
  if (!binding || !binding->service || !binding->kernel || !binding->next_action_id) {
    return {rime::core::Error::Code::InvalidContract,
            "rime:clipboard requires a clipboard service, kernel and action id source"};
  }
  return rime::core::Error::none();
}

}  // namespace

rime::core::Error register_clipboard_module(rime::js::Host& host, ClipboardModuleBinding* binding) {
  if (const auto error = check_binding(binding); !error.ok()) return error;
  host.set_module_data("rime:clipboard", binding);
  host.modules().add_native("rime:clipboard",
                            [](JSContext* context) { return create_clipboard_module(context); });
  return rime::core::Error::none();
}

rime::core::Error register_clipboard_module(rime::js::Runtime& runtime,
                                            ClipboardModuleBinding* binding) {
  if (const auto error = check_binding(binding); !error.ok()) return error;
  return runtime.add_native_module(
      "rime:clipboard", [](JSContext* context) { return create_clipboard_module(context); },
      binding);
}

}  // namespace rime::win32
