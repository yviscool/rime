declare module "rime:runtime" {
  const runtime: import("./index").RuntimeBridge;
  export { runtime };
}

declare module "rime:window" {
  const windows: import("./index").WindowsBridge;
  export { windows };
}

declare module "rime:input" {
  const input: import("./input").InputBridge;
  export { input };
}

declare module "rime:process" {
  const process: import("./process").ProcessBridge;
  export { process };
}

declare module "rime:clipboard" {
  const clipboard: import("./clipboard").ClipboardBridge;
  export { clipboard };
}

declare module "rime:automation" {
  const automation: import("./automation").AutomationBridge;
  export { automation };
}
