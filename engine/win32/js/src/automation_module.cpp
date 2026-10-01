#include "rime/win32/js_automation.hpp"

#include "async_task.hpp"
#include "rime/core/json.hpp"
#include "rime/js/host.hpp"
#include "rime/js/runtime.hpp"

#include "quickjs.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>

namespace rime::win32 {
namespace {

namespace json = rime::core::json;

rime::js::Host* host_of(JSContext* context) {
  return static_cast<rime::js::Host*>(JS_GetContextOpaque(context));
}

AutomationModuleBinding* binding_of(JSContext* context) {
  auto* host = host_of(context);
  if (!host) return nullptr;
  return static_cast<AutomationModuleBinding*>(host->module_data("rime:automation"));
}

// Strict JS number -> uint64 element id: non-numbers, NaN/Infinity,
// fractions, negatives and values beyond 2^53-1 raise a TypeError.
bool strict_element_id(JSContext* context, JSValueConst value, std::uint64_t& out,
                       const char* what) {
  if (!JS_IsNumber(value)) {
    JS_ThrowTypeError(context, "%s: elementId must be a number", what);
    return false;
  }
  double number = 0;
  if (JS_ToFloat64(context, &number, value)) return false;
  if (!std::isfinite(number) || std::trunc(number) != number) {
    JS_ThrowTypeError(context, "%s: elementId must be an integer", what);
    return false;
  }
  constexpr double kMaxSafeInteger = 9007199254740991.0;  // 2^53 - 1
  if (number < 0.0 || number > kMaxSafeInteger) {
    JS_ThrowTypeError(context, "%s: elementId must be positive", what);
    return false;
  }
  out = static_cast<std::uint64_t>(number);
  if (out == 0) {
    JS_ThrowTypeError(context, "%s: elementId must be positive", what);
    return false;
  }
  return true;
}

// Capability denials are thrown Errors naming the policy entry, mirroring
// the hook/input modules (the kernel enforces the same policy again when
// the queued action executes).
JSValue throw_capability_error(JSContext* context, const char* capability) {
  const std::string text = std::string("required capability was not granted: ") + capability;
  JSValue error = JS_NewError(context);
  if (JS_IsException(error)) return JS_EXCEPTION;
  JSValue message = JS_NewString(context, text.c_str());
  if (JS_IsException(message)) {
    JS_FreeValue(context, error);
    return JS_EXCEPTION;
  }
  // JS_DefinePropertyValueStr consumes `message` on both success and failure.
  if (JS_DefinePropertyValueStr(context, error, "message", message, JS_PROP_WRITABLE) < 0) {
    JS_FreeValue(context, error);
    return JS_EXCEPTION;
  }
  return JS_Throw(context, error);
}

// Optional string field of the find query: missing leaves `present` false;
// a non-string throws a TypeError naming the field.
bool optional_query_string(JSContext* context, JSValueConst object, const char* field,
                           std::string& out, bool& present) {
  JSValue value = JS_GetPropertyStr(context, object, field);
  if (JS_IsException(value)) return false;
  if (JS_IsUndefined(value) || JS_IsNull(value)) return true;
  if (!JS_IsString(value)) {
    JS_FreeValue(context, value);
    JS_ThrowTypeError(context, "find(query): query.%s must be a string", field);
    return false;
  }
  const char* text = JS_ToCString(context, value);
  JS_FreeValue(context, value);
  if (!text) return false;
  out = text;
  present = true;
  JS_FreeCString(context, text);
  return true;
}

// Optional integer field of the find query within [min, max].
bool optional_query_u64(JSContext* context, JSValueConst object, const char* field,
                        std::uint64_t min, std::uint64_t max, std::uint64_t& out) {
  JSValue value = JS_GetPropertyStr(context, object, field);
  if (JS_IsException(value)) return false;
  if (JS_IsUndefined(value) || JS_IsNull(value)) return true;
  double number = 0;
  if (!JS_IsNumber(value) || JS_ToFloat64(context, &number, value) || !std::isfinite(number) ||
      std::trunc(number) != number || number < static_cast<double>(min) ||
      number > static_cast<double>(max)) {
    JS_FreeValue(context, value);
    JS_ThrowTypeError(context, "find(query): query.%s must be an integer in %llu..%llu", field,
                      static_cast<unsigned long long>(min), static_cast<unsigned long long>(max));
    return false;
  }
  out = static_cast<std::uint64_t>(number);
  JS_FreeValue(context, value);
  return true;
}

bool is_plain_object(JSValueConst value) {
  return JS_IsObject(value) && !JS_IsArray(value) && !JS_IsNull(value);
}

// find(query[, options]) -> { elements: [...] }
// Shape errors (arity, object-ness, field types) throw synchronously; the
// queued action carries the validated query to UiaExecutor.
JSValue automation_find(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                        void* opaque) {
  auto* binding = static_cast<AutomationModuleBinding*>(opaque);
  if (!binding || !binding->service || !binding->kernel || !binding->dispatcher ||
      !binding->next_action_id) {
    return JS_ThrowInternalError(context, "rime:automation is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "find(query[, options])");
  if (!is_plain_object(argv[0])) {
    return JS_ThrowTypeError(context, "find(query): query must be an object");
  }
  if (!binding->kernel->allows("windows.automation.find")) {
    return throw_capability_error(context, "windows.automation.find");
  }

  std::string name;
  std::string control_type;
  std::string automation_id;
  bool name_present = false;
  bool control_type_present = false;
  bool automation_id_present = false;
  if (!optional_query_string(context, argv[0], "name", name, name_present) ||
      !optional_query_string(context, argv[0], "controlType", control_type,
                             control_type_present) ||
      !optional_query_string(context, argv[0], "automationId", automation_id,
                             automation_id_present)) {
    return JS_EXCEPTION;
  }
  std::uint64_t from_id = 0;
  std::uint64_t max_results = 8;
  if (!optional_query_u64(context, argv[0], "fromId", 0, 9007199254740991ull, from_id) ||
      !optional_query_u64(context, argv[0], "maxResults", 1, 64, max_results)) {
    return JS_EXCEPTION;
  }

  if (!name_present && !control_type_present && !automation_id_present) {
    return JS_ThrowTypeError(
        context,
        "find(query): query requires at least one of name, controlType or automationId");
  }

  json::Value payload = json::Value::object();
  if (name_present) payload.set("name", json::Value::string(name));
  if (control_type_present) payload.set("controlType", json::Value::string(control_type));
  if (automation_id_present) payload.set("automationId", json::Value::string(automation_id));
  if (from_id != 0) payload.set("fromId", json::Value::number(static_cast<double>(from_id)));
  if (max_results != 8) {
    payload.set("maxResults", json::Value::number(static_cast<double>(max_results)));
  }

  ActionOptions options;
  if (argc == 2 && !parse_action_options(context, argv[1], options)) return JS_EXCEPTION;
  auto action = make_action(*binding->next_action_id, "rime:automation", "automation.find",
                            "windows.automation.find", {"automation", "desktop"},
                            json::stringify(payload), options);
  return run_action(context, *binding->dispatcher, std::move(action), options.cancellation_id);
}

// read(elementId[, options]) -> element snapshot
JSValue automation_read(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                        void* opaque) {
  auto* binding = static_cast<AutomationModuleBinding*>(opaque);
  if (!binding || !binding->service || !binding->kernel || !binding->dispatcher ||
      !binding->next_action_id) {
    return JS_ThrowInternalError(context, "rime:automation is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "read(elementId[, options])");
  std::uint64_t element_id = 0;
  if (!strict_element_id(context, argv[0], element_id, "read(elementId)")) return JS_EXCEPTION;
  if (!binding->kernel->allows("windows.automation.read")) {
    return throw_capability_error(context, "windows.automation.read");
  }

  ActionOptions options;
  if (argc == 2 && !parse_action_options(context, argv[1], options)) return JS_EXCEPTION;
  auto action = make_action(*binding->next_action_id, "rime:automation", "automation.read",
                            "windows.automation.read",
                            {"element", std::to_string(element_id)}, "{}", options);
  return run_action(context, *binding->dispatcher, std::move(action), options.cancellation_id);
}

// invoke(elementId[, options]) -> { invoked: true }
JSValue automation_invoke(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                          void* opaque) {
  auto* binding = static_cast<AutomationModuleBinding*>(opaque);
  if (!binding || !binding->service || !binding->kernel || !binding->dispatcher ||
      !binding->next_action_id) {
    return JS_ThrowInternalError(context, "rime:automation is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "invoke(elementId[, options])");
  std::uint64_t element_id = 0;
  if (!strict_element_id(context, argv[0], element_id, "invoke(elementId)")) return JS_EXCEPTION;
  if (!binding->kernel->allows("windows.automation.invoke")) {
    return throw_capability_error(context, "windows.automation.invoke");
  }

  ActionOptions options;
  if (argc == 2 && !parse_action_options(context, argv[1], options)) return JS_EXCEPTION;
  auto action = make_action(*binding->next_action_id, "rime:automation", "automation.invoke",
                            "windows.automation.invoke",
                            {"element", std::to_string(element_id)}, "{}", options);
  return run_action(context, *binding->dispatcher, std::move(action), options.cancellation_id);
}

// release(elementId) -> boolean
// Synchronous local reference drop - no action, no capability: it only
// forgets our own element handle. Unknown ids (or a stopped service)
// answer false instead of throwing.
JSValue automation_release(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                           void* opaque) {
  auto* binding = static_cast<AutomationModuleBinding*>(opaque);
  if (!binding || !binding->service) {
    return JS_ThrowInternalError(context, "rime:automation is not wired");
  }
  if (argc != 1) return JS_ThrowTypeError(context, "release(elementId)");
  std::uint64_t element_id = 0;
  if (!strict_element_id(context, argv[0], element_id, "release(elementId)")) return JS_EXCEPTION;
  return JS_NewBool(context, binding->service->release(element_id) ? 1 : 0);
}

int automation_module_init(JSContext* context, JSModuleDef* module) {
  auto* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    JS_ThrowInternalError(context, "rime:automation requires an automation module binding");
    return -1;
  }
  JSValue automation = JS_NewObject(context);
  auto add = [&](const char* name, JSCClosure* function, int length) -> bool {
    JSValue value = JS_NewCClosure(context, function, name, nullptr, length, 0, binding);
    if (JS_IsException(value)) {
      JS_FreeValue(context, automation);
      return false;
    }
    // JS_SetPropertyStr consumes `value` on both success and failure.
    if (JS_SetPropertyStr(context, automation, name, value) < 0) {
      JS_FreeValue(context, automation);
      return false;
    }
    return true;
  };
  if (!add("find", automation_find, 1) || !add("read", automation_read, 1) ||
      !add("invoke", automation_invoke, 1) || !add("release", automation_release, 1)) {
    return -1;
  }
  return JS_SetModuleExport(context, module, "automation", automation);
}

JSModuleDef* create_automation_module(JSContext* context) {
  JSModuleDef* module = JS_NewCModule(context, "rime:automation", automation_module_init);
  if (!module) return nullptr;
  if (JS_AddModuleExport(context, module, "automation") < 0) return nullptr;
  return module;
}

rime::core::Error check_binding(const AutomationModuleBinding* binding) {
  if (!binding || !binding->service || !binding->kernel || !binding->dispatcher ||
      !binding->next_action_id) {
    return {rime::core::Error::Code::InvalidContract,
            "rime:automation requires an automation service, kernel and dispatcher"};
  }
  return rime::core::Error::none();
}

}  // namespace

rime::core::Error register_automation_module(rime::js::Host& host,
                                             AutomationModuleBinding* binding) {
  if (const auto error = check_binding(binding); !error.ok()) return error;
  host.set_module_data("rime:automation", binding);
  host.modules().add_native("rime:automation",
                            [](JSContext* context) { return create_automation_module(context); });
  return rime::core::Error::none();
}

rime::core::Error register_automation_module(rime::js::Runtime& runtime,
                                             AutomationModuleBinding* binding) {
  if (const auto error = check_binding(binding); !error.ok()) return error;
  return runtime.add_native_module(
      "rime:automation", [](JSContext* context) { return create_automation_module(context); },
      binding);
}

}  // namespace rime::win32
