const files = [
  "contracts/schema/action-v1.schema.json",
  "contracts/schema/result-v1.schema.json",
  "contracts/schema/examples/action-v1.json",
  "contracts/schema/examples/result-v1.json",
];
for (const file of files) await Bun.file(file).json();
console.log(`Checked ${files.length} contract files`);
export {};
