// QuickJS module-loader fixture, NOT a TS source.
// Kept as plain `.mjs` on purpose: `tests/js/js_smoke.cpp` imports
// `./answer.mjs` to verify file-root confinement and ESM loading.
// Excluded from typecheck by design (tsconfig `allowJs: false`, `include: **/*.ts`).
// Bun never executes this file; only the C++ QuickJS host loads it.
export const answer = 42;
