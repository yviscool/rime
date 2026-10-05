# Action Contract Golden Files

A minimal four-layer golden set for 15 action types. Every file is a
machine-readable contract sample for exactly one action type; two independent
consumers must agree on every file:

- `bun tools/golden-smoke.ts` (TypeScript, runs inside `bun run contract:smoke`)
- `rime_golden_tests` (C++, ctest name `rime_golden_contract`)
- `rime_golden_exec_tests` (C++, ctest name `rime_golden_exec`) - executes the
  `failure` and `semantic` layers through the real desktop executors

Golden files and this README are ASCII-only English.

## File layout

```text
contracts/golden/<type>.json
```

`<type>` is the registry action type verbatim, including dots:
`window.move.json`, `input.send.json`, `automation.invoke.json`.

The 15 covered actions:

```text
window.move          window.focus          window.close
window.kill          window.zorder         window.set.title
window.set.transparent                      window.set.region
window.group.add     input.send            input.mouse
process.launch       process.terminate     clipboard.write
automation.invoke
```

## Top-level fields

| field          | type             | meaning |
| -------------- | ---------------- | ------- |
| `type`         | string           | action type; must equal the file name stem and the matching entry in `contracts/registry/actions.json` |
| `capability`   | string           | capability the action requires; must equal the registry entry |
| `payloadSchema`| string \| null   | `contracts/schema/<file>.json#/$defs/<name>`; `null` exactly when the registry says `inline` |
| `shape`        | object           | schema-layer payload validity (`valid`, `violations`) |
| `semantic`     | array            | expected effects, executed by `rime_golden_exec` against the desktop |
| `lifecycle`    | array            | results the Action Kernel decides before any executor runs |
| `failure`      | array            | executor-layer rejections |
| `envelope`     | array            | ActionV1 envelope examples the codec must reject |

Both consumers cross-check `type`, `capability` and `payloadSchema` against
`contracts/registry/actions.json` (read-only). The registry stays the single
source of truth; a golden file that disagrees fails the smoke test.

## `shape` - schema-layer payload validity

```json
"shape": {
  "valid":     [{ "name": "...", "payload": { } }],
  "violations":[{ "name": "...", "payload": { }, "code": "invalid_contract" }]
}
```

- When `payloadSchema` is a non-null `$def` reference, every `valid` payload
  must validate against that `$def` and every `violations` payload must not
  (with `code` = `invalid_contract`). Validation uses the JSON Schema
  Draft 2020-12 subset implemented by `tools/schema-smoke.ts` and mirrored by
  the C++ test: `type`, `const`, `enum`, `oneOf`, `anyOf`, `required`,
  `properties`, `additionalProperties`, `items`, `$ref` -> `$defs`,
  `minLength`, `minimum`, `maximum`. Note the subset only reports
  `additionalProperties` for schemas that also declare `properties`
  (`emptyPayload` does not, so its violations are non-object payloads).
- When `payloadSchema` is `null` (registry value `inline`), the two lists
  declare the *executor* validator that will be wired in R4-B; this round only
  checks structure. `valid` payloads are objects, except `input.send`, whose
  executor contract is a JSON array of key steps
  (`engine/win32/src/input_executor.cpp:42`) - the ActionV1 envelope requires
  an object payload, so `input.send` is produced in-process only and its
  envelope-shaped rejections live in `failure`.
- `shape.valid` payloads must be objects (or the `input.send` array).
  `shape.violations` payloads may be any non-null JSON value, because a
  violation is precisely a payload that violates the declared shape.

## `semantic` - expected effects

```json
"semantic": [{ "name": "...", "payload": { }, "check": { "kind": "...", "expect": "..." } }]
```

Round-1 `check.kind` whitelist, executed end to end by the `rime_golden_exec`
desktop test:

| kind             | `expect` |
| ---------------- | -------- |
| `window.rect`    | one of `work-left-half`, `work-right-half`, `work-full`, `work-top`, `work-bottom` |
| `process.exists` | `true` |
| `clipboard.text` | a string, compared with the text the action wrote |

How `rime_golden_exec` executes each kind:

- `window.rect` - creates a fixture window, runs the payload, and compares the
  window rectangle with the work-area placement for that position name.
- `process.exists` - runs the payload verbatim, then checks the launched pid.
  `ProcessService::launch` passes the bare `command` to `CreateProcessW` as
  `lpApplicationName`, which Win32 resolves against the current directory only
  (no PATH search), so the launch runs with the Windows system directory as the
  current directory and the previous directory is restored immediately
  afterwards; the golden payload itself is never rewritten. The whole fixture
  process tree is terminated before the assertion and before the test ends.
- `clipboard.text` - writes the payload, reads the text back, then restores the
  clipboard captured before the case.

An empty `semantic` array means no whitelisted kind can express the expected
effect for that action. It is empty here for `window.focus`, `window.close`,
`window.kill` (foreground/z-order/destruction, not a rect), `window.zorder`,
`window.set.title`, `window.set.transparent`, `window.set.region`,
`window.group.add` (state outside the whitelist), `input.send`,
`input.mouse` (injected input has no whitelisted observation), and
`process.terminate` (needs `process.exists == false`, which the whitelist does
not offer yet), `automation.invoke` (element invocation, no whitelisted kind).

## `lifecycle` - results decided before any executor runs

```json
"lifecycle": [
  { "name": "...", "deadlineMode": "past",
    "action": { "schemaVersion": 1, "id": 1, "source": { "kind": "user", "id": "golden" },
                "type": "...", "capability": "...", "target": { "kind": "...", "id": "..." },
                "preconditions": [], "deadlineUnixMs": 1893456000000,
                "parentActionId": null, "payload": { }, "idempotencyKey": "..." },
    "expect": { "code": "timeout" } }
]
```

- Only pre-executor outcomes are allowed: `timeout`, `capability_denied`,
  `unsupported`. The envelope-breaking sample lives in the top-level
  `envelope` field and must fail with `invalid_contract`.
- `deadlineMode` is `past` or `future`. The `deadlineUnixMs` inside `action`
  is the fixed schema example value `1893456000000`; the C++ runner rewrites
  it from its `ManualClock` (`clock.unix_ms() - 1` for `past`,
  `clock.unix_ms() + 60000` for `future`) before `Kernel::execute`, so the
  deadline judgement is deterministic. The TypeScript consumer only validates
  the envelope against `contracts/schema/action-v1.schema.json`.
- `expired-deadline` (`past`) expects `timeout`: the kernel checks the
  deadline before the capability policy.
- `capability-mismatch` (`future`) expects `capability_denied`: `action.capability`
  is deliberately different from the golden top-level `capability` (which stays
  the real capability of the action type). The C++ runner grants only the
  top-level capability for this case, so the mismatch is denied.
- `precondition-not-evaluated` (`future`) expects `unsupported`: the kernel
  rejects any action that declares a precondition, before deadline and
  capability checks.
- Every file carries all three cases, so each file has at least two.

## `failure` - executor-layer rejections

```json
"failure": [{ "name": "...", "payload": { }, "code": "invalid_contract",
              "messageContains": "optional", "target": { "kind": "...", "id": "..." } }]
```

- Real inputs the executor rejects. `payload` is always an object.
- `rime_golden_exec` runs every entry through the real Action Kernel and
  executor with all six capabilities granted, and additionally proves the
  rejection happens before any side effect: the window service never starts,
  no process is launched, no input is injected, and the clipboard is byte for
  byte unchanged.
- `messageContains` is written only where the executor source has a stable
  literal (`engine/win32/src/window_executor.cpp`,
  `engine/win32/src/input_executor.cpp`, `engine/win32/src/process_executor.cpp`,
  `engine/win32/src/clipboard_executor.cpp`, `engine/automation/src/uia_executor.cpp`).
- `target` is an optional field, present only when the executor rejects the
  envelope target before it ever reads the payload
  (`window.focus`/`window.close`/`window.kill` target kind,
  `process.terminate` target id, `clipboard.write` target id,
  `automation.invoke` target kind, `input.send` target kind). In those cases
  `payload` is the empty object the executor would otherwise accept.

## `envelope` - rejected ActionV1 samples

```json
"envelope": [{ "name": "missing-target", "action": { ... }, "expect": { "code": "invalid_contract" } }]
```

Each sample is a complete ActionV1 JSON object missing a required field. The
TypeScript consumer proves `contracts/schema/action-v1.schema.json` rejects it;
the C++ consumer proves `rime::action::decode_action` fails with
`invalid_contract`.

## Error codes

Every `code` in `shape.violations`, `lifecycle`, `failure` and `envelope`
comes from the contract error set shared by
`contracts/generated/contracts.ts` and `engine/core/src/error_names.cpp`:

```text
none  invalid_state  queue_closed  queue_full  cancelled  timeout
capability_denied  invalid_contract  unsupported  execution_failed  target_gone
```

## Running

```text
bun tools/golden-smoke.ts                     # TypeScript consumer
bun run contract:smoke                        # schema smoke + golden smoke
ctest --test-dir build/quickjs -R rime_golden  # C++ consumer
```
