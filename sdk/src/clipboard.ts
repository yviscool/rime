import { runAction, type ActionOptions, type NativeActionOptions } from "./action";

export interface ClipboardText {
  text: string;
}

/** Options for {@link clipboard.wait}: the shared action options plus ClipWait's flag. */
export interface ClipboardWaitOptions extends ActionOptions {
  /** Wait for any clipboard format instead of text/file content (AHK's second ClipWait argument). */
  anyData?: boolean;
}

/** Resolved shape of {@link clipboard.wait}. */
export interface ClipboardWaitResult {
  ready: true;
}

/** Raw shape of {@link ClipboardBridge.saveAll}: the opaque blob as bytes. */
export interface ClipboardSnapshot {
  bytes: number[];
}

/** Resolved shape of {@link ClipboardBridge.restoreAll}. */
export interface ClipboardRestoreResult {
  /** Formats the swap actually put back (skipped formats are not counted). */
  formats: number;
}

/**
 * AHK `ClipboardAll`: an opaque snapshot of every serialisable clipboard
 * format. The encoding is private to the runtime - the bytes are what JS
 * carries, and a caller can neither name nor synthesise an individual format.
 *
 * AHK's no-argument `ClipboardAll()` reads the clipboard inside the
 * constructor. A JavaScript constructor cannot await native work, so capture
 * is {@link clipboard.saveAll} and this constructor takes bytes that already
 * exist: `new ClipboardAll(await clipboard.saveAll().then(s => s.bytes))`.
 */
export class ClipboardAll {
  private readonly data: number[];

  /** AHK `ClipboardAll(data, size?)`. */
  constructor(data?: number[], size?: number) {
    const source = data ?? [];
    if (!Array.isArray(source)) {
      throw new TypeError("ClipboardAll(data): data must be an array of bytes");
    }
    for (const byte of source) {
      if (!Number.isInteger(byte) || byte < 0 || byte > 255) {
        throw new RangeError("ClipboardAll(data): data must hold integers in 0..255");
      }
    }
    if (size === undefined) {
      this.data = source.slice();
      return;
    }
    if (!Number.isInteger(size) || size < 0) {
      throw new TypeError("ClipboardAll(data, size): size must be a non-negative integer");
    }
    // AHK reads `size` bytes from the caller's buffer and trusts it; bounds
    // are checked here instead, so a short buffer fails rather than copying
    // whatever follows it.
    if (size > source.length) {
      throw new RangeError("ClipboardAll(data, size): size exceeds the data length");
    }
    this.data = source.slice(0, size);
  }

  /** Snapshot length in bytes (AHK's `Size`). */
  get size(): number {
    return this.data.length;
  }

  /** A copy of the snapshot, so a caller cannot mutate it in place. */
  get bytes(): number[] {
    return this.data.slice();
  }
}

/** Bridge of the `rime:clipboard` module. Writes are Actions. */
export interface ClipboardBridge {
  /** Empty `text` means the clipboard holds no text format. */
  read(options?: NativeActionOptions): Promise<ClipboardText>;
  write(text: string, options?: NativeActionOptions): Promise<ClipboardText>;
  /**
   * AHK `ClipWait`. Resolves `{ready:true}` once the clipboard holds content;
   * no Action Trace (worker-lane slices).
   */
  wait(options?: NativeActionOptions & { anyData?: boolean }): Promise<ClipboardWaitResult>;
  /** AHK `ClipboardAll()` with no arguments. Capability `windows.clipboard.read`. */
  saveAll(options?: NativeActionOptions): Promise<ClipboardSnapshot>;
  /** AHK `Clipboard := clipallObj`. Runs the `clipboard.restore` Action. */
  restoreAll(bytes: number[], options?: NativeActionOptions): Promise<ClipboardRestoreResult>;
}

async function clipboardBridge(): Promise<ClipboardBridge> {
  const module = await import("rime:clipboard");
  return module.clipboard;
}

export const clipboard = {
  /**
   * Reads text from the clipboard.
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  read(options?: ActionOptions): Promise<ClipboardText> {
    return runAction(options, (bridgeOptions) =>
      clipboardBridge().then((bridge) => bridge.read(bridgeOptions)),
    );
  },
  /**
   * Writes text through the `windows.clipboard.write` action pipeline.
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  write(text: string, options?: ActionOptions): Promise<ClipboardText> {
    return runAction(options, (bridgeOptions) =>
      clipboardBridge().then((bridge) => bridge.write(text, bridgeOptions)),
    );
  },
  /**
   * Waits until the clipboard holds text and file content (default) or any
   * format (`anyData: true`). Capability `windows.clipboard.read`; no Action
   * Trace (worker-lane slices).
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  wait(options?: ClipboardWaitOptions): Promise<ClipboardWaitResult> {
    const anyData = options?.anyData;
    return runAction(options, (bridgeOptions) =>
      clipboardBridge().then((bridge) => bridge.wait({ ...bridgeOptions, anyData })),
    );
  },
  /**
   * Captures every serialisable clipboard format into a {@link ClipboardAll}
   * (AHK `ClipboardAll()`). Read-only: capability `windows.clipboard.read`, no
   * Action Trace.
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  saveAll(options?: ActionOptions): Promise<ClipboardAll> {
    return runAction(options, (bridgeOptions) =>
      clipboardBridge().then((bridge) =>
        bridge.saveAll(bridgeOptions).then((result) => new ClipboardAll(result.bytes)),
      ),
    );
  },
  /**
   * Puts a snapshot back on the clipboard, replacing whatever is there (AHK
   * `Clipboard := clipallObj`). Runs the `clipboard.restore` Action, so it is
   * traced, cancellable and gated on `windows.clipboard.write`.
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  restoreAll(
    snapshot: ClipboardAll | number[],
    options?: ActionOptions,
  ): Promise<ClipboardRestoreResult> {
    const bytes = snapshot instanceof ClipboardAll ? snapshot.bytes : snapshot;
    return runAction(options, (bridgeOptions) =>
      clipboardBridge().then((bridge) => bridge.restoreAll(bytes, bridgeOptions)),
    );
  },
};
