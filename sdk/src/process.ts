import { runAction, type ActionOptions, type NativeActionOptions } from "./action";

export interface ProcessInfo {
  pid: number;
  parentPid: number;
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
  info(pid: number, options?: NativeActionOptions): Promise<ProcessInfo>;
  launch(options: LaunchOptions): Promise<{ pid: number }>;
  terminate(pid: number, options?: NativeActionOptions): Promise<{ pid: number }>;
}

async function processBridge(): Promise<ProcessBridge> {
  const module = await import("rime:process");
  return module.process;
}

export const Process = {
  /** Snapshot of the running processes. */
  list(options?: ActionOptions): Promise<ProcessInfo[]> {
    return runAction(options, (native) =>
      processBridge().then((process) => process.list(native)),
    );
  },
  info(pid: number, options?: ActionOptions): Promise<ProcessInfo> {
    return runAction(options, (native) =>
      processBridge().then((process) => process.info(pid, native)),
    );
  },
  /** Starts a process through the `process.launch` action pipeline. */
  launch(request: ProcessLaunchRequest): Promise<{ pid: number }> {
    const { signal: _signal, ...fields } = request;
    return runAction(request, (native) =>
      processBridge().then((process) => process.launch({ ...fields, ...native })),
    );
  },
  /** Terminates through the `process.terminate` action pipeline. */
  terminate(pid: number, options?: ActionOptions): Promise<{ pid: number }> {
    return runAction(options, (native) =>
      processBridge().then((process) => process.terminate(pid, native)),
    );
  },
};
