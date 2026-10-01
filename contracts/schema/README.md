# Runtime Contracts

This directory owns versioned wire and data contracts shared by the native runtime, SDK, plugins and tests. JSON Schema Draft 2020-12 is the source of truth for serialized contracts.

`action-v1.schema.json` and `result-v1.schema.json` define the first Action request/response boundary. IDs are bounded by JavaScript's safe integer range so the same values remain exact in QuickJS and TypeScript. Deadlines are absolute Unix milliseconds; the host converts them to its monotonic clock when accepting an action. Result failures use the shared `Code` enum, which includes `timeout` for deadline expiry and UI-queue timeouts.

`window-v1.schema.json` defines the Window service surface: `WindowQuery` (`title`/`matchMode`/`ahkClass`/`ahkExe`/`ahkId`/`includeHidden`/`active`), `WindowSnapshot`, and the payload contracts for `move`/`focus`/`close`/`hide`/`show`/`minimize`/`maximize`/`restore`. `examples/` holds three window fixtures: `window-v1.json` (query plus response), `window-v1-snapshot.json` and `window-v1-move-payload.json`.

The C++ codec in `engine/action/src/codec.cpp` implements strict validation for this schema subset (required fields, `additionalProperties: false`, safe-integer bounds) and round-trips the examples in `tests/native/contract_tests.cpp`. `tools/contract.ts` generates the TypeScript declarations in `contracts/generated/` from these schemas; `bun run contract:check` fails when the generated file is stale, and `bun run contract:smoke` (`tools/schema-smoke.ts`) validates the examples plus rejected fixtures against the same schema subset.

`bun run matrix:gen` regenerates `docs/api/compatibility-matrix.md` from `docs/api/coverage.json`; `bun run matrix:check` (part of `bun run test`) fails when the matrix is stale.
