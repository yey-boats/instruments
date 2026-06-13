import { test } from "node:test";
import assert from "node:assert";
import { readFileSync } from "node:fs";
import { versionCompatible, authOk } from "../version.js";
import { validate } from "../validators.js";

const fx = (n) => JSON.parse(readFileSync(new URL(`../../fixtures/${n}`, import.meta.url)));

test("attach fixture validates", () => assert.ok(validate.Attach(fx("attach.json"))));
test("switch fixture validates", () => assert.ok(validate.Switch(fx("switch.json"))));
test("device record validates", () => assert.ok(validate.DeviceRecord(fx("device_record.json"))));
test("control state validates", () => assert.ok(validate.ControlState(fx("control_state.json"))));
test("v1.x compatible, v2 not", () => {
  assert.equal(versionCompatible("1.0"), true);
  assert.equal(versionCompatible("1.99"), true);
  assert.equal(versionCompatible("2.0"), false);
});
test("unknown field still validates (forward-compat)", () =>
  assert.ok(validate.Attach(fx("attach_unknown_field.json"))));
test("auth open vs keyed", () => {
  assert.equal(authOk("", "x"), true);
  assert.equal(authOk("k", "k"), true);
  assert.equal(authOk("k", "z"), false);
});
