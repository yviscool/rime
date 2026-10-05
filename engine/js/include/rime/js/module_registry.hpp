#pragma once

#include "rime/core/types.hpp"

#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

struct JSContext;
struct JSModuleDef;

namespace rime::js {

// Maps `rime:*` specifiers to native C modules and confines file-based ES
// modules to an explicit root directory. Used as the QuickJS module loader.
class ModuleRegistry final {
 public:
  using NativeFactory = std::function<JSModuleDef*(JSContext*)>;

  // Registers a native `rime:*` module. An empty name, a null factory or a
  // name that is already registered fails with InvalidContract, so two
  // registrations can never silently shadow each other in load()'s
  // first-match lookup.
  rime::core::Error add_native(std::string name, NativeFactory factory);
  // Enables on-disk ES modules confined to `root`. Empty disables file modules.
  rime::core::Error set_file_root(std::string root);

  JSModuleDef* load(JSContext* context, const char* module_name);
  // Returns a js_malloc'd specifier for QuickJS, or nullptr on refusal
  // (an exception is then thrown into the context).
  char* normalize(JSContext* context, const char* base_name, const char* module_name);

  [[nodiscard]] std::vector<std::string> native_modules() const;
  [[nodiscard]] std::vector<std::string> loaded_files() const;
  [[nodiscard]] std::string file_root() const;

 private:
  bool confined(const std::string& candidate) const;

  mutable std::mutex mutex_;
  std::vector<std::pair<std::string, NativeFactory>> natives_;
  std::string file_root_;
  std::vector<std::string> loaded_files_;
};

}  // namespace rime::js
