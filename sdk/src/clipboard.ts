export interface ClipboardText {
  text: string;
}

/** Bridge of the `rime:clipboard` module. Writes are Actions. */
export interface ClipboardBridge {
  /** Empty `text` means the clipboard holds no text format. */
  read(): Promise<ClipboardText>;
  write(text: string): Promise<ClipboardText>;
}

async function clipboardBridge(): Promise<ClipboardBridge> {
  const module = await import("rime:clipboard");
  return module.clipboard;
}

export const clipboard = {
  read(): Promise<ClipboardText> {
    return clipboardBridge().then((bridge) => bridge.read());
  },
  /** Writes text through the `clipboard.write` action pipeline. */
  write(text: string): Promise<ClipboardText> {
    return clipboardBridge().then((bridge) => bridge.write(text));
  },
};
