#include "rime/js/module_registry.hpp"

#include "quickjs.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace rime::js {
namespace {

namespace fs = std::filesystem;

std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](const unsigned char character) { return static_cast<char>(std::tolower(character)); });
  return value;
}

bool starts_with(const std::string& value, const std::string& prefix) {
  return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

char* duplicate(JSContext* context, const std::string& value) {
  auto* copy = static_cast<char*>(js_malloc(context, value.size() + 1));
  if (!copy) return nullptr;
  std::memcpy(copy, value.c_str(), value.size() + 1);
  return copy;
}

}  // namespace

rime::core::Error ModuleRegistry::add_native(std::string name, NativeFactory factory) {
  if (name.empty() || !factory) {
    return {rime::core::Error::Code::InvalidContract,
            "native module registration requires a name and factory"};
  }
  std::lock_guard lock(mutex_);
  // Exact-match duplicate check mirrors load()'s exact-match lookup: a
  // second registration of the same specifier would otherwise be silently
  // unreachable behind the first match.
  for (const auto& entry : natives_) {
    if (entry.first == name) {
      return {rime::core::Error::Code::InvalidContract,
              "native module is already registered: " + name};
    }
  }
  natives_.emplace_back(std::move(name), std::move(factory));
  return rime::core::Error::none();
}

rime::core::Error ModuleRegistry::set_file_root(std::string root) {
  if (root.empty()) {
    std::lock_guard lock(mutex_);
    file_root_.clear();
    return rime::core::Error::none();
  }
  std::error_code error;
  const fs::path canonical = fs::weakly_canonical(fs::path(root), error);
  if (error) {
    return {rime::core::Error::Code::InvalidState,
            "cannot resolve module file root: " + error.message()};
  }
  if (!fs::is_directory(canonical, error)) {
    return {rime::core::Error::Code::InvalidState,
            "module file root is not a directory: " + canonical.string()};
  }
  std::lock_guard lock(mutex_);
  file_root_ = canonical.string();
  return rime::core::Error::none();
}

bool ModuleRegistry::confined(const std::string& candidate) const {
  std::error_code error;
  const fs::path canonical = fs::weakly_canonical(fs::path(candidate), error);
  if (error) return false;
  std::string root;
  {
    std::lock_guard lock(mutex_);
    root = file_root_;
  }
  if (root.empty()) return false;
  const std::string root_text = lowercase(root);
  const std::string candidate_text = lowercase(canonical.string());
  if (!starts_with(candidate_text, root_text)) return false;
  if (candidate_text.size() == root_text.size()) return true;
  const char next = candidate_text[root_text.size()];
  return next == '/' || next == '\\';
}

char* ModuleRegistry::normalize(JSContext* context, const char* base_name,
                                const char* module_name) {
  if (!module_name) return nullptr;
  const std::string name(module_name);
  if (starts_with(name, "rime:")) return duplicate(context, name);

  fs::path path(name);
  if (path.is_relative()) {
    fs::path base(base_name ? base_name : "");
    path = base.parent_path() / path;
  }
  path = path.lexically_normal();
  if (path.is_relative()) {
    std::string root;
    {
      std::lock_guard lock(mutex_);
      root = file_root_;
    }
    if (root.empty()) {
      JS_ThrowReferenceError(context, "file modules are disabled (no file root)");
      return nullptr;
    }
    path = fs::path(root) / path;
  }
  const fs::path normalized = path.lexically_normal();
  if (!confined(normalized.string())) {
    JS_ThrowReferenceError(context, "module path escapes the file root: %s",
                           normalized.string().c_str());
    return nullptr;
  }
  return duplicate(context, normalized.string());
}

JSModuleDef* ModuleRegistry::load(JSContext* context, const char* module_name) {
  if (!module_name) return nullptr;
  const std::string name(module_name);
  if (starts_with(name, "rime:")) {
    NativeFactory factory;
    {
      std::lock_guard lock(mutex_);
      for (const auto& [native_name, native_factory] : natives_) {
        if (native_name == name) {
          factory = native_factory;
          break;
        }
      }
    }
    if (!factory) {
      JS_ThrowReferenceError(context, "unknown native module '%s'", name.c_str());
      return nullptr;
    }
    return factory(context);
  }

  if (!confined(name)) {
    JS_ThrowReferenceError(context, "module path is outside the file root: %s", name.c_str());
    return nullptr;
  }
  std::ifstream input(name, std::ios::binary);
  if (!input) {
    JS_ThrowReferenceError(context, "could not open module '%s'", name.c_str());
    return nullptr;
  }
  const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  JSValue value = JS_Eval(context, source.c_str(), source.size(), name.c_str(),
                          JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
  if (JS_IsException(value)) return nullptr;
  {
    std::lock_guard lock(mutex_);
    if (std::find(loaded_files_.begin(), loaded_files_.end(), name) == loaded_files_.end()) {
      loaded_files_.push_back(name);
    }
  }
  // NOTE: JS_VALUE_GET_PTR is the only low-level accessor used here; it is
  // tied to the pinned quickjs-ng version - do not introduce newer JS_* APIs
  // without bumping/locking that dependency.
  auto* module = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(value));
  JS_FreeValue(context, value);
  return module;
}

std::vector<std::string> ModuleRegistry::native_modules() const {
  std::lock_guard lock(mutex_);
  std::vector<std::string> names;
  names.reserve(natives_.size());
  for (const auto& [name, factory] : natives_) names.push_back(name);
  return names;
}

std::vector<std::string> ModuleRegistry::loaded_files() const {
  std::lock_guard lock(mutex_);
  return loaded_files_;
}

std::string ModuleRegistry::file_root() const {
  std::lock_guard lock(mutex_);
  return file_root_;
}

}  // namespace rime::js
