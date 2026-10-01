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

/** Bridge of the `rime:process` module. Launch and terminate are Actions. */
export interface ProcessBridge {
  list(options?: NativeActionOptions): Promise<ProcessInfo[]>;
  info(pid: ProcessId, options?: NativeActionOptions): Promise<ProcessInfo>;
  launch(options: LaunchOptions): Promise<{ pid: ProcessId }>;
  terminate(pid: ProcessId, options?: NativeActionOptions): Promise<{ pid: ProcessId }>;
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
   * Terminates through the `process.terminate` action pipeline.
   * @throws ActionError with `timeout` / `cancelled` / `capability_denied`.
   */
  terminate(pid: ProcessId, options?: ActionOptions): Promise<{ pid: ProcessId }> {
    return runAction(options, (native) =>
      processBridge().then((process) => process.terminate(pid, native)),
    );
  },
};
