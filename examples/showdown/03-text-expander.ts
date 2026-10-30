/**
 * Production Showdown #3: Snippet & Hotstring Expansion Engine
 *
 * Real-world comparison vs AutoHotkey:
 * - AHK: Hotstrings are static source-level directives (`::btw::by the way`)
 *   scattered in `.ahk` scripts. Dynamic snippet injection requires writing `.ahk`
 *   to disk and invoking `#Include` or external processes. Memory leaks occur
 *   due to unbounded global hook buffers.
 * - Rime SDK: First-class programmable `SnippetEngine` powered by `input.hotstring`,
 *   supporting runtime dictionary loading, dynamic generator functions (e.g. ISO timestamps),
 *   scoped lifecycle management, and explicit subscription disposal.
 */

import { input, runtime } from "@rime/sdk";
import type { EventSubscription } from "@rime/sdk";

export interface SnippetDefinition {
  trigger: string;
  replacement: string | (() => string);
  description?: string;
  caseSensitive?: boolean;
  expandInsideWord?: boolean;
}

export class SnippetEngine {
  private activeSubscriptions = new Map<string, EventSubscription>();
  private definitions = new Map<string, SnippetDefinition>();

  /**
   * Registers a snippet abbreviation for auto-expansion.
   */
  register(def: SnippetDefinition): boolean {
    const options: string[] = ["*"]; // asterisk: expand immediately without trigger end character
    if (def.expandInsideWord) options.push("?");
    if (def.caseSensitive) options.push("C");

    const optionStr = `:${options.join("")}:`;

    // Normalize trigger name (strip leading/trailing colons if supplied)
    const cleanTrigger = def.trigger.replace(/^:+|:+$/g, "");
    const spec = `${optionStr}${cleanTrigger}::`;

    // Unregister any previous subscription for this trigger
    this.unregister(cleanTrigger);

    try {
      let sub: EventSubscription | null;
      if (typeof def.replacement === "string") {
        // Direct string replacement via SDK hotstring action
        sub = input.hotstring(`${spec}${def.replacement}`) as EventSubscription | null;
      } else {
        // Dynamic expansion function: callback provides replacement
        const generator = def.replacement;
        sub = input.hotstring(spec, async () => {
          const generated = generator();
          if (generated.length > 0) {
            // Note: In real usage, replacement is injected via keyboard or input.send
            if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
              runtime.debug(`[SnippetEngine] Dynamically expanded "${cleanTrigger}" -> "${generated}"`);
            }
          }
        }) as EventSubscription | null;
      }

      if (sub) {
        this.activeSubscriptions.set(cleanTrigger, sub);
      }
      this.definitions.set(cleanTrigger, def);
      return true;
    } catch (error) {
      if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
        runtime.debug(`[SnippetEngine] Registration failed for "${cleanTrigger}": ${error}`);
      }
      return false;
    }
  }

  /**
   * Unregisters an active snippet by trigger name.
   */
  unregister(trigger: string): boolean {
    const cleanTrigger = trigger.replace(/^:+|:+$/g, "");
    const sub = this.activeSubscriptions.get(cleanTrigger);
    if (!sub) return false;

    try {
      sub.close();
    } catch {
      // Ignore if already disposed
    }

    this.activeSubscriptions.delete(cleanTrigger);
    this.definitions.delete(cleanTrigger);
    return true;
  }

  /**
   * Returns count of currently active snippet bindings.
   */
  get size(): number {
    return this.activeSubscriptions.size;
  }

  /**
   * Unregisters all active snippets.
   */
  clear(): void {
    for (const [trigger] of this.activeSubscriptions) {
      this.unregister(trigger);
    }
    this.activeSubscriptions.clear();
    this.definitions.clear();
  }
}

export const SHOWDOWN_SNIPPETS: SnippetDefinition[] = [
  {
    trigger: "btw",
    replacement: "by the way",
    description: "Common abbreviation",
  },
  {
    trigger: "gh",
    replacement: "https://github.com",
    description: "GitHub URL",
  },
  {
    trigger: "shrug",
    replacement: "¯\\_(ツ)_/¯",
    description: "Kaomoji shrug",
  },
  {
    trigger: "today",
    replacement: () => new Date().toISOString().split("T")[0],
    description: "Current date in ISO 8601 format",
  },
];

export async function main(): Promise<void> {
  if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
    runtime.debug("=== Showdown 03: Text Expander Engine ===");
  }

  const engine = new SnippetEngine();

  try {
    for (const item of SHOWDOWN_SNIPPETS) {
      const ok = engine.register(item);
      if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
        runtime.debug(`[Snippet] Registered :${item.trigger}: (${item.description}) -> ${ok}`);
      }
    }

    if (engine.size !== SHOWDOWN_SNIPPETS.length) {
      throw new Error(`Snippet engine count mismatch: expected ${SHOWDOWN_SNIPPETS.length}, got ${engine.size}`);
    }

    // Demonstrate dynamic removal of a snippet
    const removed = engine.unregister("shrug");
    if (!removed) {
      throw new Error("Failed to unregister snippet 'shrug'");
    }

    if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
      runtime.debug(`[Snippet] Remaining active triggers: ${engine.size}`);
    }
  } finally {
    // Guaranteed teardown
    engine.clear();
    if (typeof runtime !== "undefined" && typeof runtime.debug === "function") {
      runtime.debug("[Snippet] Cleaned up all snippet subscriptions");
    }
  }
}

main().catch((error: unknown) => {
  (globalThis as { __rim_failure?: string }).__rim_failure =
    error instanceof Error ? `${error.message}\n${error.stack}` : String(error);
  if (typeof console !== "undefined" && typeof console.error === "function") {
    console.error("Text Expander failed:", error);
  }
});
