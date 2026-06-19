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
