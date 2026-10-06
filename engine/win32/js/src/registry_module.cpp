#include "rime/win32/js_registry.hpp"

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

// Capability names checked for this module. Pointed at from
// contracts/registry/actions.json (capabilities.registry.read/.write), so the
// literal lives here at the top of the file rather than inline in the bodies.
constexpr const char* kRegistryReadCapability = "registry.read";
constexpr const char* kRegistryWriteCapability = "registry.write";

RegistryModuleBinding* binding_of(JSContext* context) {
  auto* host = static_cast<rime::js::Host*>(JS_GetContextOpaque(context));
  if (!host) return nullptr;
  return static_cast<RegistryModuleBinding*>(host->module_data("rime:registry"));
}

// Copies a JS string into a std::string. Returns false with an exception
// pending when the conversion fails, so callers can just return JS_EXCEPTION.
bool copy_string(JSContext* context, JSValueConst value, std::string& out) {
  const char* text = JS_ToCString(context, value);
  if (!text) return false;
  out = text;
  JS_FreeCString(context, text);
  return true;
}

// One read result: {"type": <lowercase registry type>, "value": <shaped by
// type>} - text for sz/expand_sz, an integer for dword/qword, an array of
// strings for multi_sz and an array of 0..255 numbers for binary.
json::Value reg_value_json(const RegValue& value) {
  if (value.type == "sz" || value.type == "expand_sz") return json::Value::string(value.value_sz);
  if (value.type == "dword" || value.type == "qword") {
    return json::Value::number(static_cast<double>(value.value_int));
  }
  json::Value array = json::Value::array();
  if (value.type == "multi_sz") {
    for (const std::string& item : value.value_multi) array.push(json::Value::string(item));
    return array;
  }
  for (const std::uint8_t byte : value.value_bin) {
    array.push(json::Value::number(static_cast<double>(byte)));
  }
  return array;
}

// Builds the {"view": <view>} object returned by view() and setView().
bool make_view_object(JSContext* context, const std::string& view, JSValue& out) {
  out = JS_NewObject(context);
  if (JS_IsException(out)) return false;
  JSValue value = JS_NewString(context, view.c_str());
  if (JS_IsException(value)) {
    JS_FreeValue(context, out);
    out = JS_EXCEPTION;
    return false;
  }
  // JS_SetPropertyStr consumes `value` on both success and failure.
  if (JS_SetPropertyStr(context, out, "view", value) < 0) {
    JS_FreeValue(context, out);
    out = JS_EXCEPTION;
    return false;
  }
  return true;
}

JSValue registry_read(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                      void*) {
  RegistryModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:registry is not wired");
  }
  if (argc < 1 || argc > 3) return JS_ThrowTypeError(context, "read(key, name?, options?)");
  if (!JS_IsString(argv[0])) {
    return JS_ThrowTypeError(context, "read(key, name?, options?): key must be a string");
  }
  std::string key;
  if (!copy_string(context, argv[0], key)) return JS_EXCEPTION;
  if (key.empty()) {
    return JS_ThrowTypeError(context, "read(key, name?, options?): key must not be empty");
  }

  // Argument shapes: read(key), read(key, options), read(key, name) and
  // read(key, name, options). The second slot is the name only when it is a
  // string, so an options object there still parses.
  std::string name;
  int options_index = -1;
  if (argc == 2) {
    if (JS_IsString(argv[1])) {
      if (!copy_string(context, argv[1], name)) return JS_EXCEPTION;
    } else {
      options_index = 1;
    }
  } else if (argc == 3) {
    if (JS_IsString(argv[1])) {
      if (!copy_string(context, argv[1], name)) return JS_EXCEPTION;
      options_index = 2;
    } else if (JS_IsUndefined(argv[1]) || JS_IsNull(argv[1])) {
      options_index = 2;
    } else {
      return JS_ThrowTypeError(context, "read(key, name?, options?): name must be a string");
    }
  }
  ActionOptions options;
  if (options_index >= 0 && !parse_action_options(context, argv[options_index], options)) {
    return JS_EXCEPTION;
  }

  rime::action::Kernel* kernel = binding->kernel;
  RegistryService* service = binding->service;
  return start_async(
      context,
      [kernel, service, key = std::move(key), name = std::move(name)]() -> AsyncOutcome {
        if (!kernel->allows(kRegistryReadCapability)) {
          return async_failure("capability_denied",
                               std::string("required capability was not granted: ") +
                                   kRegistryReadCapability);
        }
        RegValue value;
        if (const auto error = service->read(key, name, value); !error.ok()) {
          return async_failure(error);
        }
        json::Value result = json::Value::object();
        result.set("type", json::Value::string(value.type));
        result.set("value", reg_value_json(value));
        return async_success(json::stringify(result));
      },
      options.cancellation_id);
}

JSValue registry_write(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                       void*) {
  RegistryModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel || !binding->dispatcher ||
      !binding->next_action_id) {
    return JS_ThrowInternalError(context, "rime:registry is not wired");
  }
  if (argc < 1 || argc > 2) return JS_ThrowTypeError(context, "write(payload, options?)");
  if (!JS_IsObject(argv[0])) {
    return JS_ThrowTypeError(context, "write(payload): payload must be an object");
  }
  // The target id is built here, synchronously, from payload.key - every other
  // field is validated by the executor so a rejected field still reaches the
  // trace as an invalid_contract result instead of a silent local throw.
  JSValue key_property = JS_GetPropertyStr(context, argv[0], "key");
  if (JS_IsException(key_property)) return JS_EXCEPTION;
  std::string key;
  const bool key_is_string = JS_IsString(key_property);
  if (key_is_string && !copy_string(context, key_property, key)) {
    JS_FreeValue(context, key_property);
    return JS_EXCEPTION;
  }
  JS_FreeValue(context, key_property);
  if (!key_is_string || key.empty()) {
    return JS_ThrowTypeError(context, "write(payload): payload.key must be a non-empty string");
  }

  JSValue json_value = JS_JSONStringify(context, argv[0], JS_UNDEFINED, JS_UNDEFINED);
  if (JS_IsException(json_value)) return JS_EXCEPTION;
  if (!JS_IsString(json_value)) {
    JS_FreeValue(context, json_value);
    return JS_ThrowTypeError(context, "write(payload): payload must serialize to JSON");
  }
  std::string payload;
  if (!copy_string(context, json_value, payload)) {
    JS_FreeValue(context, json_value);
    return JS_EXCEPTION;
  }
  JS_FreeValue(context, json_value);

  ActionOptions options;
  if (argc >= 2 && !parse_action_options(context, argv[1], options)) return JS_EXCEPTION;

  auto action = make_action(*binding->next_action_id, "rime:registry", "registry.write",
                            kRegistryWriteCapability, {"registry", key}, std::move(payload),
                            options);
  return run_action(context, *binding->dispatcher, std::move(action), options.cancellation_id);
}

JSValue registry_view(JSContext* context, JSValueConst, int argc, JSValueConst*, int, void*) {
  RegistryModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service) {
    return JS_ThrowInternalError(context, "rime:registry is not wired");
  }
  if (argc > 0) return JS_ThrowTypeError(context, "view()");
  JSValue object = JS_EXCEPTION;
  if (!make_view_object(context, binding->service->view(), object)) return object;
  return object;
}

JSValue registry_set_view(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                          void*) {
  RegistryModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service) {
    return JS_ThrowInternalError(context, "rime:registry is not wired");
  }
  if (argc != 1) return JS_ThrowTypeError(context, "setView(view)");
  if (!JS_IsString(argv[0])) {
    return JS_ThrowTypeError(context, "setView(view): view must be a string");
  }
  std::string view;
  if (!copy_string(context, argv[0], view)) return JS_EXCEPTION;
  // Rejected as a TypeError before the service sees it: a bad view is a
  // caller mistake, not a runtime state change to report back as a value.
  if (view != "default" && view != "64" && view != "32") {
    return JS_ThrowTypeError(context, "setView(view): view must be \"default\", \"64\" or \"32\"");
  }
  if (const auto error = binding->service->set_view(view); !error.ok()) {
    return JS_ThrowInternalError(context, "%s", error.message.c_str());
  }
  JSValue object = JS_EXCEPTION;
  if (!make_view_object(context, binding->service->view(), object)) return object;
  return object;
}

int registry_module_init(JSContext* context, JSModuleDef* module) {
  RegistryModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel || !binding->dispatcher ||
      !binding->next_action_id) {
    JS_ThrowInternalError(context, "rime:registry requires a registry module binding");
    return -1;
  }
  JSValue registry = JS_NewObject(context);
  auto add = [&](const char* name, JSCClosure* function, int length) -> bool {
    JSValue fn = JS_NewCClosure(context, function, name, nullptr, length, 0, binding);
    if (JS_IsException(fn)) {
      JS_FreeValue(context, registry);
      return false;
    }
    // JS_SetPropertyStr consumes `fn` on both success and failure.
    if (JS_SetPropertyStr(context, registry, name, fn) < 0) {
      JS_FreeValue(context, registry);
      return false;
    }
    return true;
  };
  if (!add("read", registry_read, 1) || !add("write", registry_write, 1) ||
      !add("view", registry_view, 0) || !add("setView", registry_set_view, 1)) {
    return -1;
  }
  return JS_SetModuleExport(context, module, "registry", registry);
}

JSModuleDef* create_registry_module(JSContext* context) {
  JSModuleDef* module = JS_NewCModule(context, "rime:registry", registry_module_init);
  if (!module) return nullptr;
  if (JS_AddModuleExport(context, module, "registry") < 0) return nullptr;
  return module;
}

rime::core::Error check_binding(const RegistryModuleBinding* binding) {
  if (!binding || !binding->service || !binding->kernel || !binding->dispatcher ||
      !binding->next_action_id) {
    return {rime::core::Error::Code::InvalidContract,
            "rime:registry requires a registry service, kernel, dispatcher and action id "
            "source"};
  }
  return rime::core::Error::none();
}

}  // namespace

rime::core::Error register_registry_module(rime::js::Host& host, RegistryModuleBinding* binding) {
  if (const auto error = check_binding(binding); !error.ok()) return error;
  host.set_module_data("rime:registry", binding);
  if (const auto error = host.modules().add_native(
          "rime:registry", [](JSContext* context) { return create_registry_module(context); });
      !error.ok()) {
    return error;
  }
  return rime::core::Error::none();
}

rime::core::Error register_registry_module(rime::js::Runtime& runtime,
                                            RegistryModuleBinding* binding) {
  if (const auto error = check_binding(binding); !error.ok()) return error;
  return runtime.add_native_module(
      "rime:registry", [](JSContext* context) { return create_registry_module(context); },
      binding);
}

}  // namespace rime::win32
