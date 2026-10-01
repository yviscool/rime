export default {
  runtime: {
    id: "rime",
    schemaVersion: 1,
    engine: "engine",
    hosts: ["desktop"],
  },
  paths: {
    contracts: "contracts",
    sdk: "sdk",
    apps: "apps",
    plugins: "plugins",
    tests: "tests",
  },
} as const;
