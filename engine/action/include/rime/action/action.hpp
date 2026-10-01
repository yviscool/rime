#pragma once

#include "rime/core/cancellation.hpp"
#include "rime/core/json.hpp"
#include "rime/core/types.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rime::action {

struct Identity {
  std::string kind;
  std::string id;
};

struct Precondition {
  std::string type;
  std::string expected;
};

struct Action {
  rime::core::ActionId id{0};
  std::uint16_t schema_version{1};
  Identity source;
  std::string type;
  std::string capability;
  Identity target;
  std::vector<Precondition> preconditions;
  std::uint64_t deadline_unix_ms{0};
  rime::core::ActionId parent_action_id{0};
  std::string payload;
  std::string idempotency_key;
};

struct Result {
  rime::core::ActionId id{0};
  bool succeeded{false};
  bool cancelled{false};
  std::string detail;
  rime::core::Error error;
  // Optional JSON object returned by the executor (result-v1 `value`).
  rime::core::json::Value value{};
};

class Executor {
 public:
  virtual ~Executor() = default;
  virtual Result execute(const Action&, rime::core::CancellationToken) = 0;
};

}  // namespace rime::action
