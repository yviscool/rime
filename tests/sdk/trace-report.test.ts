import { expect, test } from "bun:test";
import {
  analyzeTrace,
  formatReport,
  parseTraceLines,
  type TraceEntry,
} from "../../tools/trace-report";

const sampleLines = [
  JSON.stringify({
    sequence: 1,
    kind: "StateChanged",
    subject: "runtime.start",
    detail: "",
    actionId: 0,
    capability: "",
    resultCode: "",
    durationMs: 0,
    unixMs: 1730000000000,
    queueWaitMs: 0,
  }),
  JSON.stringify({
    sequence: 2,
    kind: "ActionAccepted",
    subject: "storage.write",
    detail: "",
    actionId: 1,
    capability: "filesystem.write",
    resultCode: "",
    durationMs: 0,
    unixMs: 1730000000005,
    queueWaitMs: 0,
  }),
  JSON.stringify({
    sequence: 3,
    kind: "ActionStarted",
    subject: "storage.write",
    detail: "",
    actionId: 1,
    capability: "filesystem.write",
    resultCode: "",
    durationMs: 0,
    unixMs: 1730000000009,
    queueWaitMs: 4,
  }),
  JSON.stringify({
    sequence: 4,
    kind: "ActionFinished",
    subject: "storage.write",
    detail: "",
    actionId: 1,
    capability: "filesystem.write",
    resultCode: "none",
    durationMs: 25,
    unixMs: 1730000000034,
    queueWaitMs: 0,
  }),
  JSON.stringify({
    sequence: 5,
    kind: "ActionAccepted",
    subject: "window.move",
    detail: "",
    actionId: 2,
    capability: "windows.window.write",
    resultCode: "",
    durationMs: 0,
    unixMs: 1730000000035,
    queueWaitMs: 0,
  }),
  JSON.stringify({
    sequence: 6,
    kind: "ActionStarted",
    subject: "window.move",
    detail: "",
    actionId: 2,
    capability: "windows.window.write",
    resultCode: "",
    durationMs: 0,
    unixMs: 1730000000036,
    queueWaitMs: 1,
  }),
  JSON.stringify({
    sequence: 7,
    kind: "ActionFinished",
    subject: "window.move",
    detail: "",
    actionId: 2,
    capability: "windows.window.write",
    resultCode: "none",
    durationMs: 80,
    unixMs: 1730000000116,
    queueWaitMs: 0,
  }),
  JSON.stringify({
    sequence: 8,
    kind: "ActionAccepted",
    subject: "clipboard.write",
    detail: "",
    actionId: 3,
    capability: "windows.clipboard.write",
    resultCode: "",
    durationMs: 0,
    unixMs: 1730000000120,
    queueWaitMs: 0,
  }),
  JSON.stringify({
    sequence: 9,
    kind: "ActionRefused",
    subject: "clipboard.write",
    detail: "",
    actionId: 3,
    capability: "windows.clipboard.write",
    resultCode: "cancelled",
    durationMs: 0,
    unixMs: 1730000000125,
    queueWaitMs: 0,
  }),
  JSON.stringify({
    sequence: 10,
    kind: "ActionAccepted",
    subject: "registry.write",
    detail: "",
    actionId: 4,
    capability: "windows.registry.write",
    resultCode: "",
    durationMs: 0,
    unixMs: 1730000000130,
    queueWaitMs: 0,
  }),
  JSON.stringify({
    sequence: 11,
    kind: "ActionStarted",
    subject: "registry.write",
    detail: "",
    actionId: 4,
    capability: "windows.registry.write",
    resultCode: "",
    durationMs: 0,
    unixMs: 1730000000132,
    queueWaitMs: 2,
  }),
  JSON.stringify({
    sequence: 12,
    kind: "ActionFinished",
    subject: "registry.write",
    detail: "",
    actionId: 4,
    capability: "windows.registry.write",
    resultCode: "capability_denied",
    durationMs: 10,
    unixMs: 1730000000142,
    queueWaitMs: 0,
  }),
].join("\n");

test("parseTraceLines extracts valid TraceEntry objects and ignores noise", () => {
  const content = `${sampleLines}\nSome random log line\n{"invalid: json\n`;
  const entries = parseTraceLines(content);
  expect(entries.length).toBe(12);
  expect(entries[0].kind).toBe("StateChanged");
  expect(entries[1].subject).toBe("storage.write");
  expect(entries[1].actionId).toBe(1);
});

test("analyzeTrace calculates duration, counts, breakdown, percentiles and bottlenecks", () => {
  const entries = parseTraceLines(sampleLines);
  const analysis = analyzeTrace(entries);

  expect(analysis.entriesCount).toBe(12);
  expect(analysis.totalRunDuration).toBe(142);
  expect(analysis.breakdown.total).toBe(4);
  expect(analysis.breakdown.success).toBe(2);
  expect(analysis.breakdown.cancelled).toBe(1);
  expect(analysis.breakdown.failed).toBe(1);

  // Percentiles for durations: [25, 80, 0, 10] -> sorted: [0, 10, 25, 80]
  expect(analysis.percentiles.duration.min).toBe(0);
  expect(analysis.percentiles.duration.max).toBe(80);
  expect(analysis.percentiles.duration.p50).toBe(18); // interpolated between 10 and 25

  // Percentiles for queue wait: [4, 1, 0, 2] -> sorted: [0, 1, 2, 4]
  expect(analysis.percentiles.queueWait.min).toBe(0);
  expect(analysis.percentiles.queueWait.max).toBe(4);

  // Bottlenecks
  expect(analysis.bottlenecks.longestActions[0].subject).toBe("window.move");
  expect(analysis.bottlenecks.longestActions[0].durationMs).toBe(80);
  expect(analysis.bottlenecks.highestWait[0].subject).toBe("storage.write");
  expect(analysis.bottlenecks.highestWait[0].queueWaitMs).toBe(4);
});

test("formatReport produces ASCII waterfall timeline and diagnostic report", () => {
  const entries = parseTraceLines(sampleLines);
  const report = formatReport(entries);

  expect(report).toContain("RIME ACTION TRACE DIAGNOSTIC REPORT");
  expect(report).toContain("Run Duration:  142ms");
  expect(report).toContain("Success: 2 (50.0%)");
  expect(report).toContain("Cancelled: 1 (25.0%)");
  expect(report).toContain("Failed: 1 (25.0%)");
  expect(report).toContain("LATENCY PERCENTILES");
  expect(report).toContain("BOTTLENECK ANALYSIS");
  expect(report).toContain("SPATIOTEMPORAL WATERFALL TIMELINE");
  expect(report).toContain("storage.write");
  expect(report).toContain("filesystem.write");
  expect(report).toContain("window.move");
  expect(report).toContain("windows.window.write");
  expect(report).toContain("clipboard.write");
  expect(report).toContain("windows.clipboard.write");
  expect(report).toContain("registry.write");
  expect(report).toContain("windows.registry.write");
  expect(report).toContain("[OK]");
  expect(report).toContain("[CANCEL]");
  expect(report).toContain("[FAIL: capability_denied]");
});
