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
};
