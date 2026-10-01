# Repository Structure

The repository follows the PocketJS-style split between public contracts, platform-neutral engine code, host adapters, SDK/tooling, applications and verification:

```text
contracts/  wire schemas, semantic specs and generated declarations
engine/     native core, action kernel, QuickJS host and Win32 layers
hosts/      OS and process adapters; desktop is the first host
sdk/        public TypeScript API and build-time types
apps/       Rim and future first-party applications
plugins/    versioned plugin SDK and implementations
tools/      Bun build, check, test and diagnostics commands
tests/      contract, native, host, replay and integration tests
docs/       architecture and operational records
site/       generated documentation/playground surface
```

The dependency direction is `contracts -> engine -> hosts -> apps`; `sdk` and `tools` may consume contracts, while engine code must not depend on applications. `engine/win32` and `engine/win32/js` carry the Win32 services (`window`, `input`, `process`, `clipboard`) and their `rime:*` native modules; `engine/js` owns the QuickJS host, module loader and ABI.

The root `package.json`, `bunfig.toml`, `tsconfig.json` and `rime.config.ts` (single TS-first source of truth; legacy `rime.json` removed) are the workspace/toolchain boundary. Native compilation remains CMake-owned inside `engine/` and each host; Bun orchestrates checks, packaging and developer commands.

QuickJS-ng is pinned to `v0.17.0` (`6d46d07d04041b40f4f49eaa7fdebe44c314c699`) and is enabled with the `quickjs` CMake preset. The default `dev` preset keeps network-fetched engine dependencies disabled so core validation remains offline and repeatable.

`bun run quickjs:test` builds the QuickJS integration suite (`engine/js`, `engine/win32/js`, `tests/js`) in `build/quickjs`; the first run fetches the pinned QuickJS-ng source over the network. `bun run test` runs this suite together with the contract, TypeScript and MSVC checks. Slice/JS tests require `--preset quickjs` (msvc/dev presets keep QuickJS disabled).

TypeScript applications are built with `bun run ts:build`. The output remains an ES module and imports the native `rime:runtime` module; it is not executed by Bun. `bun run ts:quickjs` builds the QuickJS preset and hands the Rim bundle to `rime_js_bundle`, which registers the `rime:*` native modules the SDK imports (`runtime`, `window`, `input`, `process`, `clipboard`), runs the bundle on a dedicated JS Thread in `rime::js::Runtime`, settles pending tasks, surfaces async failures through `globalThis.__rim_failure`, and verifies deterministic shutdown. `bun run test` includes this path.

`rime::js::Host` owns one QuickJS context and may only be used on its owner thread. `rime::js::Runtime` owns the JS Thread and serializes module evaluation tasks to that Host. Native modules must be registered through the loader and exposed through stable module IDs; application code must not depend on a global runtime object.
