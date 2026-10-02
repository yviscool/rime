// Bun all-in-one: contract schema smoke via `bun tools/schema-smoke.ts`.
// Validates contracts/schema examples + rejected fixtures against the same
// JSON Schema Draft 2020-12 subset implemented by engine/action/src/codec.cpp.
import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { resolve } from "node:path";

type JsonValue = null | boolean | number | string | JsonValue[] | { [key: string]: JsonValue };

interface Schema {
  $ref?: string;
  $defs?: Record<string, Schema>;
  $schema?: string;
  const?: JsonValue;
  enum?: JsonValue[];
  type?: string | string[];
  required?: string[];
  properties?: Record<string, Schema>;
  additionalProperties?: boolean | Schema;
  items?: Schema;
  minLength?: number;
  minimum?: number;
  maximum?: number;
}

const root = resolve(import.meta.dir, "..");
const readJson = async (path: string): Promise<JsonValue> =>
  JSON.parse(await readFile(resolve(root, path), "utf8")) as JsonValue;

// Minimal JSON Schema Draft 2020-12 subset validator covering the features
// used by contracts/schema: type, const, enum, required, properties,
// additionalProperties, items, $ref, minLength, minimum and maximum.
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

  if (value !== null && typeof value === "object" && !Array.isArray(value) && schema.properties) {
    const record = value as Record<string, JsonValue>;
    const required = schema.required ?? [];
    for (const key of required) {
      check(Object.hasOwn(record, key), `missing required property '${key}'`);
    }
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

const actionSchema = (await readJson("contracts/schema/action-v1.schema.json")) as unknown as Schema;
const resultSchema = (await readJson("contracts/schema/result-v1.schema.json")) as unknown as Schema;
const windowSchema = (await readJson("contracts/schema/window-v1.schema.json")) as unknown as Schema;
const action = (await readJson("contracts/schema/examples/action-v1.json")) as unknown as Record<string, JsonValue>;
const result = (await readJson("contracts/schema/examples/result-v1.json")) as unknown as Record<string, JsonValue>;
const windowQuery = (await readJson("contracts/schema/examples/window-v1.json")) as unknown as Record<string, JsonValue>;
const windowSnapshot = (await readJson("contracts/schema/examples/window-v1-snapshot.json")) as unknown as JsonValue;
const windowMovePayload = (await readJson("contracts/schema/examples/window-v1-move-payload.json")) as unknown as JsonValue;

assert.equal(actionSchema.$schema, "https://json-schema.org/draft/2020-12/schema");
assert.equal(resultSchema.$schema, actionSchema.$schema);
assert.equal(windowSchema.$schema, actionSchema.$schema);
assert.deepEqual(actionSchema.required, [
  "schemaVersion", "id", "source", "type", "capability", "target", "preconditions", "deadlineUnixMs", "payload",
]);

const actionProblems = validate(actionSchema, action as JsonValue, actionSchema);
assert.deepEqual(actionProblems, [], `action example violates schema:\n${actionProblems.join("\n")}`);
const resultProblems = validate(resultSchema, result as JsonValue, resultSchema);
assert.deepEqual(resultProblems, [], `result example violates schema:\n${resultProblems.join("\n")}`);

const queryProblems = validate(windowSchema, windowQuery as JsonValue, windowSchema);
assert.deepEqual(queryProblems, [], `window query example violates schema:\n${queryProblems.join("\n")}`);
const waitQuery = { ...windowQuery, until: "closed" } as Record<string, JsonValue>;
const waitProblems = validate(windowSchema, waitQuery as JsonValue, windowSchema);
assert.deepEqual(waitProblems, [], `window wait query violates schema:\n${waitProblems.join("\n")}`);
const snapshotProblems = validate(windowSchema.$defs?.["snapshot"] as Schema, windowSnapshot, windowSchema);
assert.deepEqual(snapshotProblems, [], `window snapshot example violates schema:\n${snapshotProblems.join("\n")}`);
const movePayloadProblems = validate(windowSchema.$defs?.["movePayload"] as Schema, windowMovePayload, windowSchema);
assert.deepEqual(movePayloadProblems, [], `window move payload violates schema:\n${movePayloadProblems.join("\n")}`);
const groupAddPayload = { title: "Rime Group", matchMode: "startswith", includeHidden: false } as JsonValue;
const groupAddProblems = validate(windowSchema.$defs?.["groupAddPayload"] as Schema, groupAddPayload, windowSchema);
assert.deepEqual(groupAddProblems, [], `window group add payload violates schema:\n${groupAddProblems.join("\n")}`);
const groupFocusPayload = { reverse: true } as JsonValue;
const groupFocusProblems = validate(
  windowSchema.$defs?.["groupFocusPayload"] as Schema,
  groupFocusPayload,
  windowSchema,
);
assert.deepEqual(groupFocusProblems, [], `window group focus payload violates schema:\n${groupFocusProblems.join("\n")}`);
const groupClosePayload = { mode: "all" } as JsonValue;
const groupCloseProblems = validate(
  windowSchema.$defs?.["groupClosePayload"] as Schema,
  groupClosePayload,
  windowSchema,
);
assert.deepEqual(groupCloseProblems, [], `window group close payload violates schema:\n${groupCloseProblems.join("\n")}`);

// A failed result may carry the timeout code produced by deadline enforcement.
const timeoutResult: Record<string, JsonValue> = {
  ...result,
  status: "failed",
  error: { code: "timeout", message: "action deadline exceeded", retryable: true },
};
const timeoutProblems = validate(resultSchema, timeoutResult as JsonValue, resultSchema);
assert.deepEqual(timeoutProblems, [], `timeout result violates schema:\n${timeoutProblems.join("\n")}`);

assert.equal(action["schemaVersion"], 1);
assert.equal(action["id"], 42);
assert.equal((action["source"] as Record<string, JsonValue>)["kind"], "user");
assert.equal((action["target"] as Record<string, JsonValue>)["kind"], "window");
assert.equal((action["payload"] as Record<string, JsonValue>)["position"], "left");
assert.equal(result["schemaVersion"], 1);
assert.equal(result["actionId"], action["id"]);
assert.equal(result["status"], "succeeded");
assert.equal(result["error"], null);

// Rejected fixtures prove the validator actually rejects violations.
const violations: Array<[JsonValue, Schema, Schema?]> = [
  [{ ...action, extra: true }, actionSchema],
  [{ ...action, id: 9007199254740993 }, actionSchema],
  [{ ...action, payload: [] }, actionSchema],
  [{ ...result, status: "nope" }, resultSchema],
  [{ ...result, unexpected: 1 }, resultSchema],
  [{ ...result, status: "failed", error: { code: "deadlined", message: "x", retryable: false } }, resultSchema],
  [{ ...windowQuery, unexpected: true }, windowSchema],
  [{ ...windowQuery, matchMode: "fuzzy" }, windowSchema],
  [{ ...windowQuery, ahkId: "" }, windowSchema],
  [{ ...waitQuery, until: "whenever" }, windowSchema],
  [{ ...(windowSnapshot as Record<string, JsonValue>), state: "weird" }, windowSchema.$defs?.["snapshot"] as Schema, windowSchema],
  [{ ...(windowSnapshot as Record<string, JsonValue>), hwnd: 42 }, windowSchema.$defs?.["snapshot"] as Schema, windowSchema],
  [{ ...(windowMovePayload as Record<string, JsonValue>), position: "diagonal" }, windowSchema.$defs?.["movePayload"] as Schema, windowSchema],
  [{ ...(groupAddPayload as Record<string, JsonValue>), unexpected: 1 }, windowSchema.$defs?.["groupAddPayload"] as Schema, windowSchema],
  [{ ...(groupAddPayload as Record<string, JsonValue>), matchMode: "fuzzy" }, windowSchema.$defs?.["groupAddPayload"] as Schema, windowSchema],
  [{ ...(groupAddPayload as Record<string, JsonValue>), ahkId: 0 }, windowSchema.$defs?.["groupAddPayload"] as Schema, windowSchema],
  [{ ...(groupFocusPayload as Record<string, JsonValue>), reverse: "yes" }, windowSchema.$defs?.["groupFocusPayload"] as Schema, windowSchema],
  [{ ...(groupClosePayload as Record<string, JsonValue>), mode: "bogus" }, windowSchema.$defs?.["groupClosePayload"] as Schema, windowSchema],
  [{ mode: "all", extra: true }, windowSchema.$defs?.["groupClosePayload"] as Schema, windowSchema],
];
for (const [value, schema, schemaRoot] of violations) {
  const problems = validate(schema, value, schemaRoot ?? schema);
  assert.ok(problems.length > 0, "expected schema violation to be reported");
}

console.log("Action, result and window schema validation passed (examples + violations)");
