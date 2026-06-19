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
