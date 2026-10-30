import { Process, Window, runtime } from "@rime/sdk";

// CLI application for Rime QuickJS host.
// Validates runtime communication, diagnostics, window enumeration, and process enumeration.

function log(...args: unknown[]): void {
  const line = args
    .map((arg) => (typeof arg === "object" ? JSON.stringify(arg) : String(arg)))
    .join(" ");
  if (typeof console !== "undefined" && typeof console.log === "function") {
    console.log(line);
  }
  if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
    try {
      runtime.debug(line);
    } catch {
      // Best-effort debug channel
    }
  }
}

async function main(): Promise<void> {
  // 1. Runtime ping
  const ping = runtime.ping();
  if (ping !== "pong") {
    throw new Error(`Rime runtime bridge ping failed: expected "pong", got "${ping}"`);
  }
  log(`[rime-cli] runtime.ping: ${ping}`);

  // 2. Runtime inspection
  const inspection = runtime.inspect();
  if (typeof inspection !== "string" || inspection.length === 0) {
    throw new Error("runtime.inspect must return a non-empty JSON string");
  }
  log(`[rime-cli] runtime.inspect: ${inspection}`);

  // 3. Runtime context
  const context = runtime.context();
  if (!context || context.schemaVersion !== 1 || !Array.isArray(context.modules)) {
    throw new Error("runtime.context must return a valid RuntimeContext with schemaVersion 1");
  }
  log(`[rime-cli] runtime.context modules (${context.modules.length}): ${context.modules.join(", ")}`);

  // 4. Window list
  const windows = await Window.list();
  if (!Array.isArray(windows)) {
    throw new Error("Window.list must resolve an array of Window instances");
  }
  log(`[rime-cli] Window.list: ${windows.length} windows found`);

  // 5. Process list
  const processes = await Process.list();
  if (!Array.isArray(processes) || processes.length === 0) {
    throw new Error("Process.list must resolve a non-empty array of ProcessInfo objects");
  }
  log(`[rime-cli] Process.list: ${processes.length} processes found`);
}

main().catch((error: unknown) => {
  (globalThis as { __rim_failure?: string }).__rim_failure =
    error instanceof Error ? error.stack || error.message : String(error);
});
