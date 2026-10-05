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

// TODO(pipeline): introduce Context/Intent types and route all input through
// `Context -> Intent -> Action IR -> Action Kernel` per AGENTS.md architecture
// rules. Not added here to avoid a repo-wide type churn; Action/Result stay
// the pipeline IR until the Context/Intent design lands.
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
  // Internal diagnostics field. NOT part of the wire contract: the wall-clock
  // ms (system_clock) at which the Dispatcher accepted this action into its
  // queue, stamped by Dispatcher::submit when it is 0; the Kernel turns the
  // delta into TraceEntry::queue_wait_ms on ActionStarted. A pre-set value is
  // preserved so in-process tests can inject a deterministic accept time.
  // codec.cpp cannot leak it: encode_action() writes an explicit field list
  // (schemaVersion/id/source/type/capability/target/preconditions/
  // deadlineUnixMs/parentActionId/payload/idempotencyKey) and
  // decode_action() rejects unknown keys through has_only_keys, so this
  // member never reaches or leaves JSON.
  std::uint64_t accepted_unix_ms{0};
  // Internal deadline budget. NOT part of the wire contract (same codec
  // argument as accepted_unix_ms above): deadlineUnixMs resolved against the
  // kernel clock's wall domain at first sight - Dispatcher::submit on
  // accept, Kernel::execute for direct paths - and frozen into that clock's
  // monotonic domain (now() - time_since_epoch), so a wall-clock jump (NTP
  // step, manual set_unix_ms) can neither shorten nor extend the budget
  // while the action waits in the queue or runs. Per-process only: never
  // serialized, so cross-process comparability stays with the wall value.
  // 0 means unresolved (checked in the wall domain as a fallback).
  std::int64_t deadline_mono_ms{0};
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
