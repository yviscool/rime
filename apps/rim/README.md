# Rim

Rim is the first application built on the Rime Runtime. It may depend on `@rime/sdk` and public host contracts, but runtime and engine layers must never depend on Rim.

The application source is TypeScript. `bun run ts:build` emits `build/ts/rim.js` as an ES module; the resulting JavaScript is loaded by the QuickJS-ng Host in `bun run ts:quickjs`. Runtime APIs are imported from the stable `rime:runtime` module through `@rime/sdk`.

`main()` exercises the implemented SDK surface inside that host: `runtime.ping()`, `Window.list()`, `Process.list()`, `clipboard.read()` and an `input.subscribe` shape check. The calls cross SDK -> native module -> Win32 service; the `rime_js_bundle` harness settles the host afterwards and fails the run when `main()` rejects (reported through `globalThis.__rim_failure`). Mutating paths (`Window.move`, `Process.launch`, `clipboard.write`, input hooks) are covered by the native and slice tests instead, so the bundle run never moves windows, spawns processes or writes the clipboard.
