import { runAction, type ActionOptions, type NativeActionOptions } from "./action";

/** Options of {@link sound.play}: the shared action options plus AHK's wait flag. */
export interface SoundPlayOptions extends ActionOptions {
  /**
   * Resolve when the sound has finished (AHK's second `SoundPlay` argument).
   * The wait is sliced on the worker lane, so a long file never pins a thread;
   * without it the promise resolves as soon as MCI accepted the play.
   */
  wait?: boolean;
}

/**
 * Target of an endpoint call: AHK's component and device arguments plus the
 * shared action options. Both keep AHK's string grammar - `""`/omitted is the
 * default (the endpoint's master control on the default render device), and
 * `"2"`, `"Wave"`, `"Wave:2"` mean what they mean in a script. The strings are
 * parsed natively (`SoundService::parse_component` / `parse_device`), so a bad
 * shape is a `target_gone`/`unsupported` failure, not a silent fallback to the
 * default endpoint.
 */
export interface SoundEndpointOptions extends ActionOptions {
  /** Control to address: `""` master (default), `"name"`, `"name:N"` or an instance number. */
  component?: string;
  /** Device to address: `""` default render endpoint (default), `"name"`, `"name:N"` or a 1-based index. */
  device?: string;
}

/**
 * Bridge of the `rime:sound` module. Every call goes straight to the
 * `SoundService`, so none of them builds an Action or lands in the trace;
 * `media.sound` is read natively inside each worker body.
 */
export interface SoundBridge {
  beep(frequency?: number, duration?: number, options?: NativeActionOptions): Promise<null>;
  play(file: string, options?: NativeActionOptions & { wait?: boolean }): Promise<null>;
  getVolume(
    options?: NativeActionOptions & { component?: string; device?: string },
  ): Promise<number>;
  setVolume(
    value: number | string,
    options?: NativeActionOptions & { component?: string; device?: string },
  ): Promise<null>;
  getMute(
    options?: NativeActionOptions & { component?: string; device?: string },
  ): Promise<boolean>;
  setMute(
    muted: boolean,
    options?: NativeActionOptions & { component?: string; device?: string },
  ): Promise<null>;
  getName(
    options?: NativeActionOptions & { component?: string; device?: string },
  ): Promise<string>;
}

async function soundBridge(): Promise<SoundBridge> {
  const module = await import("rime:sound");
  return module.sound;
}

/**
 * Splits an endpoint target into the bridge's `component`/`device` fields and
 * the shared action options. `undefined` and `""` both mean AHK's default, and
 * an option nobody passed stays an absent key rather than `undefined` - the
 * same rule `play` follows for `wait` - so a caller cannot smuggle a typed
 * `undefined` through and the native side sees one shape.
 */
function splitEndpoint(options: SoundEndpointOptions | undefined): {
  target: { component?: string; device?: string };
  action: ActionOptions;
} {
  const { component, device, ...action } = options ?? {};
  const target: { component?: string; device?: string } = {};
  if (component !== undefined) target.component = component;
  if (device !== undefined) target.device = device;
  return { target, action };
}

export const sound = {
  /**
   * AHK `SoundBeep(frequency?, duration?)`: 523 Hz for 150 ms by default, and
   * a negative duration falls back to 150 ms instead of running long. The
   * values are resolved natively (`SoundService::resolve_beep`), so a caller
   * cannot accidentally pass a fraction through.
   * Capability `media.sound`; no Action Trace (a direct service call).
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  beep(frequency?: number, duration?: number, options?: ActionOptions): Promise<void> {
    return runAction(options, (native) =>
      soundBridge().then((bridge) => bridge.beep(frequency, duration, native)),
    ).then(() => undefined);
  },
  /**
   * AHK `SoundPlay(file, wait?)`: plays one file on the runtime's single MCI
   * alias, closing whatever that alias still held first - so two plays never
   * race for the device. A file starting with `*` is AHK's `MessageBeep`
   * path (`*` alone is type 0) and never waits, exactly like AHK.
   *
   * `wait` rides on the options object rather than as a second positional
   * argument, so the shared action options stay one object.
   *
   * MCI picks the device from the file's extension, so the path must end in a
   * type it knows (`.wav`, `.mp3`, ...): the same bytes named `.tmp` are
   * rejected with `execution_failed: MCI open failed` while a `.wav` plays.
   * Capability `media.sound`; no Action Trace.
   * @throws ActionError with `execution_failed` (MCI rejected the file),
   *   `timeout` / `cancelled` (a wait that ran out), `capability_denied`.
   */
  play(file: string, options?: SoundPlayOptions): Promise<void> {
    const { wait, ...action } = options ?? {};
    return runAction(action, (native) =>
      soundBridge().then((bridge) =>
        bridge.play(file, wait === undefined ? native : { ...native, wait }),
      ),
    ).then(() => undefined);
  },
  /**
   * AHK `SoundGetVolume(component?, device?)`: the volume percentage of the
   * target control, defaulting to the default endpoint's master control -
   * exactly what the script call with no arguments does. The value is a
   * percentage (0..100) and a read-back can differ from a value just written
   * in the last bits, because it round-trips through float32 the same way
   * AHK's return does.
   * Capability `media.sound`; no Action Trace (a direct service call).
   * @throws ActionError with `target_gone` (no such device or component),
   *   `unsupported` (that control carries no volume), `capability_denied`.
   */
  getVolume(options?: SoundEndpointOptions): Promise<number> {
    const { target, action } = splitEndpoint(options);
    return runAction(action, (native) =>
      soundBridge().then((bridge) => bridge.getVolume({ ...native, ...target })),
    );
  },
  /**
   * AHK `SoundSetVolume(value, component?, device?)`: writes a percentage, or
   * adjusts with `"+5"` / `"-5"`. A negative number is the adjustment form
   * too - AHK decides from the first character of the stringified argument,
   * and this API follows that rule rather than inventing a second one - so
   * `setVolume(-5)` lowers by 5 and there is no absolute negative volume.
   *
   * Inputs are parsed by the same native parser the script path uses: spaces
   * are trimmed, `0x` hex and exponents are legal, an unparseable string
   * rejects with a `TypeError` before any worker runs, and the value is
   * clamped to [-100, 100] after scaling - `1e999` lands on 100, it is not an
   * error.
   * Capability `media.sound`; no Action Trace (a direct service call).
   * @throws ActionError with `target_gone` / `unsupported` / `capability_denied`.
   * @throws TypeError when `value` is neither a number nor a string, or is not
   *   a setting AHK would accept (non-finite numbers included).
   */
  setVolume(value: number | string, options?: SoundEndpointOptions): Promise<void> {
    const { target, action } = splitEndpoint(options);
    return runAction(action, (native) =>
      soundBridge().then((bridge) =>
        bridge.setVolume(value, { ...native, ...target }),
      ),
    ).then(() => undefined);
  },
  /**
   * AHK `SoundGetMute(component?, device?)`: whether the target control is
   * muted, as a boolean instead of AHK's 1/0.
   * Capability `media.sound`; no Action Trace (a direct service call).
   * @throws ActionError with `target_gone` / `unsupported` / `capability_denied`.
   */
  getMute(options?: SoundEndpointOptions): Promise<boolean> {
    const { target, action } = splitEndpoint(options);
    return runAction(action, (native) =>
      soundBridge().then((bridge) => bridge.getMute({ ...native, ...target })),
    );
  },
  /**
   * AHK `SoundSetMute(muted, component?, device?)`: absolute mute state.
   *
   * AHK's relative form (`SoundSetMute("+1")` toggles, `"-1"` unmutes) is a
   * string convention with no honest boolean spelling, so this API takes a
   * boolean and a toggle is `setMute(!await getMute())`. The omission is
   * deliberate and recorded in `docs/api/sound.md`.
   * Capability `media.sound`; no Action Trace (a direct service call).
   * @throws ActionError with `target_gone` / `unsupported` / `capability_denied`.
   * @throws TypeError when `muted` is not a boolean.
   */
  setMute(muted: boolean, options?: SoundEndpointOptions): Promise<void> {
    const { target, action } = splitEndpoint(options);
    return runAction(action, (native) =>
      soundBridge().then((bridge) => bridge.setMute(muted, { ...native, ...target })),
    ).then(() => undefined);
  },
  /**
   * AHK `SoundGetName(component?, device?)`: the friendly name of the target
   * control's endpoint ("Speakers (Realtek(R) Audio)" on a typical machine).
   * Capability `media.sound`; no Action Trace (a direct service call).
   * @throws ActionError with `target_gone` / `unsupported` / `capability_denied`.
   */
  getName(options?: SoundEndpointOptions): Promise<string> {
    const { target, action } = splitEndpoint(options);
    return runAction(action, (native) =>
      soundBridge().then((bridge) => bridge.getName({ ...native, ...target })),
    );
  },
};
