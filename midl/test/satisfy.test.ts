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
