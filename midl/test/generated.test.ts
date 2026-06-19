// midl/test/generated.test.ts
import { test, expect } from "vitest";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";
import { validateManifestStructure } from "../src/index";
import type { Manifest } from "../src/types";

const genDir = join(dirname(fileURLToPath(import.meta.url)), "..", "..", "schemas", "gen");
const load = (cls: string): Manifest =>
  JSON.parse(readFileSync(join(genDir, `yb-midl-capabilities.${cls}.json`), "utf8"));

const CLASSES = ["square-480", "landscape-800x480", "landscape-1024x600"];

test.each(CLASSES)("generated manifest %s is structurally valid", (cls) => {
  expect(validateManifestStructure(load(cls))).toEqual([]);
});

test("square-480 advertises the 9 element types and correct limits", () => {
  const m = load("square-480");
  const types = m.elements.map((e) => e.type).sort();
  expect(types).toEqual(
    ["autopilot", "bar", "button", "compass", "gauge", "single-value", "text", "trend", "windrose"].sort(),
  );
  const c = m.classes.find((x) => x.id === "square-480")!;
  expect(c.maxTiles).toBe(4);
  expect(c.maxDepth).toBe(3);
  expect(m.midl).toMatch(/^\d+\.\d+\.\d+$/);
});

test("dial elements carry glyphs, non-dial elements do not", () => {
  const m = load("square-480");
  const compass = m.elements.find((e) => e.type === "compass")!;
  const numeric = m.elements.find((e) => e.type === "single-value")!;
  expect(compass.glyphs && compass.glyphs.length).toBeGreaterThan(0);
  expect(numeric.glyphs).toBeUndefined();
});
