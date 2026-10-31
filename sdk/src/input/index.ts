import { input as nativeInput } from "rime:input";
import type { InputBridge, RawKeyEvent, Subscription } from "./types";

export type { SendKeyStep } from "../send";

export * from "./types";
export * from "./events";
export * from "./hooks";

export { keyboard } from "./keyboard";
export { mouse } from "./mouse";

export const input: InputBridge = {
  ...nativeInput,
  onRawKey(handler: (event: RawKeyEvent) => void): Subscription {
    if (typeof (nativeInput as any).onRawKey === "function") {
      return (nativeInput as any).onRawKey(handler);
    }
    const id = nativeInput.subscribeRaw(handler);
    let closed = false;
    return {
      close() {
        if (!closed) {
          closed = true;
          nativeInput.unsubscribeRaw(id);
        }
      },
    };
  },
};
