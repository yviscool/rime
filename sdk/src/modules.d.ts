declare module "rime:runtime" {
  const runtime: import("./index").RuntimeBridge;
  export { runtime };
}

declare module "rime:window" {
  const windows: import("./index").WindowsBridge;
  const settings: { window: import("./index").WindowSettingsBridge };
  const groups: import("./index").WindowsGroupsBridge;
  export { groups, settings, windows };
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

declare module "rime:screen" {
  const screen: import("./screen").ScreenBridge;
  export { screen };
}

declare module "rime:automation" {
  const automation: import("./automation").AutomationBridge;
  export { automation };
}

declare module "rime:storage" {
  const storage: import("./storage").StorageBridge;
  export { storage };
}

declare module "rime:registry" {
  const registry: import("./registry").RegistryBridge;
  export { registry };
}
