// Bun all-in-one: action contract golden smoke via `bun tools/golden-smoke.ts`.
// Consumes contracts/golden/*.json (four layers per action: shape, semantic,
// lifecycle, failure + envelope) and cross-checks every file against
// contracts/registry/actions.json.
//
// The Draft 2020-12 subset validator below is the same subset as
// tools/schema-smoke.ts (type, const, enum, oneOf, anyOf, required,
// properties, additionalProperties, items, $ref -> $defs, minLength, minimum,
// maximum, with violations reported as a list). It is copied rather than
// imported because tools/schema-smoke.ts is read-only for this task; keep the
// two implementations in sync - tests/native/golden_tests.cpp mirrors the
// same subset a third time.
// It finishes by running tools/golden-fixture.ts --check (see the bottom of
// this file), so the generated QuickJS consumer of the failure layer cannot
// drift from the golden files validated here.
import assert from "node:assert/strict";
import { readFile, readdir } from "node:fs/promises";
import { resolve } from "node:path";

import { check_drift } from "./golden-fixture";

type JsonValue = null | boolean | number | string | JsonValue[] | { [key: string]: JsonValue };

interface Schema {
  $ref?: string;
  $defs?: Record<string, Schema>;
  $schema?: string;
  const?: JsonValue;
  enum?: JsonValue[];
  type?: string | string[];
  oneOf?: Schema[];
  anyOf?: Schema[];
  required?: string[];
  properties?: Record<string, Schema>;
  additionalProperties?: boolean | Schema;
  items?: Schema;
  minLength?: number;
  minimum?: number;
  maximum?: number;
}

interface GoldenShape {
  valid: Array<{ name: string; payload: JsonValue }>;
  violations: Array<{ name: string; payload: JsonValue; code: string }>;
}

interface GoldenLifecycle {
  name: string;
  deadlineMode: string;
  action: Record<string, JsonValue>;
  expect: { code: string };
}

interface GoldenFailure {
  name: string;
  payload: JsonValue;
  code: string;
  messageContains?: string;
  target?: { kind?: JsonValue; id?: JsonValue };
}

interface GoldenEnvelope {
  name: string;
  action: Record<string, JsonValue>;
  expect: { code: string };
}

interface GoldenFile {
  type: string;
  capability: string;
  payloadSchema: string | null;
  shape: GoldenShape;
  semantic: Array<{ name: string; payload: JsonValue; check: { kind: string; expect: JsonValue } }>;
  lifecycle: GoldenLifecycle[];
  failure: GoldenFailure[];
  envelope: GoldenEnvelope[];
}

const root = resolve(import.meta.dir, "..");
const readJson = async (path: string): Promise<JsonValue> =>
  JSON.parse(await readFile(resolve(root, path), "utf8")) as JsonValue;

// Minimal JSON Schema Draft 2020-12 subset validator - see the file header.
function validate(schema: Schema, value: JsonValue, rootSchema: Schema, path = "$"): string[] {
  const problems: string[] = [];
  const check = (condition: boolean, message: string): void => {
    if (!condition) problems.push(`${path}: ${message}`);
  };

  if (schema.$ref) {
    const key = schema.$ref.replace("#/$defs/", "");
    const target = rootSchema.$defs?.[key];
    assert.ok(target, `unresolved $ref ${schema.$ref}`);
    problems.push(...validate(target, value, rootSchema, path));
    return problems;
  }
  if (schema.const !== undefined) {
    check(JSON.stringify(value) === JSON.stringify(schema.const), `must equal ${JSON.stringify(schema.const)}`);
    return problems;
  }
  if (schema.enum) {
    check(
      schema.enum.some((entry) => JSON.stringify(entry) === JSON.stringify(value)),
      `must be one of ${JSON.stringify(schema.enum)}`,
    );
    return problems;
  }
  if (schema.oneOf) {
    const matches = schema.oneOf.filter((branch) => validate(branch, value, rootSchema, path).length === 0);
    check(matches.length === 1, "must match exactly one oneOf branch");
  }
  if (schema.anyOf) {
    const matches = schema.anyOf.filter((branch) => validate(branch, value, rootSchema, path).length === 0);
    check(matches.length >= 1, "must match at least one anyOf branch");
  }

  const types = Array.isArray(schema.type) ? schema.type : schema.type ? [schema.type] : [];
  if (types.length > 0) {
    const matches = types.some((type) => matchesType(type, value));
    check(matches, `must match type ${types.join("|")}`);
    if (!matches) return problems;
  }

  if (typeof value === "number") {
    if (schema.minimum !== undefined) check(value >= schema.minimum, `must be >= ${schema.minimum}`);
    if (schema.maximum !== undefined) check(value <= schema.maximum, `must be <= ${schema.maximum}`);
    if (types.includes("integer")) check(Number.isInteger(value), "must be an integer");
  }
  if (typeof value === "string" && schema.minLength !== undefined) {
    check(value.length >= schema.minLength, `must have minLength ${schema.minLength}`);
  }

  if (Array.isArray(value) && schema.items) {
    value.forEach((element, index) => {
      problems.push(...validate(schema.items as Schema, element, rootSchema, `${path}[${index}]`));
    });
  }

  if (value !== null && typeof value === "object" && !Array.isArray(value)) {
    const record = value as Record<string, JsonValue>;
    for (const key of schema.required ?? []) {
      check(Object.hasOwn(record, key), `missing required property '${key}'`);
    }
    if (schema.properties) {
      if (schema.additionalProperties === false) {
        for (const key of Object.keys(record)) {
          check(Object.hasOwn(schema.properties, key), `unexpected property '${key}'`);
        }
      }
      for (const [key, entry] of Object.entries(schema.properties)) {
        if (Object.hasOwn(record, key)) {
          problems.push(...validate(entry, record[key], rootSchema, `${path}.${key}`));
        }
      }
    }
  }
  return problems;
}

function matchesType(type: string, value: JsonValue): boolean {
  switch (type) {
    case "object":
      return value !== null && typeof value === "object" && !Array.isArray(value);
    case "array":
      return Array.isArray(value);
    case "string":
      return typeof value === "string";
    case "integer":
      return typeof value === "number" && Number.isInteger(value);
    case "number":
      return typeof value === "number" && Number.isFinite(value);
    case "boolean":
      return typeof value === "boolean";
    case "null":
      return value === null;
    default:
      return false;
  }
}

const ERROR_CODES = new Set([
  "none",
  "invalid_state",
  "queue_closed",
  "queue_full",
  "cancelled",
  "timeout",
  "capability_denied",
  "invalid_contract",
  "unsupported",
  "execution_failed",
  "target_gone",
]);
const DEADLINE_MODES = new Set(["past", "future"]);
const PLACEMENTS = ["work-left-half", "work-right-half", "work-full", "work-top", "work-bottom"];
const SEMANTIC_KINDS: Record<string, (expect: JsonValue) => boolean> = {
  "window.rect": (expect) => typeof expect === "string" && PLACEMENTS.includes(expect),
  "process.exists": (expect) => expect === true,
  "clipboard.text": (expect) => typeof expect === "string",
};
const GOLDEN_KEYS = [
  "type",
  "capability",
  "payloadSchema",
  "shape",
  "semantic",
  "lifecycle",
  "failure",
  "envelope",
];
const EXPECTED_FILES = [
  "automation.invoke.json",
  "clipboard.write.json",
  "input.mouse.json",
  "input.send.json",
  "process.launch.json",
  "process.terminate.json",
  "window.close.json",
  "window.focus.json",
  "window.group.add.json",
  "window.kill.json",
  "window.move.json",
  "window.set.region.json",
  "window.set.title.json",
  "window.set.transparent.json",
  "window.zorder.json",
];

const is_object = (value: JsonValue): value is { [key: string]: JsonValue } =>
  value !== null && typeof value === "object" && !Array.isArray(value);
const is_non_empty_string = (value: JsonValue | undefined): value is string =>
  typeof value === "string" && value.length > 0;

const registry = (await readJson("contracts/registry/actions.json")) as unknown as {
  actions: Array<{ type: string; capability: string; payloadSchema: string }>;
};
const registry_by_type = new Map<string, { capability: string; payloadSchema: string }>(
  registry.actions.map((entry) => [entry.type, { capability: entry.capability, payloadSchema: entry.payloadSchema }]),
);

const actionSchema = (await readJson("contracts/schema/action-v1.schema.json")) as unknown as Schema;
const schema_roots = new Map<string, Schema>();
async function load_schema(path: string): Promise<Schema> {
  const cached = schema_roots.get(path);
  if (cached) return cached;
  const parsed = (await readJson(path)) as unknown as Schema;
  schema_roots.set(path, parsed);
  return parsed;
}
async function payload_validator(reference: string): Promise<(value: JsonValue) => string[]> {
  const separator = reference.indexOf("#");
  assert.ok(separator > 0, `payloadSchema must look like <file>#/$defs/<name>: ${reference}`);
  const schema_path = reference.slice(0, separator);
  const fragment = reference.slice(separator + 1);
  assert.ok(fragment.startsWith("/$defs/"), `payloadSchema fragment must be a $defs pointer: ${reference}`);
  const key = fragment.slice("/$defs/".length);
  const schema_root = await load_schema(schema_path);
  const definition = schema_root.$defs?.[key];
  assert.ok(definition, `unresolved $defs/${key} in ${schema_path}`);
  return (value) => validate(definition, value, schema_root);
}

const golden_files = (await readdir(resolve(root, "contracts/golden")))
  .filter((name) => name.endsWith(".json"))
  .sort();
assert.deepEqual(golden_files, EXPECTED_FILES, "contracts/golden must hold exactly the 15 golden files");

let valid_checked = 0;
let violations_checked = 0;
let lifecycle_checked = 0;
let semantic_checked = 0;
let failure_checked = 0;
let envelope_checked = 0;

for (const file of golden_files) {
  const label = `contracts/golden/${file}`;
  const golden = (await readJson(`contracts/golden/${file}`)) as unknown as GoldenFile;
  assert.ok(is_object(golden as unknown as JsonValue), `${label}: golden file must be a JSON object`);

  // a. file name, registry capability and payloadSchema must agree.
  assert.deepEqual(
    Object.keys(golden).sort(),
    [...GOLDEN_KEYS].sort(),
    `${label}: unexpected top-level fields`,
  );
  assert.equal(`${golden.type}.json`, file, `${label}: type must match the file name`);
  const registered = registry_by_type.get(golden.type);
  assert.ok(registered, `${label}: type is missing from contracts/registry/actions.json`);
  assert.equal(golden.capability, registered.capability, `${label}: capability disagrees with the registry`);
  const registered_schema = registered.payloadSchema === "inline" ? null : registered.payloadSchema;
  assert.equal(golden.payloadSchema, registered_schema, `${label}: payloadSchema disagrees with the registry`);
  assert.ok(is_non_empty_string(golden.type), `${label}: type must be a non-empty string`);
  assert.ok(is_non_empty_string(golden.capability), `${label}: capability must be a non-empty string`);
  const check_payload = golden.payloadSchema === null ? null : await payload_validator(golden.payloadSchema);

  // b/c. shape: schema-layer validity (or the declared executor validator).
  assert.ok(is_object(golden.shape as unknown as JsonValue), `${label}: shape must be an object`);
  assert.ok(Array.isArray(golden.shape.valid), `${label}: shape.valid must be an array`);
  assert.ok(Array.isArray(golden.shape.violations), `${label}: shape.violations must be an array`);
  assert.ok(golden.shape.valid.length > 0, `${label}: shape.valid must not be empty`);
  for (const entry of golden.shape.valid) {
    const where = `${label}: shape.valid[${entry.name}]`;
    assert.ok(is_non_empty_string(entry.name), `${where}: name must be a non-empty string`);
    assert.ok(Object.hasOwn(entry, "payload"), `${where}: payload must be present`);
    assert.ok(entry.payload !== null, `${where}: payload must not be null`);
    if (check_payload) {
      // Every schema'd payload is an object type in contracts/schema.
      assert.ok(is_object(entry.payload), `${where}: payload must be an object`);
      const problems = check_payload(entry.payload);
      assert.deepEqual(problems, [], `${where}: payload must validate:\n${problems.join("\n")}`);
    } else {
      // Inline contract: object payloads, except input.send whose executor
      // takes a JSON array of key steps (input_executor.cpp:42).
      assert.ok(
        is_object(entry.payload) || Array.isArray(entry.payload),
        `${where}: payload must be an object or an inline step array`,
      );
    }
    valid_checked += 1;
  }
  for (const entry of golden.shape.violations) {
    const where = `${label}: shape.violations[${entry.name}]`;
    assert.ok(is_non_empty_string(entry.name), `${where}: name must be a non-empty string`);
    assert.ok(ERROR_CODES.has(entry.code), `${where}: code must be a contract error code`);
    assert.ok(entry.payload !== null && entry.payload !== undefined, `${where}: payload must be present`);
    if (check_payload) {
      const problems = check_payload(entry.payload);
      assert.ok(problems.length > 0, `${where}: payload must violate its schema`);
      assert.equal(entry.code, "invalid_contract", `${where}: schema violations report invalid_contract`);
    }
    violations_checked += 1;
  }

  // d. lifecycle: envelope-valid actions with pre-executor expectations.
  assert.ok(Array.isArray(golden.lifecycle), `${label}: lifecycle must be an array`);
  assert.ok(golden.lifecycle.length >= 2, `${label}: lifecycle needs at least two cases`);
  let expired_deadline = false;
  let capability_mismatch = false;
  let precondition_case = false;
  for (const entry of golden.lifecycle) {
    const where = `${label}: lifecycle[${entry.name}]`;
    assert.ok(is_non_empty_string(entry.name), `${where}: name must be a non-empty string`);
    assert.ok(DEADLINE_MODES.has(entry.deadlineMode), `${where}: deadlineMode must be past or future`);
    assert.ok(Object.hasOwn(entry, "expect"), `${where}: expect must be present`);
    assert.ok(ERROR_CODES.has(entry.expect.code), `${where}: expect.code must be a contract code`);
    assert.ok(is_object(entry.action as unknown as JsonValue), `${where}: action must be an object`);
    const problems = validate(actionSchema, entry.action as JsonValue, actionSchema);
    assert.deepEqual(problems, [], `${where}: action must satisfy action-v1:\n${problems.join("\n")}`);
    assert.equal(entry.action.type, golden.type, `${where}: action.type must match the golden type`);
    assert.equal(entry.action.capability !== golden.capability, entry.expect.code === "capability_denied",
      `${where}: only capability_mismatch may carry a different action.capability`);
    assert.ok(Array.isArray(entry.action.preconditions), `${where}: action.preconditions must be an array`);
    assert.equal((entry.action.preconditions as JsonValue[]).length > 0, entry.expect.code === "unsupported",
      `${where}: unsupported must come from a declared precondition`);
    assert.equal(entry.deadlineMode === "past", entry.expect.code === "timeout",
      `${where}: timeout must come from a past deadline`);
    if (entry.expect.code === "timeout") expired_deadline = true;
    if (entry.expect.code === "capability_denied") capability_mismatch = true;
    if (entry.expect.code === "unsupported") precondition_case = true;
    lifecycle_checked += 1;
  }
  assert.ok(expired_deadline, `${label}: missing the expired-deadline (timeout) case`);
  assert.ok(capability_mismatch, `${label}: missing the capability-mismatch case`);
  assert.ok(precondition_case, `${label}: missing the precondition (unsupported) case`);

  // semantic: round-1 kind whitelist, no runtime consumer until R4-B.
  assert.ok(Array.isArray(golden.semantic), `${label}: semantic must be an array`);
  for (const entry of golden.semantic) {
    const where = `${label}: semantic[${entry.name}]`;
    assert.ok(is_non_empty_string(entry.name), `${where}: name must be a non-empty string`);
    assert.ok(is_object(entry.payload), `${where}: payload must be an object`);
    assert.ok(Object.hasOwn(entry, "check"), `${where}: check must be present`);
    assert.ok(is_object(entry.check as unknown as JsonValue), `${where}: check must be an object`);
    const kind_check = SEMANTIC_KINDS[entry.check.kind];
    assert.ok(kind_check, `${where}: check.kind must be one of ${Object.keys(SEMANTIC_KINDS).join(", ")}`);
    assert.ok(kind_check(entry.check.expect), `${where}: check.expect does not fit ${entry.check.kind}`);
    semantic_checked += 1;
  }

  // failure: executor-layer rejections; the QuickJS mapping of every entry
  // lives in tools/golden-fixture.ts and is drift-checked at the end.
  assert.ok(Array.isArray(golden.failure), `${label}: failure must be an array`);
  for (const entry of golden.failure) {
    const where = `${label}: failure[${entry.name}]`;
    assert.ok(is_non_empty_string(entry.name), `${where}: name must be a non-empty string`);
    assert.ok(is_object(entry.payload), `${where}: payload must be an object`);
    assert.ok(ERROR_CODES.has(entry.code), `${where}: code must be a contract error code`);
    if (entry.messageContains !== undefined) {
      assert.ok(is_non_empty_string(entry.messageContains), `${where}: messageContains must be a non-empty string`);
    }
    if (entry.target !== undefined) {
      assert.ok(is_object(entry.target as unknown as JsonValue), `${where}: target must be an object`);
      assert.ok(is_non_empty_string(entry.target.kind), `${where}: target.kind must be a non-empty string`);
      assert.ok(is_non_empty_string(entry.target.id), `${where}: target.id must be a non-empty string`);
    }
    failure_checked += 1;
  }

  // envelope: ActionV1 samples the schema must reject.
  assert.ok(Array.isArray(golden.envelope), `${label}: envelope must be an array`);
  assert.ok(golden.envelope.length > 0, `${label}: envelope must hold at least one rejected sample`);
  for (const entry of golden.envelope) {
    const where = `${label}: envelope[${entry.name}]`;
    assert.ok(is_non_empty_string(entry.name), `${where}: name must be a non-empty string`);
    assert.equal(entry.expect.code, "invalid_contract", `${where}: envelope samples fail with invalid_contract`);
    const problems = validate(actionSchema, entry.action as JsonValue, actionSchema);
    assert.ok(problems.length > 0, `${where}: action-v1 schema must reject this sample`);
    envelope_checked += 1;
  }
}

console.log(
  `Golden contract smoke passed (${golden_files.length} files, ${valid_checked} valid, ` +
    `${violations_checked} violations, ${lifecycle_checked} lifecycle, ${semantic_checked} semantic, ` +
    `${failure_checked} failure, ${envelope_checked} envelope)`,
);

// The QuickJS consumer of the failure layer is generated from the very files
// validated above, so it is regenerated and re-checked here: a golden edit
// that is not reflected in tests/js/fixtures/golden-violations.generated.mjs
// fails contract:smoke with the regenerate hint instead of shipping a stale
// expressibility audit.
const fixture = await check_drift();
console.log(
  `Golden fixture in sync with QuickJS consumer (${fixture.executed} executed, ${fixture.skipped} skipped)`,
);
