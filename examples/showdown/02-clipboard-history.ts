/**
 * Production Showdown #2: Ring Buffer Clipboard History Manager
 *
 * Real-world comparison vs AutoHotkey:
 * - AHK: Relies on `OnClipboardChange` label/function with global variables,
 *   untyped `A_Clipboard`, and raw binary `ClipboardAll` memory blocks that
 *   readily leak memory, offer no ring-buffer capacity limits, and corrupt
 *   when formats contain non-serializable descriptors.
 * - Rime SDK: Pure TypeScript `ClipboardRingBuffer` with bounded memory capacity,
 *   immutable `ClipboardEntry` objects, async `clipboard.read()`, `clipboard.write()`,
 *   and full-format `clipboard.saveAll()` / `clipboard.restoreAll()` snapshots.
 */

import { clipboard, runtime } from "@rime/sdk";
import type { ClipboardAll } from "@rime/sdk";

export interface ClipboardEntry {
  id: number;
  text: string;
  length: number;
  timestamp: number;
  snapshot?: ClipboardAll;
}

export class ClipboardRingBuffer {
  private buffer: ClipboardEntry[] = [];
  private nextId = 1;
  private lastObservedText: string | null = null;

  constructor(readonly capacity = 25) {
    if (capacity <= 0) {
      throw new RangeError("ClipboardRingBuffer capacity must be a positive integer");
    }
  }

  get size(): number {
    return this.buffer.length;
  }

  get entries(): readonly ClipboardEntry[] {
    return this.buffer;
  }

  /**
   * Records a new text item if it differs from the most recently captured item.
   * Drops the oldest entry when buffer capacity is reached.
   */
  record(text: string, snapshot?: ClipboardAll): ClipboardEntry | null {
    if (text === this.lastObservedText || text.length === 0) {
      return null;
    }

    const entry: ClipboardEntry = {
      id: this.nextId++,
      text,
      length: text.length,
      timestamp: Date.now(),
      snapshot,
    };

    this.lastObservedText = text;
    this.buffer.unshift(entry);

    if (this.buffer.length > this.capacity) {
      this.buffer.pop();
    }

    return entry;
  }

  /**
   * Polls the live clipboard and captures changes into history.
   */
  async checkClipboard(includeFullSnapshot = false): Promise<ClipboardEntry | null> {
    const current = await clipboard.read();
    if (current.text === this.lastObservedText || current.text.length === 0) {
      return null;
    }

    let snap: ClipboardAll | undefined;
    if (includeFullSnapshot) {
      try {
        snap = await clipboard.saveAll();
      } catch {
        // Fall back to text only if binary snapshot fails
      }
    }

    return this.record(current.text, snap);
  }

  /**
   * Restores a previously saved item back to the OS clipboard by historical index (0 = latest).
   */
  async restoreIndex(index: number): Promise<boolean> {
    if (index < 0 || index >= this.buffer.length) {
      return false;
    }
    const target = this.buffer[index];
    if (target.snapshot && target.snapshot.size > 0) {
      try {
        await clipboard.restoreAll(target.snapshot);
        this.lastObservedText = target.text;
        return true;
      } catch {
        // Fallback to text write if restoreAll encounters unsupported format
      }
    }

    await clipboard.write(target.text);
    this.lastObservedText = target.text;
    return true;
  }

  /**
   * Clears the history buffer.
   */
  clear(): void {
    this.buffer = [];
    this.lastObservedText = null;
  }
}

export async function main(): Promise<void> {
  if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
    runtime.debug("=== Showdown 02: Ring Buffer Clipboard History ===");
  }

  const manager = new ClipboardRingBuffer(5);

  // 1. Preserve initial clipboard state
  const initial = await clipboard.read();
  const initialText = initial.text;

  try {
    // 2. Simulate historical entries
    const samples = [
      "Alpha Item #1: Order 1042",
      "Beta Item #2: User authentication token",
      "Gamma Item #3: System config JSON",
      "Delta Item #4: Git commit hash",
    ];

    for (const item of samples) {
      await clipboard.write(item);
      const recorded = await manager.checkClipboard();
      if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
        runtime.debug(`[Clipboard] Recorded entry #${recorded?.id}: "${recorded?.text}"`);
      }
    }

    if (manager.size !== samples.length) {
      throw new Error(`Ring buffer size mismatch: expected ${samples.length}, got ${manager.size}`);
    }

    // 3. Demonstrate restore from ring buffer (restore item at index 1 -> Gamma)
    const success = await manager.restoreIndex(1);
    if (!success) {
      throw new Error("Failed to restore clipboard item at index 1");
    }

    const currentAfterRestore = await clipboard.read();
    if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
      runtime.debug(`[Clipboard] Restored index 1: "${currentAfterRestore.text}"`);
    }

    if (currentAfterRestore.text !== samples[2]) {
      throw new Error(`Restored text mismatch: expected "${samples[2]}", got "${currentAfterRestore.text}"`);
    }
  } finally {
    // 4. Guaranteed restoration of user clipboard
    await clipboard.write(initialText);
    if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
      runtime.debug("[Clipboard] Restored initial clipboard contents successfully");
    }
  }
}

main().catch((error: unknown) => {
  (globalThis as { __rim_failure?: string }).__rim_failure =
    error instanceof Error ? `${error.message}\n${error.stack}` : String(error);
  if (typeof console !== "undefined" && typeof console.error === "function") {
    console.error("Clipboard History failed:", error);
  }
});
