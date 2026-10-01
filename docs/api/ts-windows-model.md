# TS Windows Runtime Model

Status: design supplement. This file is the compact implementation model for the five core questions. It does not claim that the current Runtime has implemented these APIs.

## Stable TypeScript surface

```ts
type OpaqueId<K extends string> = string & { readonly __kind: K };
type WindowId = OpaqueId<"window">;
type ControlId = OpaqueId<"control">;
type FileId = OpaqueId<"file">;
type SubscriptionId = OpaqueId<"subscription">;

interface ActionOptions {
  signal?: AbortSignal;
  deadlineMs?: number;
  parentActionId?: string;
  idempotencyKey?: string;
}

interface Subscription {
  readonly id: SubscriptionId;
  readonly closed: boolean;
  close(options?: ActionOptions): Promise<void>;
}

interface WindowRef {
  readonly id: WindowId;
  snapshot(options?: ActionOptions): Promise<Readonly<WindowSnapshot>>;
  move(rect: Rect, options?: ActionOptions): Promise<void>;
  activate(options?: ActionOptions): Promise<void>;
}
```

Native pointers never enter this surface. Resource references are revocable capabilities: every operation revalidates the ID, generation and owner lane. A destroyed resource produces `TargetGone`.

## Object member policy

| AHK family | TS representation | Policy |
|---|---|---|
| Object/Array/Map/Func/regex/string/date/math | Native TS/ECMAScript | Compatibility wrappers only; no Win32 dependency |
| Gui/GuiControl/Menu/InputHook/File/ComObject | Typed facade over opaque ref | Methods are lane-bound, cancellable and closeable |
| OnEvent/OnMessage/InputHook callbacks | `Subscription` | Scheduler-owned; close drains callbacks before release |
| Ptr/Handle/Hwnd/COM pointer | No standard type | Typed plugin token only, with capability and ABI version |

`objects.json` is an inventory, not an API. Each member becomes an `apiId` with a TS signature, source definition, lane, async decision, ownership rule, error union, capability and `testId` before implementation.

## Built-in variables

AHK globals become explicit values:

| AHK category | TS form |
|---|---|
| Host constants and paths | `runtime.info(): Promise<RuntimeInfo>` or immutable startup value |
| Dynamic system state | `windows.active()`, `input.state()`, `clipboard.readText()` |
| Event-local values (`A_ThisHotkey`, `A_EventInfo`, `A_GuiControl`) | `EventContext` argument, valid only during callback |
| Loop-local values (`A_Index`, `A_LoopFile*`) | iterator result or loop context, never global mutable state |
| Runtime state (`A_IsPaused`, `A_IsCritical`, hotkey limits) | read-only `scheduler.state()` snapshot |

The compatibility layer may expose `ahkCompat.current`, but it is read-only, callback-scoped and cannot be retained across tasks. `builtins.json` must gain type, dynamic/static classification, update point, lane, capability, TS replacement and test ID for every variable.

## Window implementation path

```text
TS windows API
 -> QuickJS rime:window binding
 -> serialized request {apiId, target/query, options}
 -> JS scheduler
 -> UI lane / WindowService::UiThread::call
 -> WindowRegistry (WindowId + generation <-> HWND, UI thread only)
 -> EnumWindows/GetForegroundWindow/GetWindowTextW/GetWindowRect/
    SetWindowPos/ShowWindow/SetForegroundWindow
 -> immutable snapshot or ActionResult
 -> completion queue -> QuickJS Promise
```

The identity must include service instance and generation, so an ID cannot address a newly created HWND after destruction. A snapshot should carry process ID, session ID, desktop, monitor, DPI and coordinate space. Control identity uses UIA runtime ID plus provider generation; it is not an HWND alias.

## Async decision

Synchronous: pure parsers, option validation, snapshot field access, serialization, and TS-owned string/date/regex/math operations.

Asynchronous: every UI, UIA, COM, clipboard, process, file, drive, dialog, capture, image search, wait, Hook, Timer and injected-input operation. Even a fast native read is a Promise at the JS boundary when it crosses a lane.

Every asynchronous method accepts cancellation and deadline. Wait operations distinguish `Timeout`, `Cancelled`, `QueueClosed`, `TargetGone` and `PermissionDenied`. Subscription callbacks are delivered through the scheduler, never directly from Hook or COM threads; backpressure and callback exceptions are recorded in Trace.

## Never expose

The standard library does not expose HWND/HANDLE/HHOOK/HMENU/HGLOBAL, thread IDs, window-procedure addresses, COM pointers, VARIANT addresses, QuickJS pointers, internal queues, `AttachThreadInput`, arbitrary `DllCall`/`ComCall`, or unaudited `SendMessage`. The replacements are branded refs, immutable snapshots, typed UIA patterns, structured Actions and isolated plugin tokens.

## Completion gate

The documentation is architecture-complete only when every function, object member and builtin variable has an `apiId`, TS signature, source definition, lane, async decision, capability, error union, cancellation point, ownership rule, Trace fields and `testId`. The current repository has the inventory and principles, but not this complete per-entry contract.
