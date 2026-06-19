// midl/test/generated-fresh.test.ts
import { test, expect } from "vitest";
import { execSync } from "node:child_process";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const repo = join(dirname(fileURLToPath(import.meta.url)), "..", "..");

test("committed generated manifests match a fresh generation", () => {
  // Build + run the generator into a temp dir, diff against committed files.
  execSync("c++ -std=c++17 -Iinclude tools/yb_midl_gen/gen.cpp -o /tmp/yb_midl_gen_test", { cwd: repo });
  const cases: [string, string][] = [
    ["square-480", "esp32-4848s040"],
    ["landscape-800x480", "waveshare-touch-lcd-4_3"],
    ["landscape-1024x600", "waveshare-touch-lcd-5_1024x600"],
  ];
  for (const [cls, board] of cases) {
    const fresh = execSync(`/tmp/yb_midl_gen_test ${cls} ${board}`, { cwd: repo }).toString();
    const committed = readFileSync(join(repo, "schemas", "gen", `yb-midl-capabilities.${cls}.json`), "utf8");
    expect(committed).toBe(fresh);
  }
});
