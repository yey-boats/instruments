import { test } from "node:test";
import assert from "node:assert";
import { readFileSync } from "node:fs";
import { readdirSync } from "node:fs";
import { versionCompatible, authOk, PROTO_MAJOR, PROTO_MINOR } from "../version.js";
import { validate } from "../validators.js";

const fx = (n) => JSON.parse(readFileSync(new URL(`../../fixtures/${n}`, import.meta.url)));

test("attach fixture validates", () => assert.ok(validate.Attach(fx("attach.json"))));
test("switch fixture validates", () => assert.ok(validate.Switch(fx("switch.json"))));
test("device record validates", () => assert.ok(validate.DeviceRecord(fx("device_record.json"))));
test("control state validates", () => assert.ok(validate.ControlState(fx("control_state.json"))));
test("same major compatible (any minor), other majors not", () => {
  assert.equal(PROTO_MAJOR, 2);
  assert.equal(versionCompatible(`${PROTO_MAJOR}.0`), true);
  assert.equal(versionCompatible(`${PROTO_MAJOR}.99`), true);
  assert.equal(versionCompatible(`${PROTO_MAJOR - 1}.0`), false);
  assert.equal(versionCompatible(`${PROTO_MAJOR + 1}.0`), false);
});
// instruments#25: the fixtures are what the firmware emits/accepts, so every
// one (bar the deliberate wrong-major sample) speaks the schema's major.
test("every fixture is stamped with the schema major", () => {
  const dir = new URL("../../fixtures/", import.meta.url);
  const names = readdirSync(dir).filter((n) => n.endsWith(".json"));
  assert.ok(names.length > 0);
  for (const n of names) {
    const msg = fx(n);
    const vs = [msg.v, msg.device?.v].filter((v) => v !== undefined);
    for (const v of vs) {
      if (n === "attach_v1major.json") {
        assert.equal(versionCompatible(v), false, `${n}: v=${v} must be the incompatible sample`);
      } else {
        assert.equal(versionCompatible(v), true, `${n}: v=${v} is not major ${PROTO_MAJOR}`);
      }
    }
  }
});
test("own version string is major.minor from the schema", () =>
  assert.equal(`${PROTO_MAJOR}.${PROTO_MINOR}`, "2.0"));
test("attach ack fixture validates", () => assert.ok(validate.AttachAck(fx("attach_ack.json"))));
test("switch ack fixture validates", () => assert.ok(validate.SwitchAck(fx("switch_ack.json"))));
test("heartbeat validates", () =>
  assert.ok(validate.Heartbeat({ v: "2.0", t: "heartbeat", sessionId: "s1" })));
test("detach validates", () =>
  assert.ok(validate.Detach({ v: "2.0", t: "detach", sessionId: "s1" })));
test("unknown field still validates (forward-compat)", () =>
  assert.ok(validate.Attach(fx("attach_unknown_field.json"))));
test("auth open vs keyed", () => {
  assert.equal(authOk("", "x"), true);
  assert.equal(authOk("k", "k"), true);
  assert.equal(authOk("k", "z"), false);
});
