import { beforeEach, expect, mock, test } from "bun:test";
import { ActionError, type NativeActionOptions } from "../../sdk/src/action";
import type { SoundPlayOptions } from "../../sdk/src/sound";

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

let beepCalls: BeepCall[] = [];
let playCalls: PlayCall[] = [];
let beepRejection: CodedRejection | null = null;
let beepRaw: unknown = null;
let playRejection: CodedRejection | null = null;

function reset(): void {
  beepCalls = [];
  playCalls = [];
  beepRejection = null;
  beepRaw = null;
  playRejection = null;
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
