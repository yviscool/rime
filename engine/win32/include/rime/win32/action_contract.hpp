#pragma once

#include "rime/action/action.hpp"
#include "rime/core/lane.hpp"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

// The entry contract every Action executor opens with.
//
// Six executors used to spell the same five checks out by hand: the lane
// gate, the action type, the target kind, the cancellation check and the
// "payload must be a JSON object" parse. Each copy was free to drift from the
// others, and the strings below are not internal - they are the `detail` a
// trace records, the reason a JS promise rejects and the wording
// docs/api/*.md quotes.
//
// The checks are deliberately *not* batched into one call. Executors
// interleave them on purpose (ProcessExecutor validates the target before it
// knows which action it is, RegistryExecutor cannot check the target id until
// it has read the payload key, WindowExecutor validates by set membership),
// and the order decides which failure a caller sees first. Composing these
// helpers preserves that order; batching them would erase it.
namespace rime::win32::contract {

using Action = rime::action::Action;
using Result = rime::action::Result;
using Code = rime::core::Error::Code;
using CancellationToken = rime::core::CancellationToken;

// A failed Action: `cancelled` marks the cancellation class the kernel
// reports separately from a contract failure.
inline Result fail(const Action& action, const Code code, std::string message) {
  return {action.id, false, false, message, {code, message}, {}};
}

inline Result cancelled(const Action& action, const std::string& message) {
  return {action.id, false, true, message, {Code::Cancelled, message}, {}};
}

// Fall-through wording for a type the executor does not accept.
inline Result unsupported(const Action& action) {
  return fail(action, Code::InvalidContract, "unsupported action type: " + action.type);
}

// Lane gate: the executor may only run on the lane it claims. The message
// comes from `require_lane` and names the lane it wanted and the lane it got.
inline std::optional<Result> lane(const Action& action, const rime::core::Lane expected) {
  const rime::core::Error error = rime::core::require_lane(expected);
  if (error.ok()) return std::nullopt;
  return fail(action, error.code, error.message);
}

// Type gate for an executor that accepts exactly one action type.
inline std::optional<Result> action_type(const Action& action, const char* expected) {
  if (action.type == expected) return std::nullopt;
  return unsupported(action);
}

// Target kind gate. `subject` is the wording in front of "requires": the
// message is contract text and differs per domain ("clipboard.write requires
// target kind 'clipboard'" versus the plural "process actions require ...",
// which WindowExecutor and ProcessExecutor keep as their own `fail` call
// rather than bend this shape for).
inline std::optional<Result> target_kind(const Action& action, const char* subject,
                                         const char* kind) {
  if (action.target.kind == kind) return std::nullopt;
  return fail(action, Code::InvalidContract,
              std::string(subject) + " requires target kind '" + kind +
                  "', got: " + action.target.kind);
}

// Cancellation gates. The two phases have exactly one wording each, and both
// are quoted by contract tests, so they are named rather than passed in.
inline std::optional<Result> cancel_before(const Action& action,
                                           const CancellationToken& cancellation) {
  if (!cancellation.cancelled()) return std::nullopt;
  return cancelled(action, "action was cancelled before execution");
}

inline std::optional<Result> cancel_after(const Action& action,
                                          const CancellationToken& cancellation) {
  if (!cancellation.cancelled()) return std::nullopt;
  return cancelled(action, "action was cancelled after execution");
}

// Payload gate: the executor's payload is a JSON object. `prefix` is
// `"<action type> payload "` for every executor, so the failure reads
// "registry.write payload must be a JSON object". The parse result is taken
// by reference and returned unchanged for the field extraction that follows.
inline std::optional<Result> object_payload(const Action& action,
                                            const rime::core::json::ParseOutcome& payload,
                                            const std::string& prefix) {
  if (payload.ok() && payload.value->is_object()) return std::nullopt;
  return fail(action, Code::InvalidContract, prefix + "must be a JSON object");
}

// JSON numbers are doubles, so "an integer in range" is always the same
// check: finite, equal to its own truncation, and inside the bound. The three
// readers below differ only in where the bound sits.
inline bool in_integral_range(const double number, const double min, const double max) {
  return std::isfinite(number) && std::trunc(number) == number && number >= min && number <= max;
}

// [min, max_exclusive) into a uint64. The upper bound is exclusive because
// 2^63 is exactly representable as a double and 2^63-1 is not: an inclusive
// bound spelled 9223372036854775807.0 rounds up to 2^63 and would admit the
// one value the payload contracts reject. Every uint64 range in the executors
// (registry values, storage handles, storage bytes) is naturally half-open.
inline bool json_u64_range(const rime::core::json::Value& value, const double min,
                           const double max_exclusive, std::uint64_t& out) {
  if (!value.is_number()) return false;
  const double number = value.as_number();
  if (!std::isfinite(number) || std::trunc(number) != number) return false;
  if (!(number >= min) || !(number < max_exclusive)) return false;
  out = static_cast<std::uint64_t>(number);
  return true;
}

// [min, max_inclusive] into an int64. The bounds used here (the Unix
// millisecond ceiling) are far below 2^53, so they survive the double round
// trip exactly and an inclusive comparison is honest.
inline bool json_i64_range(const rime::core::json::Value& value, const double min,
                           const double max_inclusive, std::int64_t& out) {
  if (!value.is_number()) return false;
  const double number = value.as_number();
  if (!in_integral_range(number, min, max_inclusive)) return false;
  out = static_cast<std::int64_t>(number);
  return true;
}

}  // namespace rime::win32::contract
