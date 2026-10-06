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

/** Bridge of the `rime:sound` module. Both calls are writes; neither builds an Action. */
export interface SoundBridge {
  beep(frequency?: number, duration?: number, options?: NativeActionOptions): Promise<null>;
  play(file: string, options?: NativeActionOptions & { wait?: boolean }): Promise<null>;
}

async function soundBridge(): Promise<SoundBridge> {
  const module = await import("rime:sound");
  return module.sound;
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
};
