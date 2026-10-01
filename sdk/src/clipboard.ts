import { runAction, type ActionOptions, type NativeActionOptions } from "./action";

export interface ClipboardText {
  text: string;
}

/** Bridge of the `rime:clipboard` module. Writes are Actions. */
export interface ClipboardBridge {
  /** Empty `text` means the clipboard holds no text format. */
  read(options?: NativeActionOptions): Promise<ClipboardText>;
  write(text: string, options?: NativeActionOptions): Promise<ClipboardText>;
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
};
