/**
 * Realism: L3 (Simulated Component - mock rime:input bridge simulating raw input events)
 */
import { beforeEach, expect, mock, test } from "bun:test";
import type { RawKeyEvent } from "../../sdk/src/input";

let listeners: Map<number, (event: RawKeyEvent) => void> = new Map();
let nextId = 1;
let unsubscribedIds: number[] = [];

mock.module("rime:input", () => ({
  input: {
    subscribeRaw: (handler: (event: RawKeyEvent) => void): number => {
      const id = nextId++;
      listeners.set(id, handler);
      return id;
    },
    unsubscribeRaw: (id: number): boolean => {
      unsubscribedIds.push(id);
      return listeners.delete(id);
    },
  },
}));

const { input, keyboard } = await import("../../sdk/src/input");

beforeEach(() => {
  listeners.clear();
  nextId = 1;
  unsubscribedIds = [];
});

test("input.onRawKey registers a listener and receives events with deviceId", () => {
  const received: RawKeyEvent[] = [];
  const sub = input.onRawKey((event) => {
    received.push(event);
  });

  expect(sub).toBeDefined();
  expect(typeof sub.close).toBe("function");

  const mockEvent1: RawKeyEvent = {
    deviceId: "\\\\?\\HID#VID_046D&PID_C31C#1&23456",
    vk: 0x41, // 'A'
    scanCode: 0x1e,
    down: true,
    flags: 0,
  };

  const mockEvent2: RawKeyEvent = {
    deviceId: "\\\\?\\HID#VID_1532&PID_0070#2&98765", // second physical keyboard
    vk: 0x42, // 'B'
    scanCode: 0x30,
    down: true,
    flags: 0,
  };

  for (const listener of listeners.values()) {
    listener(mockEvent1);
    listener(mockEvent2);
  }

  expect(received).toHaveLength(2);
  expect(received[0]).toEqual(mockEvent1);
  expect(received[1]).toEqual(mockEvent2);
  expect(received[0].deviceId).not.toBe(received[1].deviceId);
});

test("sub.close() cleanly unsubscribes and stops receiving events", () => {
  const received: RawKeyEvent[] = [];
  const sub = input.onRawKey((event) => {
    received.push(event);
  });

  const mockEvent: RawKeyEvent = {
    deviceId: "\\\\?\\HID#VID_046D&PID_C31C#1&23456",
    vk: 0x1b, // Escape
    scanCode: 0x01,
    down: false,
    flags: 1,
  };

  for (const listener of listeners.values()) {
    listener(mockEvent);
  }
  expect(received).toHaveLength(1);

  // Close the subscription
  sub.close();
  expect(unsubscribedIds).toContain(1);
  expect(listeners.size).toBe(0);

  // Subsequent events dispatched to remaining listeners (none)
  for (const listener of listeners.values()) {
    listener(mockEvent);
  }
  expect(received).toHaveLength(1);
});

test("sub.close() is idempotent", () => {
  const sub = input.onRawKey(() => {});
  expect(listeners.size).toBe(1);

  sub.close();
  expect(unsubscribedIds).toHaveLength(1);

  // Second close should be safe and idempotent
  sub.close();
  expect(unsubscribedIds).toHaveLength(1);
});

test("keyboard.onRawKey delegates to input.onRawKey", () => {
  const received: RawKeyEvent[] = [];
  const sub = keyboard.onRawKey((event) => {
    received.push(event);
  });

  expect(typeof sub.close).toBe("function");
  const event: RawKeyEvent = {
    deviceId: "\\\\?\\KEYBOARD_1",
    vk: 0x20, // Space
    scanCode: 0x39,
    down: true,
    flags: 0,
  };

  for (const listener of listeners.values()) {
    listener(event);
  }
  expect(received).toEqual([event]);

  sub.close();
  expect(listeners.size).toBe(0);
});
