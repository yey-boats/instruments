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

test("a structurally malformed manifest is reported, not thrown", () => {
  const badManifest = { ...manifest, midl: "bogus" } as unknown as Manifest;
  const r = validateDocument(read("valid/minimal.yaml"), badManifest, "sunton-480");
  expect(r.ok).toBe(false);
  expect(r.issues.length).toBeGreaterThan(0);
  expect(r.issues.some((i) => i.path.startsWith("/manifest"))).toBe(true);
});
