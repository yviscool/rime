#include "rime/win32/js_process.hpp"

#include "rime/core/json.hpp"
#include "rime/js/host.hpp"
#include "rime/js/runtime.hpp"

#include "async_task.hpp"
#include "quickjs.h"

#include <string>
#include <vector>

namespace rime::win32 {
namespace {

namespace json = rime::core::json;

ProcessModuleBinding* binding_of(JSContext* context) {
  auto* host = static_cast<rime::js::Host*>(JS_GetContextOpaque(context));
  if (!host) return nullptr;
  return static_cast<ProcessModuleBinding*>(host->module_data("rime:process"));
}

// Reads an optional string property; returns false (with a TypeError raised)
// when present but not a string. Missing/undefined yields `out` untouched.
bool optional_string(JSContext* context, JSValueConst object, const char* name, std::string& out) {
  JSValue property = JS_GetPropertyStr(context, object, name);
  if (JS_IsException(property)) return false;
  if (!JS_IsUndefined(property)) {
    if (!JS_IsString(property)) {
      JS_FreeValue(context, property);
      JS_ThrowTypeError(context, "options.%s must be a string", name);
      return false;
    }
    const char* text = JS_ToCString(context, property);
    if (!text) {
      JS_FreeValue(context, property);
      return false;
    }
    out = text;
    JS_FreeCString(context, text);
  }
  JS_FreeValue(context, property);
  return true;
}

JSValue process_list(JSContext* context, JSValueConst, int, JSValueConst*, int, void*) {
  ProcessModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service) {
    return JS_ThrowInternalError(context, "rime:process is not wired");
  }
  ProcessService* service = binding->service;
  return start_async(context, [service]() -> std::pair<bool, std::string> {
    std::vector<ProcessInfo> processes;
    if (const auto error = service->list(processes); !error.ok()) return {false, error.message};
    json::Value array = json::Value::array();
    for (const auto& process : processes) array.push(process_info_json(process));
    return {true, json::stringify(array)};
  });
}

JSValue process_info(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int, void*) {
  ProcessModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service) {
    return JS_ThrowInternalError(context, "rime:process is not wired");
  }
  if (argc < 1) return JS_ThrowTypeError(context, "info(pid)");
  int64_t raw_pid = 0;
  if (JS_ToInt64(context, &raw_pid, argv[0])) return JS_EXCEPTION;
  if (raw_pid <= 0) return JS_ThrowTypeError(context, "info(pid): pid must be positive");
  const std::uint32_t pid = static_cast<std::uint32_t>(raw_pid);
  ProcessService* service = binding->service;
  return start_async(context, [service, pid]() -> std::pair<bool, std::string> {
    ProcessInfo info;
    if (const auto error = service->info(pid, info); !error.ok()) return {false, error.message};
    return {true, json::stringify(process_info_json(info))};
  });
}

JSValue process_launch(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                       void*) {
  ProcessModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel || !binding->next_action_id) {
    return JS_ThrowInternalError(context, "rime:process is not wired");
  }
  if (argc < 1) return JS_ThrowTypeError(context, "launch(options)");
  if (!JS_IsObject(argv[0])) {
    return JS_ThrowTypeError(context, "launch(options): options must be an object");
  }

  std::string command;
  JSValue property = JS_GetPropertyStr(context, argv[0], "command");
  if (JS_IsException(property)) return JS_EXCEPTION;
  if (!JS_IsString(property)) {
    JS_FreeValue(context, property);
    return JS_ThrowTypeError(context, "launch(options): command must be a non-empty string");
  }
  const char* command_text = JS_ToCString(context, property);
  JS_FreeValue(context, property);
  if (!command_text) return JS_EXCEPTION;
  command = command_text;
  JS_FreeCString(context, command_text);
  if (command.empty()) {
    return JS_ThrowTypeError(context, "launch(options): command must be a non-empty string");
  }

  std::string args;
  std::string working_dir;
  if (!optional_string(context, argv[0], "args", args) ||
      !optional_string(context, argv[0], "workingDir", working_dir)) {
    return JS_EXCEPTION;
  }

  json::Value payload = json::Value::object();
  payload.set("command", json::Value::string(command));
  if (!args.empty()) payload.set("args", json::Value::string(args));
  if (!working_dir.empty()) payload.set("workingDir", json::Value::string(working_dir));

  rime::action::Kernel* kernel = binding->kernel;
  auto action = make_action(*binding->next_action_id, "rime:process", "process.launch",
                            "process.launch", {"process", "new"}, json::stringify(payload));
  return run_action(context, *kernel, std::move(action));
}

JSValue process_terminate(JSContext* context, JSValueConst, int argc, JSValueConst* argv, int,
                          void*) {
  ProcessModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel || !binding->next_action_id) {
    return JS_ThrowInternalError(context, "rime:process is not wired");
  }
  if (argc < 1) return JS_ThrowTypeError(context, "terminate(pid)");
  int64_t raw_pid = 0;
  if (JS_ToInt64(context, &raw_pid, argv[0])) return JS_EXCEPTION;
  if (raw_pid <= 0) return JS_ThrowTypeError(context, "terminate(pid): pid must be positive");

  rime::action::Kernel* kernel = binding->kernel;
  auto action = make_action(*binding->next_action_id, "rime:process", "process.terminate",
                            "process.terminate", {"process", std::to_string(raw_pid)}, "{}");
  return run_action(context, *kernel, std::move(action));
}

int process_module_init(JSContext* context, JSModuleDef* module) {
  ProcessModuleBinding* binding = binding_of(context);
  if (!binding || !binding->service || !binding->kernel || !binding->next_action_id) {
    JS_ThrowInternalError(context, "rime:process requires a process module binding");
    return -1;
  }
  JSValue process = JS_NewObject(context);
  auto add = [&](const char* name, JSCClosure* function, int length) -> bool {
    JSValue value = JS_NewCClosure(context, function, name, nullptr, length, 0, binding);
    if (JS_IsException(value)) {
      JS_FreeValue(context, process);
      return false;
    }
    JS_SetPropertyStr(context, process, name, value);
    return true;
  };
  if (!add("list", process_list, 0) || !add("info", process_info, 1) ||
      !add("launch", process_launch, 1) || !add("terminate", process_terminate, 1)) {
    return -1;
  }
  return JS_SetModuleExport(context, module, "process", process);
}

JSModuleDef* create_process_module(JSContext* context) {
  JSModuleDef* module = JS_NewCModule(context, "rime:process", process_module_init);
  if (!module) return nullptr;
  if (JS_AddModuleExport(context, module, "process") < 0) return nullptr;
  return module;
}

rime::core::Error check_binding(const ProcessModuleBinding* binding) {
  if (!binding || !binding->service || !binding->kernel || !binding->next_action_id) {
    return {rime::core::Error::Code::InvalidContract,
            "rime:process requires a process service, kernel and action id source"};
  }
  return rime::core::Error::none();
}

}  // namespace

rime::core::Error register_process_module(rime::js::Host& host, ProcessModuleBinding* binding) {
  if (const auto error = check_binding(binding); !error.ok()) return error;
  host.set_module_data("rime:process", binding);
  host.modules().add_native("rime:process",
                            [](JSContext* context) { return create_process_module(context); });
  return rime::core::Error::none();
}

rime::core::Error register_process_module(rime::js::Runtime& runtime,
                                          ProcessModuleBinding* binding) {
  if (const auto error = check_binding(binding); !error.ok()) return error;
  return runtime.add_native_module(
      "rime:process", [](JSContext* context) { return create_process_module(context); }, binding);
}

}  // namespace rime::win32
