# Runtime Contracts

This directory owns versioned wire and data contracts shared by the native runtime, SDK, plugins and tests. JSON Schema Draft 2020-12 is the source of truth for serialized contracts.

`action-v1.schema.json` and `result-v1.schema.json` define the first Action request/response boundary. IDs are bounded by JavaScript's safe integer range so the same values remain exact in QuickJS and TypeScript. Deadlines are absolute Unix milliseconds; the host converts them to its monotonic clock when accepting an action.

The C++ codec in `engine/action/src/codec.cpp` implements strict validation for this schema subset (required fields, `additionalProperties: false`, safe-integer bounds) and round-trips the examples in `tests/native/contract_tests.cpp`. `tools/contract.ts` generates the TypeScript declarations in `contracts/generated/` from these schemas; `bun run contract:check` fails when the generated file is stale, and `tests/contracts/schema-smoke.mjs` validates the examples plus rejected fixtures against the same schema subset.
