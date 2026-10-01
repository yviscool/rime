import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";

const readJson = async (path) => JSON.parse(await readFile(path, "utf8"));

// Minimal JSON Schema Draft 2020-12 subset validator covering the features
// used by contracts/schema: type, const, enum, required, properties,
// additionalProperties, items, $ref, minLength, minimum and maximum.
function validate(schema, value, root, path = "$") {
  const problems = [];
  const check = (condition, message) => {
    if (!condition) problems.push(`${path}: ${message}`);
  };

  if (schema.$ref) {
    const key = schema.$ref.replace("#/$defs/", "");
    const target = root.$defs?.[key];
    assert.ok(target, `unresolved $ref ${schema.$ref}`);
    problems.push(...validate(target, value, root, path));
    return problems;
  }
  if (schema.const !== undefined) {
    check(JSON.stringify(value) === JSON.stringify(schema.const), `must equal ${JSON.stringify(schema.const)}`);
    return problems;
  }
  if (schema.enum) {
    check(schema.enum.some((entry) => JSON.stringify(entry) === JSON.stringify(value)), `must be one of ${JSON.stringify(schema.enum)}`);
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
      problems.push(...validate(schema.items, element, root, `${path}[${index}]`));
    });
  }

  if (value !== null && typeof value === "object" && !Array.isArray(value) && schema.properties) {
    const required = schema.required ?? [];
    for (const key of required) {
      check(Object.hasOwn(value, key), `missing required property '${key}'`);
    }
    if (schema.additionalProperties === false) {
      for (const key of Object.keys(value)) {
        check(Object.hasOwn(schema.properties, key), `unexpected property '${key}'`);
      }
    }
    for (const [key, entry] of Object.entries(schema.properties)) {
      if (Object.hasOwn(value, key)) {
        problems.push(...validate(entry, value[key], root, `${path}.${key}`));
      }
    }
  }
  return problems;
}

function matchesType(type, value) {
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

const actionSchema = await readJson("contracts/schema/action-v1.schema.json");
const resultSchema = await readJson("contracts/schema/result-v1.schema.json");
const action = await readJson("contracts/schema/examples/action-v1.json");
const result = await readJson("contracts/schema/examples/result-v1.json");

assert.equal(actionSchema.$schema, "https://json-schema.org/draft/2020-12/schema");
assert.equal(resultSchema.$schema, actionSchema.$schema);
assert.deepEqual(actionSchema.required, [
  "schemaVersion", "id", "source", "type", "capability", "target", "preconditions", "deadlineUnixMs", "payload",
]);

const actionProblems = validate(actionSchema, action, actionSchema);
assert.deepEqual(actionProblems, [], `action example violates schema:\n${actionProblems.join("\n")}`);
const resultProblems = validate(resultSchema, result, resultSchema);
assert.deepEqual(resultProblems, [], `result example violates schema:\n${resultProblems.join("\n")}`);

assert.equal(action.schemaVersion, 1);
assert.equal(action.id, 42);
assert.equal(action.source.kind, "user");
assert.equal(action.target.kind, "window");
assert.equal(action.payload.position, "left");
assert.equal(result.schemaVersion, 1);
assert.equal(result.actionId, action.id);
assert.equal(result.status, "succeeded");
assert.equal(result.error, null);

// Rejected fixtures prove the validator actually rejects violations.
const violations = [
  [{ ...action, extra: true }, actionSchema],
  [{ ...action, id: 9007199254740993 }, actionSchema],
  [{ ...action, payload: [] }, actionSchema],
  [{ ...result, status: "nope" }, resultSchema],
  [{ ...result, unexpected: 1 }, resultSchema],
];
for (const [value, schema] of violations) {
  const problems = validate(schema, value, schema);
  assert.ok(problems.length > 0, "expected schema violation to be reported");
}

console.log("Action and result schema validation passed (examples + violations)");
