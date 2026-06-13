// Run from proto/js: cd proto/js && node ../gen/gen_ts.mjs
// Uses createRequire so node resolves json-schema-to-typescript from proto/js/node_modules
// regardless of which directory this script lives in.
import { createRequire } from "node:module";
import { writeFileSync, existsSync } from "node:fs";
import { fileURLToPath, URL } from "node:url";

const jsDir = new URL("../js/", import.meta.url);
const jsDirPath = fileURLToPath(jsDir);

// Resolve the library from proto/js where it is installed
const require = createRequire(new URL("package.json", jsDir));
const { compileFromFile } = require("json-schema-to-typescript");

const schemaPath = fileURLToPath(new URL("../schema/espdisp-control-1.schema.json", import.meta.url));
const outPath = fileURLToPath(new URL("../js/types.d.ts", import.meta.url));

const ts = await compileFromFile(schemaPath);
writeFileSync(outPath, ts);
console.log("generated proto/js/types.d.ts");
