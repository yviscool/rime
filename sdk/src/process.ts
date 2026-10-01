export interface ProcessInfo {
  pid: number;
  parentPid: number;
  /** Executable file name, e.g. "notepad.exe". */
  name: string;
  /** Full image path; empty when the process cannot be queried. */
  exePath: string;
}

export interface LaunchOptions {
  command: string;
  args?: string;
  workingDir?: string;
}

/** Bridge of the `rime:process` module. Launch and terminate are Actions. */
export interface ProcessBridge {
  list(): Promise<ProcessInfo[]>;
  info(pid: number): Promise<ProcessInfo>;
  launch(options: LaunchOptions): Promise<{ pid: number }>;
  terminate(pid: number): Promise<{ pid: number }>;
}

async function processBridge(): Promise<ProcessBridge> {
  const module = await import("rime:process");
  return module.process;
}

export const Process = {
  /** Snapshot of the running processes. */
  list(): Promise<ProcessInfo[]> {
    return processBridge().then((process) => process.list());
  },
  info(pid: number): Promise<ProcessInfo> {
    return processBridge().then((process) => process.info(pid));
  },
  /** Starts a process through the `process.launch` action pipeline. */
  launch(options: LaunchOptions): Promise<{ pid: number }> {
    return processBridge().then((process) => process.launch(options));
  },
  /** Terminates through the `process.terminate` action pipeline. */
  terminate(pid: number): Promise<{ pid: number }> {
    return processBridge().then((process) => process.terminate(pid));
  },
};
