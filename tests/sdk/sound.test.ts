import { beforeEach, expect, mock, test } from "bun:test";
import { ActionError, type NativeActionOptions } from "../../sdk/src/action";
import type { SoundEndpointOptions, SoundPlayOptions } from "../../sdk/src/sound";

// Realism: L3 - the real sdk/src/sound.ts facade runs against an
// instrumented rime:sound bridge; the native module is the environment, never
// the unit under test. Every expectation is a literal written by hand, so a
// facade that drops AHK's wait flag, leaks it into the shared action options
// or loses a bridge rejection cannot pass by agreeing with itself.

type CodedRejection = { code: string; message: string };

const codedError = ({ code, message }: CodedRejection) =>
  Object.assign(new Error(message), { code });

type BeepCall = [number | undefined, number | undefined, NativeActionOptions | undefined];
type PlayCall = [string, (NativeActionOptions & { wait?: boolean }) | undefined];
type TargetOptions = NativeActionOptions & { component?: string; device?: string };
type EndpointCall = [unknown, TargetOptions];

let beepCalls: BeepCall[] = [];
let playCalls: PlayCall[] = [];
let volumeCalls: EndpointCall[] = [];
let endpointRejection: CodedRejection | null = null;
let beepRejection: CodedRejection | null = null;
let beepRaw: unknown = null;
let playRejection: CodedRejection | null = null;
let volumeResult: number = 50;
let muteResult: boolean = false;
let nameResult: string = "Speakers (Realtek(R) Audio)";

function reset(): void {
  beepCalls = [];
  playCalls = [];
  volumeCalls = [];
  endpointRejection = null;
  beepRejection = null;
  beepRaw = null;
  playRejection = null;
  volumeResult = 50;
  muteResult = false;
  nameResult = "Speakers (Realtek(R) Audio)";
}

// The endpoint calls share one recorder: `value` is the argument they were
// given (the first argument for setVolume/setMute, undefined for the reads),
// so a facade that drops or reorders the value and the target cannot hide
// behind an object that happens to match.
function record(value: unknown, options: TargetOptions): TargetOptions {
  volumeCalls.push([value, options]);
  return options;
}

const bridge = {
  async beep(frequency?: number, duration?: number, options?: NativeActionOptions) {
    beepCalls.push([frequency, duration, options]);
    if (beepRaw) throw beepRaw;
    if (beepRejection) throw codedError(beepRejection);
    return null;
  },
  async play(file: string, options?: NativeActionOptions & { wait?: boolean }) {
    playCalls.push([file, options]);
    if (playRejection) throw codedError(playRejection);
    return null;
  },
  async getVolume(options?: TargetOptions) {
    record(undefined, options ?? {});
    if (endpointRejection) throw codedError(endpointRejection);
    return volumeResult;
  },
  async setVolume(value: number | string, options?: TargetOptions) {
    record(value, options ?? {});
    if (endpointRejection) throw codedError(endpointRejection);
    return null;
  },
  async getMute(options?: TargetOptions) {
    record(undefined, options ?? {});
    if (endpointRejection) throw codedError(endpointRejection);
    return muteResult;
  },
  async setMute(muted: boolean, options?: TargetOptions) {
    record(muted, options ?? {});
    if (endpointRejection) throw codedError(endpointRejection);
    return null;
  },
  async getName(options?: TargetOptions) {
    record(undefined, options ?? {});
    if (endpointRejection) throw codedError(endpointRejection);
    return nameResult;
  },
};

mock.module("rime:sound", () => ({ sound: bridge }));

const { sound } = await import("../../sdk/src/sound");

beforeEach(reset);

test("beep omits AHK's defaults so the native side resolves them", async () => {
  await expect(sound.beep()).resolves.toBe(undefined);
  // Frequency and duration stay undefined: the 523 Hz / 150 ms defaults and
  // the negative-duration fallback belong to SoundService::resolve_beep, and
  // pre-filling them here would move the documented AHK literals into TS.
  expect(beepCalls).toEqual([[undefined, undefined, undefined]]);
});

test("beep forwards an explicit pitch, duration and the action options", async () => {
  await expect(sound.beep(440, 80, { deadlineMs: 250 })).resolves.toBe(undefined);
  expect(beepCalls).toEqual([[440, 80, { deadlineMs: 250 }]]);
});

test("play passes no wait flag when the caller gave none", async () => {
  await expect(sound.play("C:\\tmp\\chime.wav")).resolves.toBe(undefined);
  // toStrictEqual (not toEqual): the absent flag must be an absent key, so a
  // facade that always writes wait cannot smuggle wait: undefined through.
  expect(playCalls.length).toBe(1);
  expect(playCalls[0][0]).toBe("C:\\tmp\\chime.wav");
  expect(playCalls[0][1]).toStrictEqual({});
});

test("play moves wait out of the action options and into the bridge call", async () => {
  const options: SoundPlayOptions = { wait: true, deadlineMs: 50 };
  await expect(sound.play("C:\\tmp\\chime.wav", options)).resolves.toBe(undefined);
  // runAction only carries the shared action fields, so wait has to be
  // re-attached; dropping it turns a blocking play into a fire-and-forget one.
  expect(playCalls).toEqual([["C:\\tmp\\chime.wav", { deadlineMs: 50, wait: true }]]);
  // The caller's options object is not mutated by the split.
  expect(options).toStrictEqual({ wait: true, deadlineMs: 50 });
});

test("play forwards wait: false rather than treating it as absent", async () => {
  await sound.play("C:\\tmp\\chime.wav", { wait: false, deadlineMs: 20 });
  expect(playCalls).toEqual([["C:\\tmp\\chime.wav", { deadlineMs: 20, wait: false }]]);
});

test("a coded bridge rejection becomes an ActionError with that code", async () => {
  beepRejection = {
    code: "capability_denied",
    message: "required capability was not granted: media.sound",
  };
  await expect(sound.beep()).rejects.toBeInstanceOf(ActionError);
  await expect(sound.beep()).rejects.toMatchObject({ code: "capability_denied" });

  playRejection = {
    code: "execution_failed",
    message: "MCI open failed",
  };
  await expect(sound.play("C:\\tmp\\gone.wav")).rejects.toBeInstanceOf(ActionError);
  await expect(sound.play("C:\\tmp\\gone.wav")).rejects.toMatchObject({
    code: "execution_failed",
    message: "MCI open failed",
  });
});

test("a rejection without a code is rethrown unchanged", async () => {
  beepRaw = new Error("bridge exploded");
  await expect(sound.beep()).rejects.toBe(beepRaw);
});

test("getVolume resolves the native percentage and forwards no options by default", async () => {
  volumeResult = 37.5;
  await expect(sound.getVolume()).resolves.toBe(37.5);
  // toStrictEqual: no component/device/deadline keys may appear when the
  // caller passed nothing, so a facade that always writes the target cannot
  // claim it stayed silent.
  expect(volumeCalls).toEqual([[undefined, {}]]);
});

test("getVolume forwards the AHK target strings and the action options", async () => {
  const options: SoundEndpointOptions = { component: "Wave", device: "2", deadlineMs: 40 };
  await sound.getVolume(options);
  expect(volumeCalls).toEqual([[undefined, { deadlineMs: 40, component: "Wave", device: "2" }]]);
  // The caller's object survives the split untouched.
  expect(options).toStrictEqual({ component: "Wave", device: "2", deadlineMs: 40 });
});

test("an explicitly undefined component stays an absent key", async () => {
  await sound.getVolume({ component: undefined, device: undefined });
  expect(volumeCalls[0][1]).toStrictEqual({});
});

test("an explicit empty component is forwarded: it is AHK's master control", async () => {
  await sound.getVolume({ component: "" });
  expect(volumeCalls[0][1]).toStrictEqual({ component: "" });
});

test("setVolume passes a percentage and an adjustment through unchanged", async () => {
  await expect(sound.setVolume(42)).resolves.toBe(undefined);
  await expect(sound.setVolume("+5")).resolves.toBe(undefined);
  await expect(sound.setVolume(-5)).resolves.toBe(undefined);
  // The value is forwarded verbatim - no TS-side scaling, rounding or sign
  // rule; the parser that decides absolute vs relative is the native one.
  expect(volumeCalls).toEqual([
    [42, {}],
    ["+5", {}],
    [-5, {}],
  ]);
});

test("setVolume keeps component/device behind the value argument", async () => {
  await sound.setVolume("+1", { component: "Master Volume", device: "Speakers", parentActionId: 7 });
  expect(volumeCalls).toEqual([
    ["+1", { parentActionId: 7, component: "Master Volume", device: "Speakers" }],
  ]);
});

test("getMute and setMute carry the boolean, not 1/0", async () => {
  muteResult = true;
  await expect(sound.getMute()).resolves.toBe(true);
  await expect(sound.setMute(true)).resolves.toBe(undefined);
  await expect(sound.setMute(false)).resolves.toBe(undefined);
  expect(volumeCalls).toEqual([[undefined, {}], [true, {}], [false, {}]]);
});

test("getName resolves the endpoint's friendly name", async () => {
  nameResult = "Headphones (JBL)";
  await expect(sound.getName({ device: "1" })).resolves.toBe("Headphones (JBL)");
  expect(volumeCalls).toEqual([[undefined, { device: "1" }]]);
});

test("a coded endpoint rejection becomes an ActionError with that code", async () => {
  endpointRejection = { code: "target_gone", message: "Device not found" };
  await expect(sound.getVolume()).rejects.toBeInstanceOf(ActionError);
  await expect(sound.getVolume()).rejects.toMatchObject({
    code: "target_gone",
    message: "Device not found",
  });
  await expect(sound.setMute(true)).rejects.toMatchObject({ code: "target_gone" });
});
