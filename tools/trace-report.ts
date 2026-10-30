import { existsSync, readdirSync, readFileSync, statSync } from "node:fs";
import { resolve } from "node:path";

export interface TraceEntry {
  sequence: number;
  kind: string;
  subject: string;
  detail: string;
  actionId: number;
  capability: string;
  resultCode: string;
  durationMs: number;
  unixMs: number;
  queueWaitMs: number;
}

export interface ActionSummary {
  id: number;
  subject: string;
  capability: string;
  status: "success" | "cancelled" | "failed";
  resultCode: string;
  queueWaitMs: number;
  durationMs: number;
  acceptedUnixMs?: number;
  startedUnixMs?: number;
  finishedUnixMs?: number;
}

export interface MetricStats {
  min: number;
  max: number;
  avg: number;
  p50: number;
  p90: number;
  p99: number;
}

export interface TraceAnalysis {
  entriesCount: number;
  actions: ActionSummary[];
  totalRunDuration: number;
  minUnixMs: number;
  maxUnixMs: number;
  breakdown: {
    total: number;
    success: number;
    cancelled: number;
    failed: number;
  };
  percentiles: {
    duration: MetricStats;
    queueWait: MetricStats;
  };
  bottlenecks: {
    longestActions: ActionSummary[];
    highestWait: ActionSummary[];
  };
}

export function parseTraceLines(content: string): TraceEntry[] {
  const entries: TraceEntry[] = [];
  const lines = content.split(/\r?\n/);
  for (const line of lines) {
    const trimmed = line.trim();
    if (!trimmed) continue;
    try {
      const obj = JSON.parse(trimmed);
      if (typeof obj === "object" && obj !== null && ("kind" in obj || "sequence" in obj)) {
        entries.push({
          sequence: Number(obj.sequence ?? 0),
          kind: String(obj.kind ?? ""),
          subject: String(obj.subject ?? ""),
          detail: String(obj.detail ?? ""),
          actionId: Number(obj.actionId ?? 0),
          capability: String(obj.capability ?? ""),
          resultCode: String(obj.resultCode ?? ""),
          durationMs: Number(obj.durationMs ?? 0),
          unixMs: Number(obj.unixMs ?? 0),
          queueWaitMs: Number(obj.queueWaitMs ?? 0),
        });
      }
    } catch {
      // Skip non-json line
    }
  }
  return entries;
}

function calcStats(values: number[]): MetricStats {
  if (values.length === 0) {
    return { min: 0, max: 0, avg: 0, p50: 0, p90: 0, p99: 0 };
  }
  const sorted = [...values].sort((a, b) => a - b);
  const sum = sorted.reduce((a, b) => a + b, 0);
  const avg = Math.round((sum / sorted.length) * 10) / 10;

  const getP = (p: number) => {
    const idx = (p / 100) * (sorted.length - 1);
    const low = Math.floor(idx);
    const high = Math.ceil(idx);
    const w = idx - low;
    return Math.round(sorted[low] * (1 - w) + sorted[high] * w);
  };

  return {
    min: sorted[0],
    max: sorted[sorted.length - 1],
    avg,
    p50: getP(50),
    p90: getP(90),
    p99: getP(99),
  };
}

export function analyzeTrace(entries: TraceEntry[]): TraceAnalysis {
  const actionsMap = new Map<number, ActionSummary>();
  let minUnixMs = Infinity;
  let maxUnixMs = -Infinity;

  for (const entry of entries) {
    if (entry.unixMs > 0) {
      if (entry.unixMs < minUnixMs) minUnixMs = entry.unixMs;
      if (entry.unixMs > maxUnixMs) maxUnixMs = entry.unixMs;
    }

    if (entry.actionId > 0) {
      let act = actionsMap.get(entry.actionId);
      if (!act) {
        act = {
          id: entry.actionId,
          subject: entry.subject || "",
          capability: entry.capability || "",
          status: "success",
          resultCode: "",
          queueWaitMs: 0,
          durationMs: 0,
        };
        actionsMap.set(entry.actionId, act);
      }

      if (entry.subject && !act.subject) act.subject = entry.subject;
      if (entry.capability && !act.capability) act.capability = entry.capability;

      if (entry.kind === "ActionAccepted") {
        act.acceptedUnixMs = entry.unixMs;
      } else if (entry.kind === "ActionStarted") {
        act.startedUnixMs = entry.unixMs;
        if (entry.queueWaitMs > 0) {
          act.queueWaitMs = entry.queueWaitMs;
        } else if (act.acceptedUnixMs && act.startedUnixMs >= act.acceptedUnixMs) {
          act.queueWaitMs = act.startedUnixMs - act.acceptedUnixMs;
        }
      } else if (entry.kind === "ActionFinished") {
        act.finishedUnixMs = entry.unixMs;
        act.durationMs =
          entry.durationMs > 0
            ? entry.durationMs
            : act.startedUnixMs
              ? Math.max(0, entry.unixMs - act.startedUnixMs)
              : 0;
        act.resultCode = entry.resultCode || "none";
        if (entry.resultCode === "cancelled") {
          act.status = "cancelled";
        } else if (entry.resultCode && entry.resultCode !== "none") {
          act.status = "failed";
        } else {
          act.status = "success";
        }
      } else if (entry.kind === "ActionRefused") {
        act.finishedUnixMs = entry.unixMs;
        act.resultCode = entry.resultCode || "refused";
        act.status = entry.resultCode === "cancelled" ? "cancelled" : "failed";
      }
    }
  }

  const actions = Array.from(actionsMap.values()).sort((a, b) => a.id - b.id);
  const totalRunDuration =
    minUnixMs < Infinity && maxUnixMs > -Infinity
      ? Math.max(0, maxUnixMs - minUnixMs)
      : actions.reduce((acc, a) => acc + a.queueWaitMs + a.durationMs, 0);

  const successCount = actions.filter((a) => a.status === "success").length;
  const cancelledCount = actions.filter((a) => a.status === "cancelled").length;
  const failedCount = actions.filter((a) => a.status === "failed").length;

  const durations = actions.map((a) => a.durationMs);
  const queueWaits = actions.map((a) => a.queueWaitMs);

  return {
    entriesCount: entries.length,
    actions,
    totalRunDuration,
    minUnixMs: minUnixMs < Infinity ? minUnixMs : 0,
    maxUnixMs: maxUnixMs > -Infinity ? maxUnixMs : 0,
    breakdown: {
      total: actions.length,
      success: successCount,
      cancelled: cancelledCount,
      failed: failedCount,
    },
    percentiles: {
      duration: calcStats(durations),
      queueWait: calcStats(queueWaits),
    },
    bottlenecks: {
      longestActions: [...actions].sort((a, b) => b.durationMs - a.durationMs).slice(0, 5),
      highestWait: [...actions].sort((a, b) => b.queueWaitMs - a.queueWaitMs).slice(0, 5),
    },
  };
}

function pad(str: string, len: number, alignLeft = true): string {
  if (str.length >= len) return str.slice(0, len);
  const spaces = " ".repeat(len - str.length);
  return alignLeft ? str + spaces : spaces + str;
}

export function formatReport(entries: TraceEntry[]): string {
  const analysis = analyzeTrace(entries);
  const out: string[] = [];

  const sep = "═".repeat(102);
  const thinSep = "─".repeat(102);

  out.push(sep);
  out.push("                           RIME ACTION TRACE DIAGNOSTIC REPORT");
  out.push(sep);
  out.push("");

  const total = analysis.breakdown.total;
  const sPct = total > 0 ? ((analysis.breakdown.success / total) * 100).toFixed(1) : "0.0";
  const cPct = total > 0 ? ((analysis.breakdown.cancelled / total) * 100).toFixed(1) : "0.0";
  const fPct = total > 0 ? ((analysis.breakdown.failed / total) * 100).toFixed(1) : "0.0";

  out.push(
    `  Run Duration:  ${pad(`${analysis.totalRunDuration}ms`, 12)}  Total Actions:  ${pad(`${total}`, 10)}  Trace Entries: ${analysis.entriesCount}`,
  );
  out.push(
    `  Action Status: Success: ${analysis.breakdown.success} (${sPct}%) | Cancelled: ${analysis.breakdown.cancelled} (${cPct}%) | Failed: ${analysis.breakdown.failed} (${fPct}%)`,
  );
  out.push("");

  // Percentiles Table
  out.push(thinSep);
  out.push("  LATENCY PERCENTILES");
  out.push(thinSep);
  out.push("  Metric              p50       p90       p99       min       max       avg");
  out.push("  ─────────────────── ───────── ───────── ───────── ───────── ───────── ─────────");

  const d = analysis.percentiles.duration;
  out.push(
    `  Execution Duration  ${pad(`${d.p50}ms`, 9, false)} ${pad(`${d.p90}ms`, 9, false)} ${pad(`${d.p99}ms`, 9, false)} ${pad(`${d.min}ms`, 9, false)} ${pad(`${d.max}ms`, 9, false)} ${pad(`${d.avg}ms`, 9, false)}`,
  );

  const q = analysis.percentiles.queueWait;
  out.push(
    `  Queue Wait          ${pad(`${q.p50}ms`, 9, false)} ${pad(`${q.p90}ms`, 9, false)} ${pad(`${q.p99}ms`, 9, false)} ${pad(`${q.min}ms`, 9, false)} ${pad(`${q.max}ms`, 9, false)} ${pad(`${q.avg}ms`, 9, false)}`,
  );
  out.push("");

  // Bottleneck Analysis
  out.push(thinSep);
  out.push("  BOTTLENECK ANALYSIS");
  out.push(thinSep);

  out.push("  Top Longest Executions:");
  if (analysis.bottlenecks.longestActions.length === 0) {
    out.push("    (none)");
  } else {
    for (const act of analysis.bottlenecks.longestActions) {
      const cap = act.capability ? ` (${act.capability})` : "";
      const statusLabel =
        act.status === "success"
          ? "[SUCCESS]"
          : act.status === "cancelled"
            ? "[CANCELLED]"
            : `[FAILED: ${act.resultCode}]`;
      out.push(
        `    #${pad(String(act.id), 3)} ${pad(act.subject, 18)}${pad(cap, 28)} ${pad(`${act.durationMs}ms`, 7, false)} ${statusLabel}`,
      );
    }
  }

  out.push("");
  out.push("  Top Queue Wait Latencies:");
  if (analysis.bottlenecks.highestWait.length === 0) {
    out.push("    (none)");
  } else {
    for (const act of analysis.bottlenecks.highestWait) {
      out.push(
        `    #${pad(String(act.id), 3)} ${pad(act.subject, 18)} wait: ${pad(`${act.queueWaitMs}ms`, 6, false)}`,
      );
    }
  }
  out.push("");

  // ASCII Waterfall Timeline
  out.push(thinSep);
  out.push("  SPATIOTEMPORAL WATERFALL TIMELINE (░: Queue Wait, █: Executing, ~: Cancelled, ✖: Failed)");
  out.push(thinSep);

  if (analysis.actions.length === 0) {
    out.push("  No actions recorded in trace.");
  } else {
    const barWidth = 40;
    const t0 = analysis.minUnixMs;
    const totalSpan = Math.max(
      1,
      analysis.totalRunDuration,
      analysis.actions.reduce((max, a) => Math.max(max, a.queueWaitMs + a.durationMs), 1),
    );

    out.push(
      `  ID   Subject              Capability Checked       Wait    Exec  Timeline (0ms -> ${totalSpan}ms)`,
    );
    out.push(
      `  ───  ───────────────────  ───────────────────────  ─────  ─────  ${"─".repeat(barWidth)}  ──────────`,
    );

    for (const act of analysis.actions) {
      // Calculate start and end offsets relative to timeline base
      let waitStartMs = 0;
      let waitEndMs = 0;
      let execStartMs = 0;
      let execEndMs = 0;

      if (act.acceptedUnixMs !== undefined && t0 > 0) {
        waitStartMs = Math.max(0, act.acceptedUnixMs - t0);
        waitEndMs =
          act.startedUnixMs !== undefined
            ? Math.max(waitStartMs, act.startedUnixMs - t0)
            : waitStartMs + act.queueWaitMs;
      } else if (act.startedUnixMs !== undefined && t0 > 0) {
        waitStartMs = Math.max(0, act.startedUnixMs - t0 - act.queueWaitMs);
        waitEndMs = Math.max(waitStartMs, act.startedUnixMs - t0);
      } else {
        waitStartMs = 0;
        waitEndMs = act.queueWaitMs;
      }

      execStartMs = waitEndMs;
      if (act.finishedUnixMs !== undefined && t0 > 0) {
        execEndMs = Math.max(execStartMs, act.finishedUnixMs - t0);
      } else {
        execEndMs = execStartMs + act.durationMs;
      }

      const toCol = (ms: number) =>
        Math.min(barWidth - 1, Math.max(0, Math.floor((ms / totalSpan) * barWidth)));

      const qStart = toCol(waitStartMs);
      const qEnd = toCol(waitEndMs);
      const eStart = toCol(execStartMs);
      const eEnd = toCol(execEndMs);

      const bar = new Array<string>(barWidth).fill(" ");

      if (act.queueWaitMs > 0) {
        for (let i = qStart; i <= Math.max(qStart, qEnd - 1); i++) {
          bar[i] = "░";
        }
      }

      const execChar =
        act.status === "success" ? "█" : act.status === "cancelled" ? "~" : "✖";

      for (let i = eStart; i <= eEnd; i++) {
        bar[i] = execChar;
      }
      if (eStart === eEnd && act.durationMs === 0) {
        bar[eStart] = execChar;
      }

      const statusTag =
        act.status === "success"
          ? "[OK]"
          : act.status === "cancelled"
            ? "[CANCEL]"
            : `[FAIL: ${act.resultCode}]`;

      const idStr = pad(`#${act.id}`, 4);
      const subjStr = pad(act.subject, 19);
      const capStr = pad(act.capability || "(none)", 23);
      const waitStr = pad(`${act.queueWaitMs}ms`, 6, false);
      const execStr = pad(`${act.durationMs}ms`, 6, false);
      const barStr = bar.join("");

      out.push(`  ${idStr} ${subjStr}  ${capStr}  ${waitStr} ${execStr}  ${barStr}  ${statusTag}`);
    }
  }

  out.push(sep);
  return out.join("\n");
}

export function runReport(contentOrEntries: string | TraceEntry[]): void {
  const entries =
    typeof contentOrEntries === "string" ? parseTraceLines(contentOrEntries) : contentOrEntries;
  console.log(formatReport(entries));
}

function findLatestTraceFile(runsDir: string): string | null {
  if (!existsSync(runsDir)) return null;
  const files = readdirSync(runsDir)
    .filter((f) => f.endsWith(".trace.jsonl"))
    .map((f) => {
      const full = resolve(runsDir, f);
      return { path: full, mtime: statSync(full).mtimeMs };
    })
    .sort((a, b) => b.mtime - a.mtime);
  return files.length > 0 ? files[0].path : null;
}

if (import.meta.main) {
  const args = process.argv.slice(2);
  let content = "";
  const runsDir = resolve(process.cwd(), "build/ts/runs");

  if (args.length > 0 && args[0] !== "-" && args[0] !== "--stdin") {
    if (existsSync(args[0])) {
      content = readFileSync(args[0], "utf-8");
    } else {
      console.error(`Error: Trace file not found: ${args[0]}`);
      process.exit(1);
    }
  } else if (args[0] === "-" || args[0] === "--stdin") {
    const chunks: Buffer[] = [];
    for await (const chunk of process.stdin) {
      chunks.push(chunk as Buffer);
    }
    content = Buffer.concat(chunks).toString("utf-8");
  } else {
    // Default: find latest trace file in build/ts/runs
    const latest = findLatestTraceFile(runsDir);
    if (latest && existsSync(latest)) {
      console.log(`[Trace Report] Reading latest trace: ${latest}\n`);
      content = readFileSync(latest, "utf-8");
    } else {
      // Fallback to stdin if available
      try {
        const chunks: Buffer[] = [];
        for await (const chunk of process.stdin) {
          chunks.push(chunk as Buffer);
        }
        content = Buffer.concat(chunks).toString("utf-8");
      } catch {
        content = "";
      }
    }
  }

  const entries = parseTraceLines(content);
  if (entries.length === 0) {
    console.error("Warning: No valid TraceEntry lines found in trace input.");
    process.exit(0);
  }
  console.log(formatReport(entries));
}
