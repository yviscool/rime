import { readFile, writeFile } from "node:fs/promises";
import { resolve } from "node:path";

interface Schema {
  $schema?: string;
  $id?: string;
  title?: string;
  type?: string | string[];
  required?: string[];
  properties?: Record<string, Schema>;
  $defs?: Record<string, Schema>;
  items?: Schema;
  const?: unknown;
  enum?: unknown[];
  $ref?: string;
  additionalProperties?: unknown;
}

const root = resolve(import.meta.dir, "..");
const schema_files = [
  { file: "contracts/schema/action-v1.schema.json", name: "ActionV1" },
  { file: "contracts/schema/result-v1.schema.json", name: "ActionResultV1" },
  { file: "contracts/schema/window-v1.schema.json", name: "WindowV1" },
] as const;
const generated_path = resolve(root, "contracts/generated/contracts.ts");

function pascal_case(value: string): string {
  return value
    .split(/[^A-Za-z0-9]+/)
    .filter(Boolean)
    .map((part) => part.charAt(0).toUpperCase() + part.slice(1))
    .join("");
}

function strip_rime_prefix(title: string, fallback: string): string {
  const name = title.replace(/^Rime\s+/, "");
  return pascal_case(name || fallback);
}

function is_identifier(key: string): boolean {
  return /^[A-Za-z_$][A-Za-z0-9_$]*$/.test(key);
}

function render_literal(value: unknown): string {
  if (typeof value === "string") return JSON.stringify(value);
  if (value === null) return "null";
  return String(value);
}

function render_union_enum(values: unknown[]): string {
  return values.map(render_literal).join(" | ");
}

function render_type(
  node: Schema,
  context: { defs: Map<string, string>; owner: string },
): string {
  if (node.$ref) {
    const key = node.$ref.replace("#/$defs/", "");
    const mapped = context.defs.get(key);
    if (!mapped) throw new Error(`unresolved $ref: ${node.$ref}`);
    return mapped;
  }
  if (node.const !== undefined) return render_literal(node.const);
  if (node.enum) return render_union_enum(node.enum);

  const types = Array.isArray(node.type) ? node.type : node.type ? [node.type] : [];
  if (types.length > 1) {
    return types
      .map((single) => render_type({ ...node, type: single }, context))
      .join(" | ");
  }
  const single = types[0];
  switch (single) {
    case "string":
      return "string";
    case "integer":
    case "number":
      return "number";
    case "boolean":
      return "boolean";
    case "null":
      return "null";
    case "array":
      return node.items ? `${render_type(node.items, context)}[]` : "unknown[]";
    case "object": {
      if (!node.properties) return "Record<string, unknown>";
      const required = new Set(node.required ?? []);
      const members = Object.entries(node.properties).map(([key, value]) => {
        const optional = required.has(key) ? "" : "?";
        const name = is_identifier(key) ? key : JSON.stringify(key);
        return `  ${name}${optional}: ${render_type(value, context)};`;
      });
      return `{\n${members.join("\n")}\n}`;
    }
    default:
      return "unknown";
  }
}

interface RenderedSchema {
  name: string;
  source: string;
  declarations: string;
  required: string[];
}

async function load_schema(file: string, name: string): Promise<RenderedSchema> {
  const schema = JSON.parse(await readFile(resolve(root, file), "utf8")) as Schema;
  if (schema.$schema !== "https://json-schema.org/draft/2020-12/schema") {
    throw new Error(`${file}: unsupported $schema`);
  }
  const defs = new Map<string, string>();
  for (const key of Object.keys(schema.$defs ?? {})) {
    defs.set(key, `${name}${pascal_case(key)}`);
  }
  const context = { defs, owner: name };

  const sections: string[] = [];
  for (const [key, node] of Object.entries(schema.$defs ?? {})) {
    const def_name = defs.get(key)!;
    if (node.type === "object" && node.properties) {
      sections.push(render_interface(def_name, node, context));
    } else {
      sections.push(`export type ${def_name} = ${render_type(node, context)};`);
    }
  }
  if (!schema.properties) throw new Error(`${file}: top-level object schema expected`);
  sections.push(render_interface(name, schema, context));

  return {
    name,
    source: file,
    declarations: sections.join("\n\n"),
    required: schema.required ?? [],
  };
}

function render_interface(name: string, node: Schema, context: { defs: Map<string, string>; owner: string }): string {
  const required = new Set(node.required ?? []);
  const members = Object.entries(node.properties ?? {}).map(([key, value]) => {
    const optional = required.has(key) ? "" : "?";
    const member_name = is_identifier(key) ? key : JSON.stringify(key);
    return `  ${member_name}${optional}: ${render_type(value, context)};`;
  });
  return `export interface ${name} {\n${members.join("\n")}\n}`;
}

const rendered: RenderedSchema[] = [];
for (const entry of schema_files) {
  rendered.push(await load_schema(entry.file, entry.name));
}

const required_action_fields = [
  "schemaVersion", "id", "source", "type", "capability", "target",
  "preconditions", "deadlineUnixMs", "payload",
];
for (const field of required_action_fields) {
  if (!rendered[0].required.includes(field)) {
    throw new Error(`action-v1 schema missing required field: ${field}`);
  }
}

const generated = [
  "// Generated by tools/contract.ts from contracts/schema. Do not edit by hand.",
  `// Sources: ${schema_files.map((entry) => entry.file).join(", ")}`,
  "",
  ...rendered.map((entry) => `// --- ${entry.source} ---\n\n${entry.declarations}`),
  "",
].join("\n");

const write = process.argv.includes("--write");
if (write) {
  await writeFile(generated_path, generated, "utf8");
  console.log(`Wrote ${generated_path}`);
} else {
  const existing = await readFile(generated_path, "utf8").catch(() => null);
  if (existing !== generated) {
    throw new Error("contracts/generated/contracts.ts is stale; run: bun run contract:gen");
  }
  const sdk_source = await readFile(resolve(root, "sdk/src/contracts.ts"), "utf8");
  if (!sdk_source.includes("generated/contracts")) {
    throw new Error("sdk/src/contracts.ts must re-export contracts/generated/contracts.ts");
  }
  console.log(`Generated contract declarations are up to date (${rendered.length} schemas)`);
}
