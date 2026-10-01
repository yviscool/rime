import { Process, Window, clipboard, input, runtime } from "@rime/sdk";

// Rim runs inside the QuickJS host as the platform proof: every call below
// crosses SDK -> native module -> Win32 service. The harness settles the
// host afterwards and fails the build when this main rejects.
async function main(): Promise<void> {
  if (runtime.ping() !== "pong") {
    throw new Error("Rime runtime bridge is unavailable");
  }

  const windows = await Window.list();
  if (!Array.isArray(windows)) {
    throw new Error("Window.list must resolve an array");
  }

  const processes = await Process.list();
  if (processes.length === 0) {
    throw new Error("Process.list must resolve a non-empty array");
  }

  const clip = await clipboard.read();
  if (typeof clip.text !== "string") {
    throw new Error("clipboard.read must resolve { text }");
  }

  if (typeof input.subscribe !== "function") {
    throw new Error("input.subscribe must be a function");
  }
}

main().catch((error: unknown) => {
  (globalThis as { __rim_failure?: string }).__rim_failure = String(error);
});
