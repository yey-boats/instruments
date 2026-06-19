# YB-MIDL Language Core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `@yey-boats/midl`, a standalone TypeScript package that parses, structurally validates, preset-expands, capability-checks, and version-checks a YB-MIDL dashboard document against a device capability manifest — the contract every other subsystem (web renderer, manager, firmware generator) consumes.

**Architecture:** Pure library, no device or rendering dependencies. Two JSON Schemas (config + capabilities) provide structural grammar; a small satisfaction validator provides the cross-document admissibility check JSON Schema can't express; a golden corpus of fixtures pins behavior. This is Plan 1 of 6 (see `2026-06-19-generic-dashboard-runtime-design.md` §3.6/§5).

**Tech Stack:** TypeScript (ESM), Vitest (tests), Ajv (JSON Schema 2020-12), `yaml` (YAML↔JSON).

**Location:** New TS package at repo root `midl/`. (Overridable: could move to the manager repo or a standalone repo; the plan assumes `midl/`.)

---

## File structure

```
midl/
  package.json            # @yey-boats/midl, scripts, deps
  tsconfig.json
  schemas/
    yb-midl-config.schema.json        # config document grammar
    yb-midl-capabilities.schema.json  # capability manifest grammar
  src/
    types.ts              # shared TS interfaces (hand-authored now; generator-owned in Plan 2)
    version.ts            # MidlVersion, parseVersion, compatible
    canonicalize.ts       # YAML/JSON parse + canonical stringify
    presets.ts            # Node expand(), countTiles(), depth()
    validate.ts           # Ajv structural validation (config + manifest)
    satisfy.ts            # capability-satisfaction validator
    index.ts              # validateDocument() + re-exports
  test/
    version.test.ts
    canonicalize.test.ts
    presets.test.ts
    validate.test.ts
    satisfy.test.ts
    corpus.test.ts
    fixtures/
      manifest.sunton-480.json
      valid/minimal.yaml
      invalid/too-many-tiles.json
      invalid/unknown-element.json
```

---

## Task 1: Scaffold the `@yey-boats/midl` package

**Files:**
- Create: `midl/package.json`
- Create: `midl/tsconfig.json`
- Create: `midl/src/index.ts` (temporary stub)

- [ ] **Step 1: Create `midl/package.json`**

```json
{
  "name": "@yey-boats/midl",
  "version": "0.1.0",
  "type": "module",
  "main": "dist/index.js",
  "types": "dist/index.d.ts",
  "scripts": {
    "test": "vitest run",
    "build": "tsc --noEmit -p tsconfig.json"
  },
  "dependencies": {
    "ajv": "^8.17.1",
    "yaml": "^2.4.5"
  },
  "devDependencies": {
    "typescript": "^5.4.5",
    "vitest": "^1.6.0"
  }
}
```

- [ ] **Step 2: Create `midl/tsconfig.json`**

```json
{
  "compilerOptions": {
    "target": "ES2022",
    "module": "ES2022",
    "moduleResolution": "Bundler",
    "strict": true,
    "declaration": true,
    "outDir": "dist",
    "resolveJsonModule": true,
    "esModuleInterop": true,
    "skipLibCheck": true
  },
  "include": ["src", "test"]
}
```

- [ ] **Step 3: Create temporary `midl/src/index.ts`**

```typescript
export const MIDL_PACKAGE = "@yey-boats/midl";
```

- [ ] **Step 4: Install deps and verify the toolchain runs**

Run: `cd midl && npm install && npx vitest run`
Expected: install succeeds; vitest reports `No test files found` (exit 0 or "no tests" message — acceptable at this stage).

- [ ] **Step 5: Commit**

```bash
git add midl/package.json midl/tsconfig.json midl/src/index.ts midl/package-lock.json
git commit -m "feat(midl): scaffold @yey-boats/midl TS package"
```

---

## Task 2: Shared TypeScript types

**Files:**
- Create: `midl/src/types.ts`
- Test: `midl/test/types.test.ts`

- [ ] **Step 1: Write the failing test**

```typescript
// midl/test/types.test.ts
import { test, expect } from "vitest";
import type { ConfigDoc, Manifest } from "../src/types";

test("ConfigDoc and Manifest shapes are constructible", () => {
  const cfg: ConfigDoc = {
    midl: "1.0.0",
    screens: [
      {
        id: "dash",
        elements: { sog: { type: "single-value", bindings: { value: { kind: "signalk", path: "navigation.speedOverGround" } } } },
        layout: { element: "sog" },
      },
    ],
  };
  const man: Manifest = {
    midl: "1.0.0",
    board: "sunton-4848s040",
    classes: [{ id: "sunton-480", maxTiles: 4, maxDepth: 3 }],
    elements: [{ type: "single-value", bindings: ["value"] }],
    sources: ["signalk"],
  };
  expect(cfg.screens[0].id).toBe("dash");
  expect(man.classes[0].maxTiles).toBe(4);
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd midl && npx vitest run test/types.test.ts`
Expected: FAIL — `Cannot find module '../src/types'`.

- [ ] **Step 3: Create `midl/src/types.ts`**

```typescript
export interface MidlVersion { major: number; minor: number; build: number; }

export type Source =
  | { kind: "signalk"; path: string }
  | { kind: "local"; id: string }
  | { kind: "const"; value: unknown }
  | { kind: "computed"; expr?: string };

export interface Action { kind: "put" | "nav" | "command"; target?: string; value?: unknown; }

export interface Element {
  type: string;
  name?: string;
  bindings?: Record<string, Source>;
  style?: Record<string, unknown>;
  format?: Record<string, unknown>;
  markers?: Array<{ glyph?: string; [k: string]: unknown }>;
  action?: Action;
  zoom?: string;
}

export type Node =
  | { element: string }
  | { dir: "row" | "col"; children: Node[]; weights?: number[] }
  | { rows: number; cols: number; cells: Node[] }
  | { preset: string; slots?: string[] };

export interface Variant { class: string; layout: Node; }

export interface Screen {
  id: string;
  title?: string;
  elements: Record<string, Element>;
  layout: Node;
  variants?: Variant[];
}

export interface ConfigDoc {
  midl: string;
  settings?: Record<string, unknown>;
  defaults?: Record<string, unknown>;
  screens: Screen[];
  alarms?: unknown[];
  presets?: Record<string, unknown>;
}

export interface DeviceClass {
  id: string;
  width?: number;
  height?: number;
  maxTiles: number;
  maxDepth: number;
  presets?: string[];
  elements?: string[];
}

export interface ElementCap {
  type: string;
  bindings?: string[];
  attrs?: string[];
  units?: string[];
  glyphs?: string[];
}

export interface Manifest {
  midl: string;
  firmwareVersion?: string;
  board: string;
  classes: DeviceClass[];
  elements: ElementCap[];
  sources?: string[];
  actionKinds?: string[];
  presets?: string[];
  fonts?: number[];
  themes?: string[];
}

export interface Issue { path: string; message: string; }
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd midl && npx vitest run test/types.test.ts`
Expected: PASS (1 test).

- [ ] **Step 5: Commit**

```bash
git add midl/src/types.ts midl/test/types.test.ts
git commit -m "feat(midl): shared config + manifest types"
```

---

## Task 3: MIDL version & compatibility

**Files:**
- Create: `midl/src/version.ts`
- Test: `midl/test/version.test.ts`

- [ ] **Step 1: Write the failing test**

```typescript
// midl/test/version.test.ts
import { test, expect } from "vitest";
import { parseVersion, compatible } from "../src/version";

test("parseVersion parses MAJOR.MINOR.BUILD", () => {
  expect(parseVersion("1.2.37")).toEqual({ major: 1, minor: 2, build: 37 });
});

test("parseVersion rejects malformed", () => {
  expect(() => parseVersion("1.2")).toThrow();
});

test("same major, older config minor runs on newer build", () => {
  expect(compatible(parseVersion("1.2.0"), parseVersion("1.5.9"))).toBe(true);
});

test("newer config minor on older build is incompatible", () => {
  expect(compatible(parseVersion("1.6.0"), parseVersion("1.5.0"))).toBe(false);
});

test("different major is incompatible", () => {
  expect(compatible(parseVersion("1.0.0"), parseVersion("2.0.0"))).toBe(false);
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd midl && npx vitest run test/version.test.ts`
Expected: FAIL — `Cannot find module '../src/version'`.

- [ ] **Step 3: Create `midl/src/version.ts`**

```typescript
import type { MidlVersion } from "./types";

export function parseVersion(s: string): MidlVersion {
  const m = /^(\d+)\.(\d+)\.(\d+)$/.exec(s);
  if (!m) throw new Error(`bad MIDL version: ${s}`);
  return { major: Number(m[1]), minor: Number(m[2]), build: Number(m[3]) };
}

// A config is admissible on a device iff majors match and the config's
// minor is <= the device's minor (forward compat: old config on new build).
export function compatible(config: MidlVersion, device: MidlVersion): boolean {
  return config.major === device.major && config.minor <= device.minor;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd midl && npx vitest run test/version.test.ts`
Expected: PASS (5 tests).

- [ ] **Step 5: Commit**

```bash
git add midl/src/version.ts midl/test/version.test.ts
git commit -m "feat(midl): version parse + compatibility rule"
```

---

## Task 4: YAML/JSON canonicalization

**Files:**
- Create: `midl/src/canonicalize.ts`
- Test: `midl/test/canonicalize.test.ts`

- [ ] **Step 1: Write the failing test**

```typescript
// midl/test/canonicalize.test.ts
import { test, expect } from "vitest";
import { parseDoc, toCanonicalJson, toYaml } from "../src/canonicalize";

test("YAML and equivalent JSON parse to the same object", () => {
  const fromYaml = parseDoc("midl: 1.0.0\nscreens: []\n");
  const fromJson = parseDoc('{"midl":"1.0.0","screens":[]}');
  expect(fromYaml).toEqual(fromJson);
});

test("round-trip JSON->YAML->JSON is lossless", () => {
  const original = { midl: "1.0.0", screens: [{ id: "a", elements: {}, layout: { element: "x" } }] };
  const back = parseDoc(toYaml(original));
  expect(back).toEqual(original);
});

test("toCanonicalJson is stable 2-space JSON", () => {
  expect(toCanonicalJson({ b: 1, a: 2 })).toBe('{\n  "b": 1,\n  "a": 2\n}');
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd midl && npx vitest run test/canonicalize.test.ts`
Expected: FAIL — `Cannot find module '../src/canonicalize'`.

- [ ] **Step 3: Create `midl/src/canonicalize.ts`**

```typescript
import { parse as parseYaml, stringify as stringifyYaml } from "yaml";

// YAML is a JSON superset, so this parses both YAML and JSON input.
export function parseDoc(text: string): unknown {
  return parseYaml(text);
}

export function toCanonicalJson(doc: unknown): string {
  return JSON.stringify(doc, null, 2);
}

export function toYaml(doc: unknown): string {
  return stringifyYaml(doc);
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd midl && npx vitest run test/canonicalize.test.ts`
Expected: PASS (3 tests).

- [ ] **Step 5: Commit**

```bash
git add midl/src/canonicalize.ts midl/test/canonicalize.test.ts
git commit -m "feat(midl): YAML/JSON canonicalization + lossless round-trip"
```

---

## Task 5: Layout preset expansion & tree metrics

**Files:**
- Create: `midl/src/presets.ts`
- Test: `midl/test/presets.test.ts`

- [ ] **Step 1: Write the failing test**

```typescript
// midl/test/presets.test.ts
import { test, expect } from "vitest";
import { expand, countTiles, depth } from "../src/presets";
import type { Node } from "../src/types";

test("'full' preset expands to a single leaf", () => {
  expect(expand({ preset: "full", slots: ["x"] })).toEqual({ element: "x" });
});

test("'hero-split' expands {1,{2,3}} to row[leaf, col[leaf,leaf]]", () => {
  expect(expand({ preset: "hero-split", slots: ["a", "b", "c"] })).toEqual({
    dir: "row",
    children: [{ element: "a" }, { dir: "col", children: [{ element: "b" }, { element: "c" }] }],
  });
});

test("unknown preset throws", () => {
  expect(() => expand({ preset: "nope", slots: [] })).toThrow(/unknown preset/);
});

test("wrong slot count throws", () => {
  expect(() => expand({ preset: "full", slots: [] })).toThrow(/slots/);
});

test("countTiles and depth on an expanded tree", () => {
  const t: Node = expand({ preset: "hero-split", slots: ["a", "b", "c"] });
  expect(countTiles(t)).toBe(3);
  expect(depth(t)).toBe(3);
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd midl && npx vitest run test/presets.test.ts`
Expected: FAIL — `Cannot find module '../src/presets'`.

- [ ] **Step 3: Create `midl/src/presets.ts`**

```typescript
import type { Node } from "./types";

export type PresetExpander = (slots: string[]) => Node;

function req(slots: string[], n: number): string[] {
  if (slots.length !== n) throw new Error(`preset expects ${n} slots, got ${slots.length}`);
  return slots;
}

// Registry of named presets. Each expands to a Node (which may itself
// contain further presets — expand() recurses). Add new presets here.
export const PRESETS: Record<string, PresetExpander> = {
  full: (s) => ({ element: req(s, 1)[0] }),
  "hero-split": (s) => {
    const [a, b, c] = req(s, 3);
    return { dir: "row", children: [{ element: a }, { dir: "col", children: [{ element: b }, { element: c }] }] };
  },
};

export function expand(n: Node): Node {
  if ("preset" in n) {
    const fn = PRESETS[n.preset];
    if (!fn) throw new Error(`unknown preset: ${n.preset}`);
    return expand(fn(n.slots ?? []));
  }
  if ("children" in n) return { ...n, children: n.children.map(expand) };
  if ("cells" in n) return { ...n, cells: n.cells.map(expand) };
  return n;
}

export function countTiles(n: Node): number {
  if ("element" in n) return 1;
  if ("children" in n) return n.children.reduce((a, c) => a + countTiles(c), 0);
  if ("cells" in n) return n.cells.reduce((a, c) => a + countTiles(c), 0);
  return 0; // a preset node (should not appear after expand)
}

export function depth(n: Node): number {
  if ("element" in n) return 1;
  const kids = "children" in n ? n.children : "cells" in n ? n.cells : [];
  return kids.length ? 1 + Math.max(...kids.map(depth)) : 1;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd midl && npx vitest run test/presets.test.ts`
Expected: PASS (5 tests).

- [ ] **Step 5: Commit**

```bash
git add midl/src/presets.ts midl/test/presets.test.ts
git commit -m "feat(midl): preset expansion + tile/depth metrics"
```

---

## Task 6: JSON Schemas + structural validation

**Files:**
- Create: `midl/schemas/yb-midl-config.schema.json`
- Create: `midl/schemas/yb-midl-capabilities.schema.json`
- Create: `midl/src/validate.ts`
- Test: `midl/test/validate.test.ts`

- [ ] **Step 1: Write the failing test**

```typescript
// midl/test/validate.test.ts
import { test, expect } from "vitest";
import { validateConfigStructure, validateManifestStructure } from "../src/validate";

const goodConfig = {
  midl: "1.0.0",
  screens: [{ id: "dash", elements: { sog: { type: "single-value" } }, layout: { element: "sog" } }],
};
const goodManifest = {
  midl: "1.0.0",
  board: "sunton-4848s040",
  classes: [{ id: "sunton-480", maxTiles: 4, maxDepth: 3 }],
  elements: [{ type: "single-value" }],
};

test("valid config has no structural issues", () => {
  expect(validateConfigStructure(goodConfig)).toEqual([]);
});

test("config missing 'midl' is rejected", () => {
  const bad = { screens: [] };
  const issues = validateConfigStructure(bad);
  expect(issues.length).toBeGreaterThan(0);
});

test("config with a malformed layout node is rejected", () => {
  const bad = { midl: "1.0.0", screens: [{ id: "d", elements: {}, layout: { bogus: true } }] };
  expect(validateConfigStructure(bad).length).toBeGreaterThan(0);
});

test("valid manifest has no structural issues", () => {
  expect(validateManifestStructure(goodManifest)).toEqual([]);
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd midl && npx vitest run test/validate.test.ts`
Expected: FAIL — `Cannot find module '../src/validate'`.

- [ ] **Step 3: Create `midl/schemas/yb-midl-config.schema.json`**

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://yey.boats/schemas/yb-midl-config.schema.json",
  "title": "YB-MIDL Config Document",
  "type": "object",
  "required": ["midl", "screens"],
  "properties": {
    "midl": { "type": "string", "pattern": "^\\d+\\.\\d+\\.\\d+$" },
    "settings": { "type": "object" },
    "defaults": { "type": "object" },
    "screens": { "type": "array", "items": { "$ref": "#/$defs/screen" } },
    "alarms": { "type": "array", "items": { "$ref": "#/$defs/alarm" } },
    "presets": { "type": "object" }
  },
  "$defs": {
    "screen": {
      "type": "object",
      "required": ["id", "elements", "layout"],
      "properties": {
        "id": { "type": "string" },
        "title": { "type": "string" },
        "elements": { "type": "object", "additionalProperties": { "$ref": "#/$defs/element" } },
        "layout": { "$ref": "#/$defs/node" },
        "variants": {
          "type": "array",
          "items": {
            "type": "object",
            "required": ["class", "layout"],
            "properties": { "class": { "type": "string" }, "layout": { "$ref": "#/$defs/node" } }
          }
        }
      }
    },
    "element": {
      "type": "object",
      "required": ["type"],
      "properties": {
        "type": { "type": "string" },
        "name": { "type": "string" },
        "bindings": { "type": "object", "additionalProperties": { "$ref": "#/$defs/source" } },
        "style": { "type": "object" },
        "format": { "type": "object" },
        "markers": { "type": "array" },
        "action": { "$ref": "#/$defs/action" },
        "zoom": { "type": "string" }
      }
    },
    "source": {
      "type": "object",
      "required": ["kind"],
      "properties": {
        "kind": { "type": "string", "enum": ["signalk", "local", "const", "computed"] },
        "path": { "type": "string" },
        "id": { "type": "string" },
        "expr": { "type": "string" },
        "value": {}
      }
    },
    "action": {
      "type": "object",
      "required": ["kind"],
      "properties": {
        "kind": { "type": "string", "enum": ["put", "nav", "command"] },
        "target": { "type": "string" },
        "value": {}
      }
    },
    "node": {
      "oneOf": [
        { "type": "object", "required": ["element"], "additionalProperties": false,
          "properties": { "element": { "type": "string" } } },
        { "type": "object", "required": ["dir", "children"], "additionalProperties": false,
          "properties": { "dir": { "enum": ["row", "col"] },
            "children": { "type": "array", "items": { "$ref": "#/$defs/node" } },
            "weights": { "type": "array", "items": { "type": "number" } } } },
        { "type": "object", "required": ["rows", "cols", "cells"], "additionalProperties": false,
          "properties": { "rows": { "type": "integer" }, "cols": { "type": "integer" },
            "cells": { "type": "array", "items": { "$ref": "#/$defs/node" } } } },
        { "type": "object", "required": ["preset"], "additionalProperties": false,
          "properties": { "preset": { "type": "string" },
            "slots": { "type": "array", "items": { "type": "string" } } } }
      ]
    },
    "alarm": {
      "type": "object",
      "required": ["id", "source"],
      "properties": {
        "id": { "type": "string" },
        "source": { "$ref": "#/$defs/source" },
        "level": { "enum": ["info", "warn", "alarm", "emergency"] },
        "lt": { "type": "number" },
        "gt": { "type": "number" },
        "message": { "type": "string" }
      }
    }
  }
}
```

- [ ] **Step 4: Create `midl/schemas/yb-midl-capabilities.schema.json`**

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://yey.boats/schemas/yb-midl-capabilities.schema.json",
  "title": "YB-MIDL Capabilities Manifest",
  "type": "object",
  "required": ["midl", "board", "classes", "elements"],
  "properties": {
    "midl": { "type": "string", "pattern": "^\\d+\\.\\d+\\.\\d+$" },
    "firmwareVersion": { "type": "string" },
    "board": { "type": "string" },
    "classes": {
      "type": "array",
      "items": {
        "type": "object",
        "required": ["id", "maxTiles", "maxDepth"],
        "properties": {
          "id": { "type": "string" },
          "width": { "type": "integer" },
          "height": { "type": "integer" },
          "maxTiles": { "type": "integer" },
          "maxDepth": { "type": "integer" },
          "presets": { "type": "array", "items": { "type": "string" } },
          "elements": { "type": "array", "items": { "type": "string" } }
        }
      }
    },
    "elements": {
      "type": "array",
      "items": {
        "type": "object",
        "required": ["type"],
        "properties": {
          "type": { "type": "string" },
          "bindings": { "type": "array", "items": { "type": "string" } },
          "attrs": { "type": "array", "items": { "type": "string" } },
          "units": { "type": "array", "items": { "type": "string" } },
          "glyphs": { "type": "array", "items": { "type": "string" } }
        }
      }
    },
    "sources": { "type": "array", "items": { "type": "string" } },
    "actionKinds": { "type": "array", "items": { "type": "string" } },
    "presets": { "type": "array", "items": { "type": "string" } },
    "fonts": { "type": "array", "items": { "type": "integer" } },
    "themes": { "type": "array", "items": { "type": "string" } }
  }
}
```

- [ ] **Step 5: Create `midl/src/validate.ts`**

```typescript
// Use the 2020-12 dialect build of Ajv: the schemas use $defs / oneOf and
// declare $schema draft 2020-12. The default Ajv export is draft-07.
import Ajv2020 from "ajv/dist/2020";
import type { ValidateFunction } from "ajv";
import configSchema from "../schemas/yb-midl-config.schema.json";
import capsSchema from "../schemas/yb-midl-capabilities.schema.json";
import type { Issue } from "./types";

const ajv = new Ajv2020({ allErrors: true, strict: false });
// Cast: the imported JSON's inferred literal type does not match Ajv's
// AnySchemaObject; the runtime value is a valid schema object.
const vConfig: ValidateFunction = ajv.compile(configSchema as object);
const vCaps: ValidateFunction = ajv.compile(capsSchema as object);

function toIssues(v: ValidateFunction): Issue[] {
  return (v.errors ?? []).map((e) => ({ path: e.instancePath || "/", message: e.message ?? "invalid" }));
}

export function validateConfigStructure(doc: unknown): Issue[] {
  return vConfig(doc) ? [] : toIssues(vConfig);
}

export function validateManifestStructure(doc: unknown): Issue[] {
  return vCaps(doc) ? [] : toIssues(vCaps);
}
```

- [ ] **Step 6: Run test to verify it passes**

Run: `cd midl && npx vitest run test/validate.test.ts`
Expected: PASS (4 tests).

- [ ] **Step 7: Commit**

```bash
git add midl/schemas/ midl/src/validate.ts midl/test/validate.test.ts
git commit -m "feat(midl): config + capabilities JSON Schemas with structural validation"
```

---

## Task 7: Capability-satisfaction validator

**Files:**
- Create: `midl/src/satisfy.ts`
- Test: `midl/test/satisfy.test.ts`

- [ ] **Step 1: Write the failing test**

```typescript
// midl/test/satisfy.test.ts
import { test, expect } from "vitest";
import { satisfies } from "../src/satisfy";
import type { ConfigDoc, Manifest } from "../src/types";

const manifest: Manifest = {
  midl: "1.0.0",
  board: "sunton-4848s040",
  classes: [{ id: "sunton-480", maxTiles: 4, maxDepth: 3, elements: ["single-value", "compass"] }],
  elements: [
    { type: "single-value", bindings: ["value"] },
    { type: "compass", bindings: ["value"] },
  ],
  sources: ["signalk"],
  actionKinds: ["put", "nav"],
};

test("a config within limits is admissible", () => {
  const cfg: ConfigDoc = {
    midl: "1.0.0",
    screens: [{ id: "d", elements: { a: { type: "single-value" } }, layout: { element: "a" } }],
  };
  expect(satisfies(cfg, manifest, "sunton-480")).toEqual([]);
});

test("unknown element type is rejected", () => {
  const cfg: ConfigDoc = {
    midl: "1.0.0",
    screens: [{ id: "d", elements: { a: { type: "windrose" } }, layout: { element: "a" } }],
  };
  const issues = satisfies(cfg, manifest, "sunton-480");
  expect(issues.some((i) => /not supported/.test(i.message))).toBe(true);
});

test("too many tiles for the class is rejected", () => {
  const cfg: ConfigDoc = {
    midl: "1.0.0",
    screens: [{
      id: "d",
      elements: { a: { type: "single-value" }, b: { type: "single-value" }, c: { type: "single-value" }, d: { type: "single-value" }, e: { type: "single-value" } },
      layout: { dir: "col", children: [{ element: "a" }, { element: "b" }, { element: "c" }, { element: "d" }, { element: "e" }] },
    }],
  };
  expect(satisfies(cfg, manifest, "sunton-480").some((i) => /too many tiles/.test(i.message))).toBe(true);
});

test("unsupported source kind is rejected", () => {
  const cfg: ConfigDoc = {
    midl: "1.0.0",
    screens: [{ id: "d", elements: { a: { type: "single-value", bindings: { value: { kind: "local", id: "gpio4" } } } }, layout: { element: "a" } }],
  };
  expect(satisfies(cfg, manifest, "sunton-480").some((i) => /source kind/.test(i.message))).toBe(true);
});

test("unknown class is rejected", () => {
  const cfg: ConfigDoc = { midl: "1.0.0", screens: [] };
  expect(satisfies(cfg, manifest, "watch-240").some((i) => /class not supported/.test(i.message))).toBe(true);
});

test("variant layout is used for its class", () => {
  const cfg: ConfigDoc = {
    midl: "1.0.0",
    screens: [{
      id: "d",
      elements: { a: { type: "single-value" }, b: { type: "compass" } },
      layout: { element: "a" },
      variants: [{ class: "sunton-480", layout: { element: "b" } }],
    }],
  };
  expect(satisfies(cfg, manifest, "sunton-480")).toEqual([]);
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd midl && npx vitest run test/satisfy.test.ts`
Expected: FAIL — `Cannot find module '../src/satisfy'`.

- [ ] **Step 3: Create `midl/src/satisfy.ts`**

```typescript
import type { ConfigDoc, Element, ElementCap, Issue, Manifest, Node, Screen, Source } from "./types";
import { expand, countTiles, depth } from "./presets";

function resolveLayout(screen: Screen, className: string): Node {
  const v = screen.variants?.find((x) => x.class === className);
  return v ? v.layout : screen.layout;
}

function elementIds(n: Node, out: string[]): void {
  if ("element" in n) out.push(n.element);
  else if ("children" in n) n.children.forEach((c) => elementIds(c, out));
  else if ("cells" in n) n.cells.forEach((c) => elementIds(c, out));
}

function checkElement(
  el: Element,
  path: string,
  allowedTypes: Set<string>,
  capByType: Map<string, ElementCap>,
  allowedSources: Set<string>,
  allowedActions: Set<string>,
  issues: Issue[],
): void {
  if (!allowedTypes.has(el.type)) {
    issues.push({ path: `${path}/type`, message: `element type not supported: ${el.type}` });
    return;
  }
  const cap = capByType.get(el.type);
  if (el.bindings) {
    for (const [field, src] of Object.entries(el.bindings)) {
      if (cap?.bindings && !cap.bindings.includes(field))
        issues.push({ path: `${path}/bindings/${field}`, message: `binding field not supported by ${el.type}: ${field}` });
      if (!allowedSources.has((src as Source).kind))
        issues.push({ path: `${path}/bindings/${field}`, message: `source kind not supported: ${(src as Source).kind}` });
    }
  }
  if (el.action && allowedActions.size > 0 && !allowedActions.has(el.action.kind))
    issues.push({ path: `${path}/action`, message: `action kind not supported: ${el.action.kind}` });
}

export function satisfies(config: ConfigDoc, manifest: Manifest, className: string): Issue[] {
  const issues: Issue[] = [];
  const cls = manifest.classes.find((c) => c.id === className);
  if (!cls) return [{ path: "/", message: `class not supported: ${className}` }];

  const allowedTypes = new Set(cls.elements ?? manifest.elements.map((e) => e.type));
  const capByType = new Map(manifest.elements.map((e) => [e.type, e]));
  const allowedSources = new Set(manifest.sources ?? ["signalk"]);
  const allowedActions = new Set(manifest.actionKinds ?? []);

  config.screens.forEach((screen, si) => {
    let tree: Node;
    try {
      tree = expand(resolveLayout(screen, className));
    } catch (e) {
      issues.push({ path: `/screens/${si}/layout`, message: (e as Error).message });
      return;
    }
    const tiles = countTiles(tree);
    if (tiles > cls.maxTiles)
      issues.push({ path: `/screens/${si}/layout`, message: `too many tiles: ${tiles} > ${cls.maxTiles}` });
    const d = depth(tree);
    if (d > cls.maxDepth)
      issues.push({ path: `/screens/${si}/layout`, message: `nesting too deep: ${d} > ${cls.maxDepth}` });

    const ids: string[] = [];
    elementIds(tree, ids);
    for (const id of ids) {
      const el = screen.elements[id];
      if (!el) {
        issues.push({ path: `/screens/${si}`, message: `layout references unknown element: ${id}` });
        continue;
      }
      checkElement(el, `/screens/${si}/elements/${id}`, allowedTypes, capByType, allowedSources, allowedActions, issues);
    }
  });
  return issues;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd midl && npx vitest run test/satisfy.test.ts`
Expected: PASS (6 tests).

- [ ] **Step 5: Commit**

```bash
git add midl/src/satisfy.ts midl/test/satisfy.test.ts
git commit -m "feat(midl): capability-satisfaction validator"
```

---

## Task 8: Top-level `validateDocument` + golden corpus

**Files:**
- Modify: `midl/src/index.ts` (replace the Task 1 stub)
- Create: `midl/test/fixtures/manifest.sunton-480.json`
- Create: `midl/test/fixtures/valid/minimal.yaml`
- Create: `midl/test/fixtures/invalid/too-many-tiles.json`
- Create: `midl/test/fixtures/invalid/unknown-element.json`
- Create: `midl/test/corpus.test.ts`

- [ ] **Step 1: Write the failing test**

```typescript
// midl/test/corpus.test.ts
import { test, expect } from "vitest";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { validateDocument } from "../src/index";
import type { Manifest } from "../src/types";

const here = dirname(fileURLToPath(import.meta.url));
const manifest: Manifest = JSON.parse(readFileSync(join(here, "fixtures/manifest.sunton-480.json"), "utf8"));
const read = (p: string) => readFileSync(join(here, "fixtures", p), "utf8");

test("valid/minimal.yaml passes", () => {
  const r = validateDocument(read("valid/minimal.yaml"), manifest, "sunton-480");
  expect(r.ok).toBe(true);
  expect(r.issues).toEqual([]);
});

test("invalid/too-many-tiles.json fails with a tile-count issue", () => {
  const r = validateDocument(read("invalid/too-many-tiles.json"), manifest, "sunton-480");
  expect(r.ok).toBe(false);
  expect(r.issues.some((i) => /too many tiles/.test(i.message))).toBe(true);
});

test("invalid/unknown-element.json fails with a type issue", () => {
  const r = validateDocument(read("invalid/unknown-element.json"), manifest, "sunton-480");
  expect(r.ok).toBe(false);
  expect(r.issues.some((i) => /not supported/.test(i.message))).toBe(true);
});

test("a cross-major config is rejected as incompatible", () => {
  const r = validateDocument('{"midl":"2.0.0","screens":[]}', manifest, "sunton-480");
  expect(r.ok).toBe(false);
  expect(r.issues.some((i) => /incompatible MIDL/.test(i.message))).toBe(true);
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd midl && npx vitest run test/corpus.test.ts`
Expected: FAIL — `validateDocument` is not exported / fixtures missing.

- [ ] **Step 3: Create the fixtures**

`midl/test/fixtures/manifest.sunton-480.json`:
```json
{
  "midl": "1.0.0",
  "firmwareVersion": "0.5.0",
  "board": "sunton-4848s040",
  "classes": [{ "id": "sunton-480", "width": 480, "height": 480, "maxTiles": 4, "maxDepth": 3, "elements": ["single-value", "compass"] }],
  "elements": [
    { "type": "single-value", "bindings": ["value"], "units": ["kn", "m"] },
    { "type": "compass", "bindings": ["value"], "glyphs": ["triangle", "diamond"] }
  ],
  "sources": ["signalk"],
  "actionKinds": ["put", "nav", "command"],
  "presets": ["full", "hero-split"]
}
```

`midl/test/fixtures/valid/minimal.yaml`:
```yaml
midl: 1.0.0
screens:
  - id: dash
    elements:
      sog: { type: single-value, bindings: { value: { kind: signalk, path: navigation.speedOverGround } } }
    layout: { element: sog }
```

`midl/test/fixtures/invalid/too-many-tiles.json`:
```json
{
  "midl": "1.0.0",
  "screens": [{
    "id": "dash",
    "elements": {
      "a": { "type": "single-value" }, "b": { "type": "single-value" },
      "c": { "type": "single-value" }, "d": { "type": "single-value" },
      "e": { "type": "single-value" }
    },
    "layout": { "dir": "col", "children": [
      { "element": "a" }, { "element": "b" }, { "element": "c" }, { "element": "d" }, { "element": "e" }
    ] }
  }]
}
```

`midl/test/fixtures/invalid/unknown-element.json`:
```json
{
  "midl": "1.0.0",
  "screens": [{
    "id": "dash",
    "elements": { "w": { "type": "windrose" } },
    "layout": { "element": "w" }
  }]
}
```

- [ ] **Step 4: Replace `midl/src/index.ts`**

```typescript
import { parseDoc } from "./canonicalize";
import { validateConfigStructure } from "./validate";
import { satisfies } from "./satisfy";
import { parseVersion, compatible } from "./version";
import type { ConfigDoc, Issue, Manifest } from "./types";

export * from "./types";
export { parseVersion, compatible } from "./version";
export { expand, countTiles, depth, PRESETS } from "./presets";
export { parseDoc, toCanonicalJson, toYaml } from "./canonicalize";
export { validateConfigStructure, validateManifestStructure } from "./validate";
export { satisfies } from "./satisfy";

export interface ValidationResult { ok: boolean; issues: Issue[]; }

// Full pipeline: structural -> version compat -> capability satisfaction.
// Returns the first failing stage's issues (do not run satisfaction on a
// structurally-invalid doc).
export function validateDocument(text: string, manifest: Manifest, className: string): ValidationResult {
  const doc = parseDoc(text) as ConfigDoc;
  const structural = validateConfigStructure(doc);
  if (structural.length) return { ok: false, issues: structural };
  if (!compatible(parseVersion(doc.midl), parseVersion(manifest.midl)))
    return { ok: false, issues: [{ path: "/midl", message: `incompatible MIDL ${doc.midl} vs device ${manifest.midl}` }] };
  const sat = satisfies(doc, manifest, className);
  return { ok: sat.length === 0, issues: sat };
}
```

- [ ] **Step 5: Run the full test suite**

Run: `cd midl && npx vitest run`
Expected: PASS — all test files green (types, version, canonicalize, presets, validate, satisfy, corpus).

- [ ] **Step 6: Verify the package typechecks**

Run: `cd midl && npx tsc --noEmit -p tsconfig.json`
Expected: exit 0, no type errors. (Emit/bundling — copying schemas into `dist`, fixing output layout — is deferred to the plan that first consumes the package as a build dependency.)

- [ ] **Step 7: Commit**

```bash
git add midl/src/index.ts midl/test/corpus.test.ts midl/test/fixtures/
git commit -m "feat(midl): validateDocument pipeline + golden corpus"
```

---

## Done criteria

- `cd midl && npx vitest run` is green across all suites.
- `cd midl && npx tsc --noEmit -p tsconfig.json` typechecks with no errors.
- The package exports `validateDocument`, `satisfies`, `expand`, `parseVersion`, `compatible`, the two schemas' validators, and all shared types.
- The golden corpus has at least one passing config and the failure classes: too-many-tiles, unknown-element, cross-major incompatibility.

## Notes for follow-on plans

- `src/types.ts` is hand-authored here; **Plan 2** (C++ catalog → MIDL generator) makes the manifest a generated artifact and should also generate these TS types from MIDL, replacing the hand-authored copy (the corpus keeps them honest).
- Preset registry currently has `full` and `hero-split`; the remaining spec presets (`{1,{2,3,4}}`, `MxN`, `{{2|3|4},1,{2|3|4}}`) are added the same way in `presets.ts` with corresponding tests.
- The native (C++) satisfaction validator for `pio test -e native` is part of the firmware plan (**Plan 6**); the shared golden corpus in `test/fixtures/` is the cross-implementation parity oracle.
- Local-source taxonomy (spec §10 open item) is currently `{signalk, local, const, computed}` in the schema; refine when Plan 6 enumerates device-local sources.
