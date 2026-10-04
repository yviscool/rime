/** One injected keyboard transition as accepted by `input.send`. */
export interface SendKeyStep {
  /** Virtual key 1..254, or a UTF-16 code unit when {@link unicode} is set. */
  vk: number;
  down: boolean;
  /** `vk` is a UTF-16 code unit sent with KEYEVENTF_UNICODE (AHK SendUnicodeChar). */
  unicode?: boolean;
}

/** Send mode; mirrors AHK's aSendRaw state (SCM_RAW / SCM_RAW_TEXT). */
export type SendMode = "send" | "raw" | "text";

/** Snapshot state one `compileSend` call starts from. */
export interface SendCompileContext {
  /** Per-side modifier mask read from `input.modifiers()` before the batch. */
  state: number;
  /** Script-held persistent mask carried over from earlier sends (AHK sModifiersLR_persistent). */
  persistent: number;
}

/** Compiled batch plus the state the caller must carry across calls. */
export interface SendCompileResult {
  steps: SendKeyStep[];
  /** Updated persistent mask to store for the next send. */
  persistent: number;
  /** Modifier mask the injection must leave behind (blind rules included). */
  endState: number;
}
