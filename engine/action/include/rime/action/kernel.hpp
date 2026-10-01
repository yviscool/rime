#pragma once

#include "rime/action/action.hpp"
#include "rime/core/trace.hpp"

#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace rime::action {

class CapabilityPolicy {
 public:
  virtual ~CapabilityPolicy() = default;
  [[nodiscard]] virtual bool allows(const std::string& capability) const = 0;
};

class StaticCapabilityPolicy final : public CapabilityPolicy {
 public:
  explicit StaticCapabilityPolicy(std::unordered_set<std::string> granted)
      : granted_(std::move(granted)) {}
  [[nodiscard]] bool allows(const std::string& capability) const override {
    return granted_.contains(capability);
  }

 private:
  std::unordered_set<std::string> granted_;
};

class Kernel final {
 public:
  explicit Kernel(std::shared_ptr<const CapabilityPolicy> policy,
                  std::shared_ptr<rime::core::TraceSink> trace = {});

  rime::core::Error register_executor(std::string action_type,
                                       std::shared_ptr<Executor> executor);
  Result execute(const Action& action, rime::core::CancellationToken cancellation = {});
  // Read-only capability probe so native query paths enforce the same policy
  // as the action pipeline without building an Action.
  [[nodiscard]] bool allows(const std::string& capability) const;

 private:
  Result fail(const Action& action, rime::core::Error::Code code, std::string message);
  void record(const Action& action, rime::core::TraceKind kind, std::string detail);

  std::shared_ptr<const CapabilityPolicy> policy_;
  std::shared_ptr<rime::core::TraceSink> trace_;
  std::mutex mutex_;
  std::unordered_map<std::string, std::shared_ptr<Executor>> executors_;
};

}  // namespace rime::action
