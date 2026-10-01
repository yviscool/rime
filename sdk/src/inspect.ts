/**
 * Pure parser for the host inspection report returned by
 * `runtime.inspect()` — the debug protocol view (modules, functions,
 * subscriptions, tasks, errors). Complements `runtime.context()`: context is
 * the stable script contract, inspection is the fuller diagnostics readout.
 * The bridge always requests kind `all`, so every section is required;
 * unknown extra fields are tolerated for forward compatibility.
 */

export interface RuntimeInspection {
  modules: { native: string[]; files: string[]; fileRoot: string };
  functions: string[];
  subscriptions: string[];
  tasks: { async: number; queued: number; timers: number; callbacks: number };
  errors: { where: string; message: string }[];
}

function requireObject(value: unknown, field: string): Record<string, unknown> {
  if (value === null || typeof value !== "object" || Array.isArray(value)) {
    throw new TypeError(`inspection.${field} must be an object`);
  }
  return value as Record<string, unknown>;
}

function requireStringArray(value: unknown, field: string): string[] {
  if (!Array.isArray(value) || value.some((entry) => typeof entry !== "string")) {
    throw new TypeError(`inspection.${field} must be an array of strings`);
  }
  return value as string[];
}

function requireCount(value: unknown, field: string): number {
  if (typeof value !== "number" || !Number.isInteger(value) || value < 0) {
    throw new TypeError(`inspection.${field} must be a non-negative integer`);
  }
  return value;
}

/** Parses and shape-validates `runtime.inspect()` output. Throws a field-named TypeError. */
export function parseInspection(json: string): RuntimeInspection {
  let parsed: unknown;
  try {
    parsed = JSON.parse(json);
  } catch (error) {
    throw new TypeError(`inspection output is not JSON: ${(error as Error).message}`);
  }
  if (parsed === null || typeof parsed !== "object" || Array.isArray(parsed)) {
    throw new TypeError("inspection output must be an object");
  }
  const root = parsed as Record<string, unknown>;
  const modules = requireObject(root.modules, "modules");
  const tasks = requireObject(root.tasks, "tasks");
  if (!Array.isArray(root.errors)) {
    throw new TypeError("inspection.errors must be an array of { where, message } objects");
  }
  const errors = root.errors.map((entry, index) => {
    const item = requireObject(entry, `errors[${index}]`);
    if (typeof item.where !== "string" || typeof item.message !== "string") {
      throw new TypeError(`inspection.errors[${index}] must have string where and message`);
    }
    return { where: item.where, message: item.message };
  });
  const fileRoot = modules.fileRoot;
  if (typeof fileRoot !== "string") {
    throw new TypeError("inspection.modules.fileRoot must be a string");
  }
  return {
    modules: {
      native: requireStringArray(modules.native, "modules.native"),
      files: requireStringArray(modules.files, "modules.files"),
      fileRoot,
    },
    functions: requireStringArray(root.functions, "functions"),
    subscriptions: requireStringArray(root.subscriptions, "subscriptions"),
    tasks: {
      async: requireCount(tasks.async, "tasks.async"),
      queued: requireCount(tasks.queued, "tasks.queued"),
      timers: requireCount(tasks.timers, "tasks.timers"),
      callbacks: requireCount(tasks.callbacks, "tasks.callbacks"),
    },
    errors,
  };
}
