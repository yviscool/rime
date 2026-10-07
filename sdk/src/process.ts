import { runAction, type ActionOptions, type NativeActionOptions } from "./action";
import type { Brand } from "./window";

/** Stable process id issued by the OS (never a raw HANDLE). */
export type ProcessId = Brand<number, "ProcessId">;

export interface ProcessInfo {
  pid: ProcessId;
  parentPid: ProcessId;
  /** Executable file name, e.g. "notepad.exe". */
  name: string;
  /** Full image path; empty when the process cannot be queried. */
  exePath: string;
}

/** Wire shape: launch fields plus the shared action options. */
export interface LaunchOptions extends NativeActionOptions {
  command: string;
  args?: string;
  workingDir?: string;
}

/** SDK shape: launch fields plus AbortSignal-style options. */
export type ProcessLaunchRequest = ActionOptions & {
  command: string;
  args?: string;
  workingDir?: string;
};

/** One process priority (AHK ProcessSetPriority letters map to these names). */
export type ProcessPriority =
  | "idle"
  | "belowNormal"
  | "normal"
  | "aboveNormal"
  | "high"
  | "realtime";

/** One shutdown request (AHK Shutdown codes). Never exercised by tests. */
export interface ShutdownPayload {
  mode: "logoff" | "shutdown" | "reboot" | "poweroff" | "hibernate";
  force?: boolean;
  /** Grace period in seconds, 0..600 (AHK Shutdown timeout). */
  timeoutSec?: number;
}

/** Credentials for `runAs` (AHK RunAs). Never logged, traced or echoed back. */
export interface RunAsRequest {
  user: string;
  password: string;
  executable: string;
  domain?: string;
  arguments?: string;
  workingDir?: string;
}

/** Wire shape of `runAs`: the fields plus the shared action options. */
export type RunAsOptions = RunAsRequest & NativeActionOptions;

/** Bridge of the `rime:process` module. Launch and terminate are Actions. */
export interface ProcessBridge {
  list(options?: NativeActionOptions): Promise<ProcessInfo[]>;
  info(pid: ProcessId, options?: NativeActionOptions): Promise<ProcessInfo>;
  launch(options: LaunchOptions): Promise<{ pid: ProcessId }>;
  /** Opens a file for editing (shell "edit" verb, notepad fallback). */
  edit(path: string, options?: NativeActionOptions): Promise<{ path: string }>;
  terminate(pid: ProcessId, options?: NativeActionOptions): Promise<{ pid: ProcessId }>;
  /** Resolves `{pid}` once the process exists; `timeout` after deadlineMs (default 5000). */
  wait(pid: ProcessId, options?: NativeActionOptions): Promise<{ pid: ProcessId }>;
  /** Resolves once the process is gone: `{pid}` on the snapshot path, `{exitCode}` on the handle path. */
  waitClose(pid: ProcessId, options?: NativeActionOptions): Promise<{
    pid?: ProcessId;
    exitCode?: number;
  }>;
  /** Launches and waits on the worker lane; resolves `{pid, exitCode}` (no Action Trace). */
  runWait(
    request: LaunchOptions,
    options?: NativeActionOptions,
  ): Promise<{ pid: ProcessId; exitCode: number }>;
  /** `process.set.priority` action; resolves `{pid}`. */
  setPriority(
    pid: ProcessId,
    priority: ProcessPriority,
    options?: NativeActionOptions,
  ): Promise<{ pid: ProcessId }>;
  /** `process.runas` action; resolves `{pid}`. The password travels only in the payload. */
  runAs(request: RunAsRequest, options?: NativeActionOptions): Promise<{ pid: ProcessId }>;
  /** `process.shutdown` action; resolves `{mode}`. */
  shutdown(payload: ShutdownPayload, options?: NativeActionOptions): Promise<{ mode: string }>;
}

async function processBridge(): Promise<ProcessBridge> {
  const module = await import("rime:process");
  return module.process;
}

export const Process = {
  /**
   * Snapshot of the running processes.
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  list(options?: ActionOptions): Promise<ProcessInfo[]> {
    return runAction(options, (native) =>
      processBridge().then((process) => process.list(native)),
    );
  },
  /**
   * Reads one process snapshot by pid.
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  info(pid: ProcessId, options?: ActionOptions): Promise<ProcessInfo> {
    return runAction(options, (native) =>
      processBridge().then((process) => process.info(pid, native)),
    );
  },
  /**
   * Starts a process through the `process.launch` action pipeline.
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  launch(request: ProcessLaunchRequest): Promise<{ pid: ProcessId }> {
    const { signal: _signal, ...fields } = request;
    return runAction(request, (native) =>
      processBridge().then((process) => process.launch({ ...fields, ...native })),
    );
  },
  /**
   * Opens a file for editing through the `process.edit` action pipeline.
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  edit(path: string, options?: ActionOptions): Promise<{ path: string }> {
    return runAction(options, (native) =>
      processBridge().then((process) => process.edit(path, native)),
    );
  },
  /**
   * Terminates through the `process.terminate` action pipeline.
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  terminate(pid: ProcessId, options?: ActionOptions): Promise<{ pid: ProcessId }> {
    return runAction(options, (native) =>
      processBridge().then((process) => process.terminate(pid, native)),
    );
  },
  /**
   * Waits until the process exists (immediately when it already does).
   * Capability `process.inspect`; no Action Trace (worker-lane slices).
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  wait(pid: ProcessId, options?: ActionOptions): Promise<{ pid: ProcessId }> {
    return runAction(options, (native) =>
      processBridge().then((process) => process.wait(pid, native)),
    );
  },
  /**
   * Waits until the process is gone. Capability `process.inspect`.
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  waitClose(pid: ProcessId, options?: ActionOptions): Promise<{ pid?: ProcessId; exitCode?: number }> {
    return runAction(options, (native) =>
      processBridge().then((process) => process.waitClose(pid, native)),
    );
  },
  /**
   * Launches and waits for exit in one call (AHK `RunWait`).
   * Capability `process.launch`; no Action Trace (worker-lane slices).
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  runWait(request: ProcessLaunchRequest): Promise<{ pid: ProcessId; exitCode: number }> {
    const { signal: _signal, ...fields } = request;
    return runAction(request, (native) =>
      processBridge().then((process) => process.runWait(fields, native)),
    );
  },
  /**
   * Changes one process priority through the `process.set.priority` action
   * pipeline (capability `process.manage`).
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied` /
   *   `invalid_contract`.
   */
  setPriority(pid: ProcessId, priority: ProcessPriority, options?: ActionOptions): Promise<{ pid: ProcessId }> {
    return runAction(options, (native) =>
      processBridge().then((process) => process.setPriority(pid, priority, native)),
    );
  },
  /**
   * Starts a process under another account through the `process.runas` action
   * pipeline (capability `process.runas`, AHK `RunAs`). The password is
   * required by AHK semantics; it never enters the trace or an error message.
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied` /
   *   `invalid_contract`.
   */
  runAs(request: RunAsRequest, options?: ActionOptions): Promise<{ pid: ProcessId }> {
    return runAction(options, (native) =>
      processBridge().then((process) => process.runAs(request, native)),
    );
  },
  /**
   * Requests a system shutdown through the `process.shutdown` action pipeline
   * (capability `process.shutdown`, AHK `Shutdown`). The success path is never
   * exercised by tests by policy - only validation and rejection are.
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied` /
   *   `invalid_contract`.
   */
  shutdown(payload: ShutdownPayload, options?: ActionOptions): Promise<{ mode: string }> {
    return runAction(options, (native) =>
      processBridge().then((process) => process.shutdown(payload, native)),
    );
  },
};
