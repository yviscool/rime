import { clipboard, Window, runtime } from "@rime/sdk";

// Rime Demo Script: Demonstrates Window listing, Active Window inspection,
// Control querying, and polite Clipboard read/write.

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
      // Best-effort debug output
    }
  }
}

async function main(): Promise<void> {
  log("=== Rime Desktop Runtime Demo ===");

  // 1. Verify runtime connectivity
  const pong = runtime.ping();
  log(`[1] Runtime status: ${pong}`);

  // 2. Window list demonstration
  const windows = await Window.list();
  log(`[2] Window List: ${windows.length} windows detected on desktop`);
  for (let i = 0; i < Math.min(3, windows.length); i++) {
    const snap = windows[i].snapshot;
    log(`    - Window #${i + 1}: [${snap.processName}] "${snap.title}" (Class: ${snap.className})`);
  }

  // 3. Active Window query
  const activeWindow = await Window.active();
  if (activeWindow) {
    const snap = activeWindow.snapshot;
    log(
      `[3] Active Window: [${snap.processName}] "${snap.title}" at (${snap.rect.left}, ${snap.rect.top}, ${snap.rect.right}, ${snap.rect.bottom})`,
    );
  } else {
    log("[3] Active Window: none currently focused");
  }

  // 4. Control query demonstration
  // Target active window, or fall back to the first available window
  const targetWindow = activeWindow ?? windows[0];
  if (targetWindow) {
    const controls = await targetWindow.controls();
    log(`[4] Controls in target window "${targetWindow.snapshot.title}": ${controls.length} controls`);

    if (controls.length > 0) {
      const first = controls[0];
      const ctrl = await targetWindow.control({ classNN: first.classNN });
      const bounds = ctrl.bounds;
      const visible = ctrl.visible;
      const enabled = ctrl.enabled;
      log(
        `    - Control ClassNN: ${first.classNN}, Visible: ${visible}, Enabled: ${enabled}, Bounds: ${bounds.width}x${bounds.height} at (${bounds.x}, ${bounds.y})`,
      );
    }

    const focusedId = targetWindow.focusedControl();
    log(`    - Focused Control ID: ${focusedId !== null ? focusedId : "none"}`);
  }

  // 5. Clipboard read/write demonstration (polite round-trip)
  const initialClipboard = await clipboard.read();
  log(`[5] Initial Clipboard text: ${JSON.stringify(initialClipboard.text)}`);

  const demoMessage = "Hello from Rime Runtime!";
  log(`    Writing to clipboard: "${demoMessage}"`);
  await clipboard.write(demoMessage);

  const updatedClipboard = await clipboard.read();
  log(`    Read back from clipboard: ${JSON.stringify(updatedClipboard.text)}`);

  if (updatedClipboard.text !== demoMessage) {
    throw new Error(
      `Clipboard verification mismatch: expected "${demoMessage}", got "${updatedClipboard.text}"`,
    );
  }

  // Restore the original clipboard content
  log("    Restoring initial clipboard content...");
  await clipboard.write(initialClipboard.text);
  log("    Clipboard restored successfully.");

  log("=== Rime Demo completed successfully ===");
}

main().catch((error: unknown) => {
  (globalThis as { __rim_failure?: string }).__rim_failure =
    error instanceof Error ? error.stack || error.message : String(error);
});
