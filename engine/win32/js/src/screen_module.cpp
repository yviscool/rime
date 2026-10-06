#include "rime/win32/js_screen.hpp"

#include "rime/core/json.hpp"
#include "rime/js/host.hpp"
#include "rime/js/runtime.hpp"

#include "async_task.hpp"
#include "quickjs.h"

#include <string>

namespace rime::win32 {
namespace {

namespace json = rime::core::json;

ScreenModuleBinding* binding_of(JSContext* context) {
  auto* host = static_cast<rime::js::Host*>(JS_GetContextOpaque(context));
  if (!host) return nullptr;
  return static_cast<ScreenModuleBinding*>(host->module_data("rime:screen"));
}

// Capability name checked by this module. Pointed at from
// contracts/registry/actions.json (capabilities.screen.capture), so the
// literal lives here at the top of the file rather than inline in the bodies.
constexpr const char* kScreenCaptureCapability = "screen.capture";

json::Value rect_value(const int left, const int top, const int right, const int bottom) {
  json::Value value = json::Value::object();
  value.set("left", json::Value::number(left));
  value.set("top", json::Value::number(top));
  value.set("right", json::Value::number(right));
  value.set("bottom", json::Value::number(bottom));
  return value;
}

json::Value monitor_value(const ScreenService::Monitor& monitor) {
  json::Value value = json::Value::object();
  value.set("index", json::Value::number(monitor.index));
  value.set("primary", json::Value::boolean(monitor.primary));
  value.set("name", json::Value::string(monitor.name));
  value.set("bounds", rect_value(monitor.left, monitor.top, monitor.right, monitor.bottom));
  value.set("work",
            rect_value(monitor.work_left, monitor.work_top, monitor.work_right, monitor.work_bottom));
  return value;
}

// Splits `screen.monitor(index?, options?)`: a leading number is the index,
// anything else must be the options object. Returns false with an exception
// pending on a shape the caller must reject synchronously.
bool parse_monitor_args(JSContext* context, int argc, JSValueConst* argv, int& index,
                        ActionOptions& options) {
  index = 0;
  int next = 0;
  if (argc > 0 && JS_IsNumber(argv[0])) {
    if (JS_ToInt32(context, &index, argv[0]) < 0) return false;
    if (index < 0) {
      JS_ThrowRangeError(context, "monitor(index): index must be 0 or a positive number");
      return false;
    }
    next = 1;
  } else if (argc > 0 && !JS_IsObject(argv[0])) {
    JS_ThrowTypeError(context, "monitor(index?, options?): index must be a number");
    return false;
  }
  if (argc - next > 1 || (argc - next == 1 && !JS_IsObject(argv[next]))) {
    JS_ThrowTypeError(context, "monitor(index?, options?): expected monitor(index?, options?)");
    return false;
  }
  if (argc - next == 1) return parse_action_options(context, argv[next], options);
  return true;
}

// screen.monitorCount(options?): AHK MonitorGetCount. EnumDisplayMonitors
// order, same as AHK, so the count and the indexes of `monitor()` agree.
// Capability: screen.capture. Query, so no Action Trace.
JSValue screen_monitor_count(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                             void*) {
  ScreenModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:screen is not wired");
  }
  if (argc > 1) return JS_ThrowTypeError(context, "monitorCount(options?)");
  ActionOptions options;
  if (argc == 1 && !parse_action_options(context, argv[0], options)) return JS_EXCEPTION;
  rime::action::Kernel* kernel = binding->kernel;
  ScreenService* service = binding->service;
  return start_async(
      context,
      [kernel, service]() -> AsyncOutcome {
        if (!kernel->allows(kScreenCaptureCapability)) {
          return capability_denied(kScreenCaptureCapability);
        }
        int count = 0;
        if (const auto error = service->monitor_count(count); !error.ok()) {
          return async_failure(error);
        }
        json::Value value = json::Value::object();
        value.set("count", json::Value::number(count));
        return async_success(json::stringify(value));
      },
      options.cancellation_id);
}

// screen.monitor(index?, options?): one record that answers AHK's MonitorGet,
// MonitorGetWorkArea, MonitorGetName and MonitorGetPrimary together - bounds,
// work area, device name and the primary flag of a single monitor. An omitted
// index means the primary monitor, exactly as AHK's omitted argument does.
// Capability: screen.capture. Query, so no Action Trace.
JSValue screen_monitor(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                       void*) {
  ScreenModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    return JS_ThrowInternalError(context, "rime:screen is not wired");
  }
  int index = 0;
  ActionOptions options;
  if (!parse_monitor_args(context, argc, argv, index, options)) return JS_EXCEPTION;
  rime::action::Kernel* kernel = binding->kernel;
  ScreenService* service = binding->service;
  return start_async(
      context,
      [kernel, service, index]() -> AsyncOutcome {
        if (!kernel->allows(kScreenCaptureCapability)) {
          return capability_denied(kScreenCaptureCapability);
        }
        ScreenService::Monitor monitor;
        if (const auto error = service->monitor_at(index, monitor); !error.ok()) {
          return async_failure(error);
        }
        return async_success(json::stringify(monitor_value(monitor)));
      },
      options.cancellation_id);
}

int screen_module_init(JSContext* context, JSModuleDef* module) {
  ScreenModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel) {
    JS_ThrowInternalError(context, "rime:screen requires a screen module binding");
    return -1;
  }
  JSValue screen = JS_NewObject(context);
  auto add = [&](const char* name, JSCClosure* function, int length) -> bool {
    JSValue fn = JS_NewCClosure(context, function, name, nullptr, length, 0, binding);
    if (JS_IsException(fn)) {
      JS_FreeValue(context, screen);
      return false;
    }
    // JS_SetPropertyStr consumes `fn` on both success and failure.
    if (JS_SetPropertyStr(context, screen, name, fn) < 0) {
      JS_FreeValue(context, screen);
      return false;
    }
    return true;
  };
  if (!add("monitorCount", screen_monitor_count, 0) || !add("monitor", screen_monitor, 0)) {
    return -1;
  }
  return JS_SetModuleExport(context, module, "screen", screen);
}

JSModuleDef* create_screen_module(JSContext* context) {
  JSModuleDef* module = JS_NewCModule(context, "rime:screen", screen_module_init);
  if (!module) return nullptr;
  if (JS_AddModuleExport(context, module, "screen") < 0) return nullptr;
  return module;
}

rime::core::Error check_binding(const ScreenModuleBinding* binding) {
  if (!binding || !binding->service || !binding->kernel) {
    return {rime::core::Error::Code::InvalidContract,
            "rime:screen requires a screen service and kernel"};
  }
  return rime::core::Error::none();
}

}  // namespace

rime::core::Error register_screen_module(rime::js::Host& host, ScreenModuleBinding* binding) {
  if (const auto error = check_binding(binding); !error.ok()) return error;
  host.set_module_data("rime:screen", binding);
  if (const auto error = host.modules().add_native(
          "rime:screen", [](JSContext* context) { return create_screen_module(context); });
      !error.ok()) {
    return error;
  }
  return rime::core::Error::none();
}

rime::core::Error register_screen_module(rime::js::Runtime& runtime,
                                         ScreenModuleBinding* binding) {
  if (const auto error = check_binding(binding); !error.ok()) return error;
  return runtime.add_native_module(
      "rime:screen", [](JSContext* context) { return create_screen_module(context); }, binding);
}

}  // namespace rime::win32
