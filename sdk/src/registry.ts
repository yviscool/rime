import { runAction, type ActionOptions, type NativeActionOptions } from "./action";

/** Registry view for subsequent operations (native = OS default word size). */
export type RegistryView = "default" | "64" | "32";

/** Lowercase Win32 value type names this runtime carries. */
export type RegistryValueType = "sz" | "expand_sz" | "dword" | "qword" | "multi_sz" | "binary";

/** One stored value: `type` discriminates the shape of `value` on the wire. */
export type RegistryValue =
  | { type: "sz" | "expand_sz"; value: string }
  | { type: "dword" | "qword"; value: number }
  | { type: "multi_sz"; value: string[] }
  | { type: "binary"; value: number[] };

/** Value fields of the `set` op: the chosen type decides the value's shape. */
export type RegistryValuePayload =
  | { type: "sz" | "expand_sz"; value: string }
  | { type: "dword" | "qword"; value: number }
  | { type: "multi_sz"; value: string[] }
  | { type: "binary"; value: number[] };

/**
 * Payload of one `registry.write` action. `name` addresses the default value
 * when omitted; `set` never creates the key (call `createKey` first), so the
 * effect of an Action is decided by its payload alone.
 */
export type RegistryWritePayload =
  | ({ op: "set"; key: string; name?: string } & RegistryValuePayload)
  | { op: "createKey"; key: string }
  | { op: "delete"; key: string; name?: string }
  | { op: "deleteKey"; key: string };

/** Result of a successful `registry.write` action. */
export interface RegistryWriteResult {
  key: string;
  op: "set" | "createKey" | "delete" | "deleteKey";
}

/**
 * Bridge of the `rime:registry` module. `write` is an Action (queued, traced,
 * capability `registry.write`); `read` and the view pair are direct service
 * calls, so they resolve without passing through the Action kernel.
 */
export interface RegistryBridge {
  read(key: string, name?: string, options?: NativeActionOptions): Promise<RegistryValue>;
  write(
    payload: RegistryWritePayload,
    options?: NativeActionOptions,
  ): Promise<RegistryWriteResult>;
  /** Synchronous in the native module; the SDK facade is async only because the module loads dynamically. */
  view(): { view: RegistryView };
  setView(view: RegistryView): { view: RegistryView };
}

async function registryBridge(): Promise<RegistryBridge> {
  const module = await import("rime:registry");
  return module.registry;
}

export const registry = {
  /**
   * Reads one value: `name` omitted or `""` addresses the default value.
   * @throws ActionError with `capability_denied` / `target_gone` / `unsupported`.
   * @throws TypeError when `key` is not a non-empty string (thrown by the module).
   */
  read(key: string, name?: string, options?: ActionOptions): Promise<RegistryValue> {
    return runAction(options, (bridgeOptions) =>
      registryBridge().then((bridge) => bridge.read(key, name, bridgeOptions)),
    );
  },
  /**
   * Writes through the `registry.write` action pipeline (set / createKey /
   * delete / deleteKey).
   * @throws ActionError with `invalid_contract` / `target_gone` / `capability_denied`.
   */
  write(payload: RegistryWritePayload, options?: ActionOptions): Promise<RegistryWriteResult> {
    return runAction(options, (bridgeOptions) =>
      registryBridge().then((bridge) => bridge.write(payload, bridgeOptions)),
    );
  },
  /** Current view: `"default"`, `"64"` or `"32"`. */
  async view(): Promise<RegistryView> {
    const bridge = await registryBridge();
    return bridge.view().view;
  },
  /** Selects the view for subsequent operations; rejects an unknown view. */
  async setView(view: RegistryView): Promise<RegistryView> {
    const bridge = await registryBridge();
    return bridge.setView(view).view;
  },
};
