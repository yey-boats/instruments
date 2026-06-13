# espdisp Control Protocol — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a versioned, transport-agnostic espdisp control protocol — defined once as a JSON Schema, code-generated into a C++ (firmware) and JS (plugin/phone) library — that lets any controller discover and control any target display over IP (mDNS + HTTP, primary) and BLE (on-demand central, fallback), with lightweight many-to-many sessions, optional shared-key auth, and a per-controller colored "controlled" frame on targets; verified on hardware by a headless ESP32-S3-DevKitC-1 controller harness.

**Architecture:** A pure, host-tested C++ protocol library (schema-generated records + hand-written version/auth/session logic, mirroring `signalk_parser.cpp`) is shared by all firmware roles. Targets (displays) expose the protocol over HTTP (`/api/p2p/*`) and a BLE Control GATT service, maintain a session table, and render the colored frame on the LVGL UI task. Controllers (the harness + the knob) discover via mDNS browse / BLE scan and control via the same generated lib over the best transport (IP > BLE). The SignalK plugin migrates onto the generated JS lib. A bare ESP32-S3-DevKitC-1 runs a headless controller loop as the on-hardware verifier.

**Tech Stack:** JSON Schema (draft 2020-12) + a project-owned Python generator (C++/ArduinoJson) + `json-schema-to-typescript`/`ajv` (JS); PlatformIO/Arduino (ESP32-S3, IDF 4.4); NimBLE (peripheral + on-demand central); Arduino `WebServer` + ESP-IDF `mdns`; LVGL 9; Unity (host) + node:test (JS); Node SignalK plugin.

**Spec:** `docs/superpowers/specs/2026-06-13-espdisp-control-protocol-design.md`

**Delivery:** One combined implementation. The phases below are build order (dependencies before consumers), not separate releases.

---

## File-structure map

```
proto/
  schema/espdisp-control-1.schema.json     # source of truth (v1)
  fixtures/*.json                          # shared conformance vectors
  gen/gen_cpp.py                           # schema -> C++ (ArduinoJson) generator
  gen/gen_ts.mjs                           # schema -> TS types (json-schema-to-typescript)
  js/package.json, index.js, validators.js, version.js, types.d.ts(gen), test/proto.test.js
include/proto/
  records_generated.h                      # GENERATED structs + (de)serializers (checked in)
  proto.h                                  # hand-written: version/compat, auth, session table API
src/proto/proto.cpp                        # hand-written pure logic
test/test_proto/test_proto.cpp             # Unity host tests over fixtures
src/proto_ip.{h,cpp}                       # firmware IP binding helpers (target + controller)
src/proto_ble.{h,cpp}                      # firmware BLE Control GATT (target) + central (controller)
src/ui/control_frame.cpp                   # target colored-frame overlay (LVGL, UI task)
src/harness_main.cpp                       # headless DevKitC-1 controller loop (harness env only)
platformio.ini                             # native filter += proto; envs: harness-s3; proto extra_script
Makefile                                   # `make proto` (regen + freshness)
signalk/plugins/signalk-espdisp-manager/   # migrate onto proto/js
```

---

# PHASE 1 — Protocol foundation (pure, host + CI tested)

### Task 1.1: Protocol JSON Schema (v1)

**Files:**
- Create: `proto/schema/espdisp-control-1.schema.json`

- [ ] **Step 1: Write the schema**

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://espdisp/proto/espdisp-control-1",
  "title": "espdisp control protocol v1",
  "x-proto-major": 1,
  "x-proto-minor": 0,
  "$defs": {
    "ViewRef": {
      "type": "object",
      "required": ["id", "title"],
      "properties": { "id": {"type": "string"}, "title": {"type": "string"} }
    },
    "Session": {
      "type": "object",
      "required": ["controllerId", "name", "color"],
      "properties": {
        "controllerId": {"type": "string"},
        "name": {"type": "string"},
        "color": {"type": "string", "pattern": "^#[0-9a-fA-F]{6}$"},
        "lastSeen": {"type": "integer"}
      }
    },
    "DeviceRecord": {
      "type": "object",
      "required": ["v", "deviceId", "role", "currentView"],
      "properties": {
        "v": {"type": "string"},
        "deviceId": {"type": "string"},
        "name": {"type": "string"},
        "role": {"type": "string", "enum": ["display", "controller", "both"]},
        "board": {"type": "string"},
        "display": {"type": "string"},
        "currentView": {"type": "string"},
        "views": {"type": "array", "items": {"$ref": "#/$defs/ViewRef"}},
        "transports": {"type": "array", "items": {"type": "string", "enum": ["ip", "ble"]}},
        "authRequired": {"type": "boolean"}
      }
    },
    "Attach": {
      "type": "object",
      "required": ["v", "t", "controllerId", "name", "color"],
      "properties": {
        "v": {"type": "string"}, "t": {"const": "attach"},
        "controllerId": {"type": "string"}, "name": {"type": "string"},
        "color": {"type": "string", "pattern": "^#[0-9a-fA-F]{6}$"},
        "key": {"type": "string"}, "ttlMs": {"type": "integer"}
      }
    },
    "AttachAck": {
      "type": "object",
      "required": ["v", "t", "accepted"],
      "properties": {
        "v": {"type": "string"}, "t": {"const": "attachAck"},
        "accepted": {"type": "boolean"}, "sessionId": {"type": "string"},
        "ttlMs": {"type": "integer"}, "reason": {"type": "string"},
        "device": {"$ref": "#/$defs/DeviceRecord"}
      }
    },
    "Switch": {
      "type": "object",
      "required": ["v", "t", "sessionId", "viewId"],
      "properties": {
        "v": {"type": "string"}, "t": {"const": "switch"},
        "sessionId": {"type": "string"}, "viewId": {"type": "string"}
      }
    },
    "SwitchAck": {
      "type": "object",
      "required": ["v", "t", "ok"],
      "properties": {
        "v": {"type": "string"}, "t": {"const": "switchAck"},
        "ok": {"type": "boolean"}, "currentView": {"type": "string"},
        "reason": {"type": "string"}
      }
    },
    "Heartbeat": {
      "type": "object", "required": ["v", "t", "sessionId"],
      "properties": { "v": {"type": "string"}, "t": {"const": "heartbeat"}, "sessionId": {"type": "string"} }
    },
    "HeartbeatAck": {
      "type": "object", "required": ["v", "t", "ok"],
      "properties": { "v": {"type": "string"}, "t": {"const": "heartbeatAck"}, "ok": {"type": "boolean"}, "ttlMs": {"type": "integer"} }
    },
    "Detach": {
      "type": "object", "required": ["v", "t", "sessionId"],
      "properties": { "v": {"type": "string"}, "t": {"const": "detach"}, "sessionId": {"type": "string"} }
    },
    "ControlState": {
      "type": "object",
      "required": ["v", "t", "currentView", "sessions"],
      "properties": {
        "v": {"type": "string"}, "t": {"const": "controlState"},
        "currentView": {"type": "string"},
        "sessions": {"type": "array", "items": {"$ref": "#/$defs/Session"}}
      }
    }
  }
}
```

- [ ] **Step 2: Validate the schema parses**

Run: `cd proto && python3 -c "import json;json.load(open('schema/espdisp-control-1.schema.json'));print('ok')"`
Expected: `ok`

- [ ] **Step 3: Commit**

```bash
git add proto/schema/espdisp-control-1.schema.json
git commit -m "feat(proto): espdisp control protocol v1 JSON Schema"
```

### Task 1.2: Conformance fixtures

**Files:**
- Create: `proto/fixtures/attach.json`, `attach_ack.json`, `switch.json`, `switch_ack.json`, `device_record.json`, `control_state.json`, `attach_v2major.json` (incompatible), `attach_unknown_field.json`

- [ ] **Step 1: Write the fixtures (each is a real protocol message)**

`proto/fixtures/attach.json`:
```json
{"v":"1.0","t":"attach","controllerId":"knob-aa01","name":"Helm knob","color":"#00bcd4","key":"hunter2","ttlMs":10000}
```
`proto/fixtures/attach_ack.json`:
```json
{"v":"1.0","t":"attachAck","accepted":true,"sessionId":"s-7","ttlMs":10000,"device":{"v":"1.0","deviceId":"mfd-helm","role":"display","currentView":"wind","views":[{"id":"wind","title":"Wind"},{"id":"nav","title":"Nav"}],"transports":["ip","ble"],"authRequired":true}}
```
`proto/fixtures/switch.json`:
```json
{"v":"1.0","t":"switch","sessionId":"s-7","viewId":"nav"}
```
`proto/fixtures/switch_ack.json`:
```json
{"v":"1.0","t":"switchAck","ok":true,"currentView":"nav"}
```
`proto/fixtures/device_record.json`:
```json
{"v":"1.0","deviceId":"mfd-helm","name":"Helm MFD","role":"both","board":"sunton_4848s040","display":"480x480","currentView":"wind","views":[{"id":"wind","title":"Wind"},{"id":"nav","title":"Nav"}],"transports":["ip","ble"],"authRequired":false}
```
`proto/fixtures/control_state.json`:
```json
{"v":"1.0","t":"controlState","currentView":"nav","sessions":[{"controllerId":"knob-aa01","name":"Helm knob","color":"#00bcd4","lastSeen":1000},{"controllerId":"plug-1","name":"Plugin","color":"#ff9800","lastSeen":1200}]}
```
`proto/fixtures/attach_v2major.json` (incompatible major):
```json
{"v":"2.0","t":"attach","controllerId":"x","name":"x","color":"#000000"}
```
`proto/fixtures/attach_unknown_field.json` (forward-compat: extra field ignored):
```json
{"v":"1.7","t":"attach","controllerId":"x","name":"x","color":"#010203","futureField":{"nested":true}}
```

- [ ] **Step 2: Validate each fixture against the schema**

Run: `cd proto && python3 -c "import json,glob; import jsonschema; s=json.load(open('schema/espdisp-control-1.schema.json')); [print(f) for f in glob.glob('fixtures/*.json')]" 2>/dev/null || pip install jsonschema && cd proto && for f in fixtures/*.json; do python3 -c "import json;json.load(open('$f'))"; done && echo all-parse-ok`
Expected: `all-parse-ok` (structural validity against `$defs` is asserted by the C++/JS suites in 1.5/1.6).

- [ ] **Step 3: Commit**

```bash
git add proto/fixtures
git commit -m "feat(proto): conformance fixtures (valid + incompatible + forward-compat)"
```

### Task 1.3: C++ generator + generated records

**Files:**
- Create: `proto/gen/gen_cpp.py`
- Create (generated, checked in): `include/proto/records_generated.h`

- [ ] **Step 1: Write the generator** (project-specific: handles our schema's `$defs` of objects with primitive / `$ref` / array-of(primitive|`$ref`) properties)

```python
#!/usr/bin/env python3
"""Generate include/proto/records_generated.h from the espdisp control schema.
Emits one POD struct per $def plus ArduinoJson from_json()/to_json() helpers.
Scope: the espdisp control schema's property kinds (string, integer, number,
boolean, array<string>, $ref, array<$ref>). Not a general JSON-Schema compiler."""
import json, sys, pathlib

CPP_PRIM = {"string": "const char*", "integer": "long", "number": "double", "boolean": "bool"}

def field_cpp_type(p):
    if "$ref" in p: return p["$ref"].split("/")[-1]      # nested struct
    t = p.get("type")
    if t == "array":
        it = p["items"]
        inner = it["$ref"].split("/")[-1] if "$ref" in it else "string"
        return ("arr", inner)
    if "const" in p: return "const char*"
    return CPP_PRIM[t]

def emit(schema):
    out = ['#pragma once',
           '// GENERATED by proto/gen/gen_cpp.py from espdisp-control-1.schema.json — do not edit.',
           '#include <ArduinoJson.h>', '#include <string.h>',
           'namespace proto {',
           f'constexpr int kProtoMajor = {schema["x-proto-major"]};',
           f'constexpr int kProtoMinor = {schema["x-proto-minor"]};', '']
    defs = schema["$defs"]
    # forward declares
    for name in defs: out.append(f'struct {name};')
    out.append('')
    for name, d in defs.items():
        out.append(f'struct {name} {{')
        for fn, p in d["properties"].items():
            ct = field_cpp_type(p)
            if isinstance(ct, tuple):
                _, inner = ct
                cap = 16
                if inner == "string":
                    out.append(f'  char {fn}[{cap}][24] = {{}}; int {fn}_count = 0;')
                else:
                    out.append(f'  {inner} {fn}[{cap}] = {{}}; int {fn}_count = 0;')
            elif ct == "const char*":
                out.append(f'  char {fn}[48] = {{0}};')
            else:
                out.append(f'  {ct} {fn} = {{}};')
        out.append('};')
        out.append('')
    # from_json / to_json
    for name, d in defs.items():
        out.append(f'inline void from_json(JsonObjectConst o, {name}& s) {{')
        for fn, p in d["properties"].items():
            ct = field_cpp_type(p)
            if isinstance(ct, tuple):
                _, inner = ct
                out.append(f'  {{ JsonArrayConst a=o["{fn}"]; s.{fn}_count=0;')
                out.append(f'    for (JsonVariantConst e: a) {{ if (s.{fn}_count>=16) break;')
                if inner == "string":
                    out.append(f'      strncpy(s.{fn}[s.{fn}_count], e | "", 23); s.{fn}_count++; }} }}')
                else:
                    out.append(f'      from_json(e.as<JsonObjectConst>(), s.{fn}[s.{fn}_count]); s.{fn}_count++; }} }}')
            elif "$ref" in p:
                out.append(f'  if (o["{fn}"].is<JsonObjectConst>()) from_json(o["{fn}"], s.{fn});')
            elif ct == "const char*":
                out.append(f'  strncpy(s.{fn}, o["{fn}"] | "", sizeof(s.{fn})-1);')
            elif ct == "bool":
                out.append(f'  s.{fn} = o["{fn}"] | false;')
            elif ct == "long":
                out.append(f'  s.{fn} = o["{fn}"] | 0L;')
            else:
                out.append(f'  s.{fn} = o["{fn}"] | 0.0;')
        out.append('}')
        out.append(f'inline void to_json(JsonObject o, const {name}& s) {{')
        for fn, p in d["properties"].items():
            ct = field_cpp_type(p)
            if "const" in p:
                out.append(f'  o["{fn}"] = "{p["const"]}";')
            elif isinstance(ct, tuple):
                _, inner = ct
                out.append(f'  {{ JsonArray a=o["{fn}"].to<JsonArray>();')
                out.append(f'    for (int i=0;i<s.{fn}_count;i++) {{')
                if inner == "string":
                    out.append(f'      a.add(s.{fn}[i]); }} }}')
                else:
                    out.append(f'      JsonObject e=a.add<JsonObject>(); to_json(e, s.{fn}[i]); }} }}')
            elif "$ref" in p:
                out.append(f'  {{ JsonObject e=o["{fn}"].to<JsonObject>(); to_json(e, s.{fn}); }}')
            else:
                out.append(f'  o["{fn}"] = s.{fn};')
        out.append('}')
        out.append('')
    out.append('}  // namespace proto')
    return "\n".join(out) + "\n"

if __name__ == "__main__":
    root = pathlib.Path(__file__).resolve().parents[2]
    schema = json.load(open(root / "proto/schema/espdisp-control-1.schema.json"))
    (root / "include/proto").mkdir(parents=True, exist_ok=True)
    (root / "include/proto/records_generated.h").write_text(emit(schema))
    print("generated include/proto/records_generated.h")
```

- [ ] **Step 2: Generate and confirm it compiles standalone**

Run: `python3 proto/gen/gen_cpp.py && cc -fsyntax-only -xc++ -std=c++17 -I include -I .pio/libdeps/native/ArduinoJson/src include/proto/records_generated.h 2>&1 | tail -3 || echo "note: needs ArduinoJson include path; full compile verified in 1.5"`
Expected: generation prints the path; syntax check passes (or defers to 1.5 where the native env supplies ArduinoJson).

- [ ] **Step 3: Commit**

```bash
git add proto/gen/gen_cpp.py include/proto/records_generated.h
git commit -m "feat(proto): C++ generator + generated ArduinoJson records"
```

### Task 1.4: Hand-written pure protocol logic (version/auth/session)

**Files:**
- Create: `include/proto/proto.h`, `src/proto/proto.cpp`

- [ ] **Step 1: Write `include/proto/proto.h`**

```cpp
#pragma once
// Hand-written pure protocol logic: version/compat, auth, the target-side
// session table. No Arduino/LVGL/BLE — host-testable (links in the native env).
#include <stdint.h>
#include "proto/records_generated.h"

namespace proto {

constexpr int kMaxSessions = 8;
constexpr long kDefaultTtlMs = 10000;

// "1.7" -> major 1, minor 7. Returns false on malformed input.
bool parse_version(const char* v, int& major, int& minor);
// Same-major required; our minor must be >= 0; unknown-minor accepted.
bool version_compatible(const char* peer_v);

// Shared-key check. configured_key == "" => open (accept any). Constant-ish compare.
bool auth_ok(const char* configured_key, const char* presented_key);

// Target-side session table (pure; the firmware feeds it now-millis).
struct SessionTable {
    Session sessions[kMaxSessions];
    char sessionId[kMaxSessions][16];
    bool used[kMaxSessions] = {false};
    long mostRecentIdx = -1;

    void clear();
    // Attach: returns index >=0 (assigns sessionId_out), or -1 if full.
    int attach(const Attach& a, long now_ms, char* sessionId_out, size_t cap);
    // Returns true and refreshes lastSeen; false if unknown sessionId.
    bool heartbeat(const char* sid, long now_ms);
    bool detach(const char* sid);
    // Reap sessions with lastSeen older than ttl_ms; returns count reaped.
    int reap(long now_ms, long ttl_ms);
    int active_count() const;
    // Fill a ControlState for serialization.
    void to_control_state(ControlState& out, const char* currentView) const;
};

}  // namespace proto
```

- [ ] **Step 2: Write `src/proto/proto.cpp`**

```cpp
#include "proto/proto.h"
#include <string.h>
#include <stdio.h>

namespace proto {

bool parse_version(const char* v, int& major, int& minor) {
    if (!v || !*v) return false;
    char* end = nullptr;
    long M = strtol(v, &end, 10);
    if (end == v || *end != '.') return false;
    long m = strtol(end + 1, &end, 10);
    if (*end != '\0') return false;
    major = (int)M; minor = (int)m; return true;
}

bool version_compatible(const char* peer_v) {
    int M, m;
    if (!parse_version(peer_v, M, m)) return false;
    return M == kProtoMajor;
}

bool auth_ok(const char* configured_key, const char* presented_key) {
    if (!configured_key || !configured_key[0]) return true;  // open
    if (!presented_key) return false;
    return strcmp(configured_key, presented_key) == 0;
}

void SessionTable::clear() {
    for (int i = 0; i < kMaxSessions; ++i) used[i] = false;
    mostRecentIdx = -1;
}

int SessionTable::attach(const Attach& a, long now_ms, char* sid_out, size_t cap) {
    int idx = -1;
    for (int i = 0; i < kMaxSessions; ++i)
        if (!used[i]) { idx = i; break; }
    if (idx < 0) return -1;
    used[idx] = true;
    strncpy(sessions[idx].controllerId, a.controllerId, sizeof(sessions[idx].controllerId) - 1);
    strncpy(sessions[idx].name, a.name, sizeof(sessions[idx].name) - 1);
    strncpy(sessions[idx].color, a.color, sizeof(sessions[idx].color) - 1);
    sessions[idx].lastSeen = now_ms;
    snprintf(sessionId[idx], 16, "s%ld-%d", now_ms % 100000, idx);
    if (sid_out) strncpy(sid_out, sessionId[idx], cap - 1);
    mostRecentIdx = idx;
    return idx;
}

bool SessionTable::heartbeat(const char* sid, long now_ms) {
    for (int i = 0; i < kMaxSessions; ++i)
        if (used[i] && strcmp(sessionId[i], sid) == 0) {
            sessions[i].lastSeen = now_ms; mostRecentIdx = i; return true;
        }
    return false;
}

bool SessionTable::detach(const char* sid) {
    for (int i = 0; i < kMaxSessions; ++i)
        if (used[i] && strcmp(sessionId[i], sid) == 0) { used[i] = false; return true; }
    return false;
}

int SessionTable::reap(long now_ms, long ttl_ms) {
    int n = 0;
    for (int i = 0; i < kMaxSessions; ++i)
        if (used[i] && now_ms - sessions[i].lastSeen > ttl_ms) { used[i] = false; n++; }
    return n;
}

int SessionTable::active_count() const {
    int n = 0;
    for (int i = 0; i < kMaxSessions; ++i) if (used[i]) n++;
    return n;
}

void SessionTable::to_control_state(ControlState& out, const char* currentView) const {
    strncpy(out.v, "1.0", sizeof(out.v) - 1);
    strncpy(out.currentView, currentView ? currentView : "", sizeof(out.currentView) - 1);
    out.sessions_count = 0;
    for (int i = 0; i < kMaxSessions; ++i) {
        if (!used[i] || out.sessions_count >= 16) continue;
        out.sessions[out.sessions_count++] = sessions[i];
    }
}

}  // namespace proto
```

- [ ] **Step 3: Commit** (tests in 1.5)

```bash
git add include/proto/proto.h src/proto/proto.cpp
git commit -m "feat(proto): pure version/auth/session-table logic"
```

### Task 1.5: C++ host tests over fixtures + logic

**Files:**
- Create: `test/test_proto/test_proto.cpp`
- Modify: `platformio.ini` (native `build_src_filter` += `+<proto/proto.cpp>`; `test_filter` += `test_proto`)

- [ ] **Step 1: Wire the native env**

In `[env:native]`, append `+<proto/proto.cpp>` to `build_src_filter` and add `test_proto` to `test_filter`.

- [ ] **Step 2: Write the failing tests**

```cpp
#include <unity.h>
#include <ArduinoJson.h>
#include <string.h>
#include "proto/proto.h"

using namespace proto;
void setUp() {} void tearDown() {}

static void test_version_parse_and_compat() {
    int M, m;
    TEST_ASSERT_TRUE(parse_version("1.7", M, m)); TEST_ASSERT_EQUAL_INT(1, M); TEST_ASSERT_EQUAL_INT(7, m);
    TEST_ASSERT_FALSE(parse_version("x", M, m));
    TEST_ASSERT_TRUE(version_compatible("1.0"));
    TEST_ASSERT_TRUE(version_compatible("1.99"));   // higher minor ok
    TEST_ASSERT_FALSE(version_compatible("2.0"));    // major mismatch
}

static void test_auth() {
    TEST_ASSERT_TRUE(auth_ok("", "anything"));       // open
    TEST_ASSERT_TRUE(auth_ok("hunter2", "hunter2"));
    TEST_ASSERT_FALSE(auth_ok("hunter2", "nope"));
    TEST_ASSERT_FALSE(auth_ok("hunter2", nullptr));
}

static void test_attach_fixture_roundtrips() {
    const char* json = R"({"v":"1.0","t":"attach","controllerId":"knob-aa01","name":"Helm knob","color":"#00bcd4","key":"hunter2","ttlMs":10000})";
    JsonDocument d; deserializeJson(d, json);
    Attach a; from_json(d.as<JsonObjectConst>(), a);
    TEST_ASSERT_EQUAL_STRING("knob-aa01", a.controllerId);
    TEST_ASSERT_EQUAL_STRING("#00bcd4", a.color);
    TEST_ASSERT_EQUAL_INT(10000, (int)a.ttlMs);
    JsonDocument out; to_json(out.to<JsonObject>(), a);
    TEST_ASSERT_EQUAL_STRING("attach", out["t"]);
    TEST_ASSERT_EQUAL_STRING("knob-aa01", out["controllerId"]);
}

static void test_device_record_views_roundtrip() {
    const char* json = R"({"v":"1.0","deviceId":"mfd-helm","role":"both","currentView":"wind","views":[{"id":"wind","title":"Wind"},{"id":"nav","title":"Nav"}],"transports":["ip","ble"]})";
    JsonDocument d; deserializeJson(d, json);
    DeviceRecord r; from_json(d.as<JsonObjectConst>(), r);
    TEST_ASSERT_EQUAL_INT(2, r.views_count);
    TEST_ASSERT_EQUAL_STRING("nav", r.views[1].id);
    TEST_ASSERT_EQUAL_INT(2, r.transports_count);
    TEST_ASSERT_EQUAL_STRING("ble", r.transports[1]);
}

static void test_unknown_field_ignored() {
    const char* json = R"({"v":"1.7","t":"attach","controllerId":"x","name":"x","color":"#010203","futureField":{"nested":true}})";
    JsonDocument d; deserializeJson(d, json);
    Attach a; from_json(d.as<JsonObjectConst>(), a);
    TEST_ASSERT_EQUAL_STRING("x", a.controllerId);   // parsed despite extra field
    TEST_ASSERT_TRUE(version_compatible(a.v));        // 1.7 compatible
}

static void test_session_table_lifecycle() {
    SessionTable t; t.clear();
    Attach a; strcpy(a.v,"1.0"); strcpy(a.controllerId,"c1"); strcpy(a.name,"C1"); strcpy(a.color,"#00bcd4");
    char sid[16];
    int idx = t.attach(a, 1000, sid, sizeof(sid));
    TEST_ASSERT_TRUE(idx >= 0); TEST_ASSERT_EQUAL_INT(1, t.active_count());
    TEST_ASSERT_TRUE(t.heartbeat(sid, 5000));
    TEST_ASSERT_EQUAL_INT(0, t.reap(9000, kDefaultTtlMs));   // 5000+10000 > 9000, alive
    TEST_ASSERT_EQUAL_INT(1, t.reap(20000, kDefaultTtlMs));  // stale -> reaped
    TEST_ASSERT_EQUAL_INT(0, t.active_count());
}

static void test_control_state_serialization() {
    SessionTable t; t.clear();
    Attach a; strcpy(a.v,"1.0"); strcpy(a.controllerId,"c1"); strcpy(a.name,"Knob"); strcpy(a.color,"#00bcd4");
    char sid[16]; t.attach(a, 1000, sid, sizeof(sid));
    ControlState cs; t.to_control_state(cs, "nav");
    JsonDocument out; to_json(out.to<JsonObject>(), cs);
    TEST_ASSERT_EQUAL_STRING("controlState", out["t"]);
    TEST_ASSERT_EQUAL_STRING("nav", out["currentView"]);
    TEST_ASSERT_EQUAL_STRING("#00bcd4", out["sessions"][0]["color"]);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_version_parse_and_compat);
    RUN_TEST(test_auth);
    RUN_TEST(test_attach_fixture_roundtrips);
    RUN_TEST(test_device_record_views_roundtrip);
    RUN_TEST(test_unknown_field_ignored);
    RUN_TEST(test_session_table_lifecycle);
    RUN_TEST(test_control_state_serialization);
    return UNITY_END();
}
```

- [ ] **Step 3: Run to verify fail, then it should pass once generated header + proto.cpp are present**

Run: `pio test -e native -f test_proto 2>&1 | tail -20`
Expected: PASS (7 tests). If `records_generated.h` is stale, run `python3 proto/gen/gen_cpp.py` first.

- [ ] **Step 4: Full suite (no regressions)**

Run: `pio test -e native 2>&1 | tail -6`
Expected: all suites pass.

- [ ] **Step 5: Commit**

```bash
git add platformio.ini test/test_proto/test_proto.cpp
git commit -m "test(proto): C++ conformance + version/auth/session tests"
```

### Task 1.6: JS protocol library + tests + `make proto`

**Files:**
- Create: `proto/js/package.json`, `proto/js/version.js`, `proto/js/validators.js`, `proto/js/index.js`, `proto/js/test/proto.test.js`, `proto/gen/gen_ts.mjs`
- Modify: `Makefile` (add `proto` target), `.github/workflows/ci.yml` (freshness check)

- [ ] **Step 1: `proto/js/package.json`**

```json
{
  "name": "@espdisp/proto",
  "version": "1.0.0",
  "type": "module",
  "main": "index.js",
  "scripts": {
    "gen": "node ../gen/gen_ts.mjs",
    "test": "node --test"
  },
  "devDependencies": { "json-schema-to-typescript": "^15.0.0" },
  "dependencies": { "ajv": "^8.17.1" }
}
```

- [ ] **Step 2: `proto/js/version.js`** (mirrors the C++ compat rule)

```javascript
import schema from "../schema/espdisp-control-1.schema.json" with { type: "json" };
export const PROTO_MAJOR = schema["x-proto-major"];
export const PROTO_MINOR = schema["x-proto-minor"];
export function parseVersion(v) {
  const m = /^(\d+)\.(\d+)$/.exec(v || "");
  return m ? { major: +m[1], minor: +m[2] } : null;
}
export function versionCompatible(v) {
  const p = parseVersion(v);
  return !!p && p.major === PROTO_MAJOR;
}
export function authOk(configuredKey, presentedKey) {
  if (!configuredKey) return true;
  return presentedKey === configuredKey;
}
```

- [ ] **Step 3: `proto/js/validators.js`** (ajv from the schema $defs)

```javascript
import Ajv from "ajv";
import schema from "../schema/espdisp-control-1.schema.json" with { type: "json" };
const ajv = new Ajv({ allowUnionTypes: true, strict: false });
ajv.addSchema(schema, "espdisp-control-1");
export function validator(defName) {
  return ajv.compile({ $ref: `espdisp-control-1#/$defs/${defName}` });
}
export const validate = {
  Attach: validator("Attach"), AttachAck: validator("AttachAck"),
  Switch: validator("Switch"), SwitchAck: validator("SwitchAck"),
  DeviceRecord: validator("DeviceRecord"), ControlState: validator("ControlState"),
};
```

- [ ] **Step 4: `proto/js/index.js`**

```javascript
export * from "./version.js";
export * from "./validators.js";
```

- [ ] **Step 5: `proto/js/test/proto.test.js`** (same fixtures as C++)

```javascript
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
```

- [ ] **Step 6: `proto/gen/gen_ts.mjs`** (TS types from schema)

```javascript
import { compileFromFile } from "json-schema-to-typescript";
import { writeFileSync } from "node:fs";
const ts = await compileFromFile(new URL("../schema/espdisp-control-1.schema.json", import.meta.url).pathname);
writeFileSync(new URL("../js/types.d.ts", import.meta.url).pathname, ts);
console.log("generated proto/js/types.d.ts");
```

- [ ] **Step 7: `make proto` target** (regen both; CI freshness)

Add to `Makefile`:
```make
proto:  ## Regenerate protocol code (C++ + TS) from the schema
	python3 proto/gen/gen_cpp.py
	cd proto/js && npm install --silent && node ../gen/gen_ts.mjs
```

- [ ] **Step 8: Run JS tests**

Run: `cd proto/js && npm install && npm test 2>&1 | tail -15`
Expected: all tests pass.

- [ ] **Step 9: CI freshness check**

In `.github/workflows/ci.yml` add a job step: `make proto && git diff --exit-code include/proto/records_generated.h proto/js/types.d.ts` (fails if the checked-in generated code is stale).

- [ ] **Step 10: Commit**

```bash
git add proto/js proto/gen/gen_ts.mjs Makefile .github/workflows/ci.yml
git commit -m "feat(proto): JS lib (ajv validators + version) + make proto + CI freshness"
```

---

# PHASE 2 — Headless test-harness firmware (ESP32-S3-DevKitC-1 controller)

### Task 2.1: Harness env + controller protocol client (IP)

**Files:**
- Create: `src/harness_main.cpp` (built ONLY in the harness env)
- Create: `src/proto_ip.h`, `src/proto_ip.cpp` (shared controller+target IP helpers)
- Modify: `platformio.ini` (add `[env:harness-s3-devkitc]`)

- [ ] **Step 1: Add the harness env**

```ini
; Headless protocol test harness on a bare ESP32-S3-DevKitC-1. No display,
; no LVGL/touch — a controller that loops protocol commands at a target.
[env:harness-s3-devkitc]
platform = espressif32@^6.7.0
board = esp32-s3-devkitc-1
framework = arduino
board_build.mcu = esp32s3
board_build.flash_size = 16MB
board_build.partitions = default_16MB.csv
build_flags =
    -D ARDUINO_USB_CDC_ON_BOOT=1
    -D BOARD_HAS_PSRAM
    -I include
    -DCORE_DEBUG_LEVEL=3
    -D ESPDISP_HARNESS=1
build_src_filter =
    -<*>
    +<harness_main.cpp>
    +<proto/proto.cpp>
    +<proto_ip.cpp>
    +<net_health.cpp>
lib_deps =
    bblanchon/ArduinoJson@^7.2.0
    links2004/WebSockets@^2.4.1
    h2zero/NimBLE-Arduino@^1.4.2
monitor_speed = 115200
```

- [ ] **Step 2: `src/proto_ip.h`** — the controller-side IP client + target-side handlers (shared)

```cpp
#pragma once
#include <Arduino.h>
#include "proto/proto.h"

namespace proto_ip {
// Controller side: HTTP calls against a target base URL ("http://host[:port]").
// Each returns true on a 2xx + parsed ack. Uses HTTPClient; call off the UI task.
bool get_device(const String& base, proto::DeviceRecord& out);
bool attach(const String& base, const proto::Attach& a, proto::AttachAck& ack);
bool do_switch(const String& base, const proto::Switch& s, proto::SwitchAck& ack);
bool heartbeat(const String& base, const char* sessionId);
bool detach(const String& base, const char* sessionId);
bool get_state(const String& base, proto::ControlState& out);
}  // namespace proto_ip
```

- [ ] **Step 3: `src/proto_ip.cpp`** — implement with `HTTPClient` (POST JSON via `to_json`, parse acks via `from_json`). (Full body: serialize the request struct with ArduinoJson `to_json`, `http.POST(payload)`, `deserializeJson` the response, `from_json` into the ack struct, return `http code/100==2`.) Target-side request handlers are registered by web.cpp in Phase 3; this file holds only the client in the harness build and is shared by the controller path.

- [ ] **Step 4: `src/harness_main.cpp`** — the automated loop

```cpp
#include <Arduino.h>
#include <WiFi.h>
#include "secrets.h"
#include "proto/proto.h"
#include "proto_ip.h"

// Target base URL via build flag or serial; default from env/NVS.
static String s_target = "http://espdisp.local";

static void run_cycle() {
    proto::DeviceRecord dev;
    if (!proto_ip::get_device(s_target, dev)) { Serial.println("[harness] FAIL get_device"); return; }
    Serial.printf("[harness] target=%s views=%d\n", dev.deviceId, dev.views_count);

    proto::Attach a{}; strcpy(a.v, "1.0"); strcpy(a.controllerId, "harness-1");
    strcpy(a.name, "Harness"); strcpy(a.color, "#e91e63"); a.ttlMs = 10000;
    proto::AttachAck ack;
    if (!proto_ip::attach(s_target, a, ack) || !ack.accepted) { Serial.println("[harness] FAIL attach"); return; }
    Serial.printf("[harness] attached sid=%s\n", ack.sessionId);

    for (int i = 0; i < dev.views_count; ++i) {
        proto::Switch sw{}; strcpy(sw.v, "1.0"); strcpy(sw.sessionId, ack.sessionId);
        strncpy(sw.viewId, dev.views[i].id, sizeof(sw.viewId) - 1);
        proto::SwitchAck sa;
        bool ok = proto_ip::do_switch(s_target, sw, sa) && sa.ok && strcmp(sa.currentView, dev.views[i].id) == 0;
        Serial.printf("[harness] switch %s -> %s\n", dev.views[i].id, ok ? "PASS" : "FAIL");
        proto_ip::heartbeat(s_target, ack.sessionId);
        delay(1500);
    }
    proto_ip::detach(s_target, ack.sessionId);
    Serial.println("[harness] cycle done");
}

void setup() {
    Serial.begin(115200);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) delay(200);
    Serial.printf("[harness] wifi up %s\n", WiFi.localIP().toString().c_str());
}
void loop() { run_cycle(); delay(5000); }
```

- [ ] **Step 5: Build the harness**

Run: `pio run -e harness-s3-devkitc 2>&1 | tail -8`
Expected: SUCCESS. (Functionally exercised in Phase 3 once a target serves `/api/p2p/*`.)

- [ ] **Step 6: Commit**

```bash
git add platformio.ini src/proto_ip.h src/proto_ip.cpp src/harness_main.cpp
git commit -m "feat(harness): headless ESP32-S3 controller + IP protocol client"
```

---

# PHASE 3 — IP binding (target endpoints + colored frame + controller discovery)

### Task 3.1: Target `/api/p2p/*` endpoints + session table

**Files:**
- Modify: `src/web.cpp` (register `/api/p2p/device|attach|switch|heartbeat|detach|state`), `include/web.h`
- Create: `src/proto_target.h`, `src/proto_target.cpp` (owns the `proto::SessionTable`, the shared key, current-view, exposes apply/query for web.cpp + BLE)

- [ ] **Step 1: `src/proto_target.h`** — the device's protocol state (single instance, mutex-guarded)

```cpp
#pragma once
#include <Arduino.h>
#include "proto/proto.h"
namespace proto_target {
void setup();                                   // load shared key from NVS
void fill_device_record(proto::DeviceRecord& r);// id/role/views/currentView/auth
bool handle_attach(const proto::Attach& a, proto::AttachAck& ack);   // auth + session
bool handle_switch(const proto::Switch& s, proto::SwitchAck& ack);   // last-writer-wins -> ui::show_by_id
bool handle_heartbeat(const char* sid);
bool handle_detach(const char* sid);
void fill_state(proto::ControlState& cs);
void tick(long now_ms);                         // reap stale sessions; notify frame on change
int  active_session_snapshot(proto::Session* out, int cap);  // for the frame (UI task reads)
}  // namespace proto_target
```

- [ ] **Step 2: `src/proto_target.cpp`** — wrap `proto::SessionTable` with a FreeRTOS mutex; `handle_switch` posts an `app::Command{ShowScreen, viewId}` (mirrors `web.cpp handle_screen_set`) and records `currentView`; `tick()` reaps and, on any session-set change, posts an `app::Command` so the frame overlay refreshes. Shared key from NVS (`storage::Namespace("proto")`, key `"key"`). `fill_device_record` reads `ui::screen_*` for views + current + `net::deviceId()` + `board::*`.

- [ ] **Step 3: Register routes in `src/web.cpp`** (next to the existing `server.on(...)` block ~line 1496)

```cpp
server.on("/api/p2p/device", HTTP_GET, handle_p2p_device);
server.on("/api/p2p/attach", HTTP_POST, handle_p2p_attach);
server.on("/api/p2p/switch", HTTP_POST, handle_p2p_switch);
server.on("/api/p2p/heartbeat", HTTP_POST, handle_p2p_heartbeat);
server.on("/api/p2p/detach", HTTP_POST, handle_p2p_detach);
server.on("/api/p2p/state", HTTP_GET, handle_p2p_state);
```
Each handler: parse body with ArduinoJson → `from_json` → `proto_target::handle_*` → `to_json` the ack → `server.send(200, "application/json", payload)`. Gate by `version_compatible` (else 400 `incompatible_version`) and reuse the existing `require_api_auth()` plus the protocol shared-key inside the message.

- [ ] **Step 4: Build the display env**

Run: `pio run -e esp32-4848s040 2>&1 | tail -5`
Expected: SUCCESS.

- [ ] **Step 5: Commit**

```bash
git add include/web.h src/web.cpp src/proto_target.h src/proto_target.cpp
git commit -m "feat(proto): target /api/p2p endpoints + session table"
```

### Task 3.2: Colored "controlled" frame overlay (target, LVGL)

**Files:**
- Create: `src/ui/control_frame.cpp`, declare `ui::control_frame::{build,refresh,set_sessions}` in `include/screens.h`
- Modify: `src/main.cpp` (build the frame on `lv_layer_top()` at boot; refresh from `proto_target::active_session_snapshot` in `ui_refresh`)

- [ ] **Step 1: Implement `ui::control_frame`** — on `lv_layer_top()`, draw up to `kMaxSessions` thin nested borders (outermost = most-recent), each an `lv_obj` with transparent fill + colored border (color parsed from `#RRGGBB`), inset by `i*frame_step`. A small name-pill (top-center) shows the most-recent controller's name. `set_sessions(const proto::Session*, int)` updates colors/visibility; called from the UI task only. No active sessions → all hidden. Border widths/insets from the consolidated style config.

- [ ] **Step 2: Wire into `main.cpp`** — build at boot (UI task); in `ui_refresh` call `proto_target::active_session_snapshot` + `ui::control_frame::set_sessions(...)` (cheap; only repaints on change via a dirty compare). `proto_target::tick(millis())` runs each refresh to reap.

- [ ] **Step 3: Build**

Run: `pio run -e esp32-4848s040 2>&1 | tail -5`
Expected: SUCCESS.

- [ ] **Step 4: Sim-render the frame** (extend `sim/` like the knob menu): render 1 session and 3 stacked sessions at 480² and 360 round → `docs/sim-shots/control-frame-*.png`; bounds OK.

Run: `make sim 2>&1 | tail -8`

- [ ] **Step 5: Commit**

```bash
git add include/screens.h src/ui/control_frame.cpp src/main.cpp sim/ docs/sim-shots/
git commit -m "feat(ui): per-controller colored control frame + sim renders"
```

### Task 3.3: Controller mDNS discovery + multi-source registry

**Files:**
- Create: `src/proto_discovery.h`, `src/proto_discovery.cpp` (mDNS browse `_espdisp._tcp`)
- Modify: `src/knob_remote.cpp` (merge mDNS/IP peers, deduped by deviceId), `src/net.cpp` (add `pv`/`role` to the device's own mDNS TXT)

- [ ] **Step 1: mDNS browse** — `proto_discovery::browse(callback)` using ESP-IDF `mdns_query_ptr("_espdisp", "_tcp", ...)`; for each result emit `{deviceId, host/ip, port, board, pv, role}` from the TXT + A record. Runs on the manager/worker task.
- [ ] **Step 2: Extend own TXT** in `src/net.cpp` mDNS registration: add `pv` = `"1.0"` and `role` = `"display"` (or `"both"`).
- [ ] **Step 3: Merge into `knob_remote`** — a `knob_remote::ingest_ip_peer(...)` path (mirrors the manager ingest from Phase F) populating remote entries with `transport=ip` + base URL; dedup by deviceId (IP preferred). `switch_view` for an IP peer calls `proto_ip::attach`+`do_switch`+`detach` (or holds a session) via the worker.
- [ ] **Step 4: Build both envs.** Run: `pio run -e esp32-4848s040 2>&1 | tail -5 && pio run -e harness-s3-devkitc 2>&1 | tail -5` → SUCCESS.
- [ ] **Step 5: Commit** `feat(proto): mDNS discovery + IP peer merge into knob_remote`

### Task 3.4: HARDWARE verification (harness ↔ Sunton target, IP)

**Files:** none (verification)

- [ ] **Step 1:** Flash the Sunton display: `pio run -e esp32-4848s040 -t upload`. Flash the harness on the DevKitC-1: `pio run -e harness-s3-devkitc -t upload` (set `WIFI_*` + target URL in `secrets.h`).
- [ ] **Step 2:** Watch the harness serial: each cycle must log `attached`, then `switch <id> -> PASS` for every view, then `cycle done`. On the display, the **pink frame** (`#e91e63`, the harness color) must appear while attached and clear after detach/timeout.
- [ ] **Step 3:** Confirm `GET /api/p2p/state` on the display reflects the harness session; confirm last-writer-wins by running two harness instances (or the harness + `curl`) with different colors → stacked frames.
- [ ] **Step 4:** `espdisp soak` the display while the harness loops; expect PASS (no heap regression).
- [ ] **Step 5: Commit** any tuning.

---

# PHASE 4 — BLE binding (Control GATT + on-demand central)

### Task 4.1: Target BLE Control GATT service

**Files:**
- Modify: `src/ble_config.cpp` / `include/ble_config.h` (add the Control service + characteristics), reusing the generated (de)serializers
- Modify: `include/board_pins.h` or a new `include/proto_ble_uuids.h` (UUID `#define` block, wrapped in `// clang-format off/on`)

- [ ] **Step 1: Define UUIDs** (Control service `a3f7e100-...`, chars `DEVICE a3f7e101` read, `CONTROL a3f7e102` write, `STATE a3f7e103` read+notify) in a `// clang-format off` block.
- [ ] **Step 2: Add the service** in the existing NimBLE server setup (mirror the CONNECTION characteristic). `DEVICE` read → `to_json(DeviceRecord)` (summarized to ≤512 B: cap views; full list via IP). `CONTROL` write → parse `{attach|switch|heartbeat|detach}` → `proto_target::handle_*` (same path as HTTP). `STATE` → `ControlState`, `notify` on change. Use `setValue(const uint8_t*, len)`.
- [ ] **Step 3: Build** `pio run -e esp32-4848s040` → SUCCESS. **Step 4: Commit** `feat(proto): target BLE Control GATT service`.

### Task 4.2: Controller on-demand BLE central

**Files:**
- Create: `src/proto_ble.h`, `src/proto_ble.cpp` (scan + connect-one + control + disconnect)
- Modify: harness env + knob env `sdkconfig`/build flags to enable the NimBLE central role; `src/proto_discovery.cpp` (BLE scan source)

- [ ] **Step 1: Enable central role** — per-env sdkconfig (`CONFIG_BT_NIMBLE_ROLE_CENTRAL=y`, `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=2`) for `harness-s3-devkitc` and `waveshare-knob-1_8` only. Document the heap-budget check.
- [ ] **Step 2: `proto_ble` on-demand central** — `scan(ms, cb)` for the Control service UUID (deviceId from adv name, `pv` from manufacturer/scan-rsp); `switch_on_peer(addr, attach, switch)` = connect → discover Control service → write CONTROL(attach) → read/await ack → write CONTROL(switch) → write CONTROL(detach) → **disconnect**. One peer at a time; never hold a connection idle. Mirrors the existing peripheral teardown discipline.
- [ ] **Step 3:** Merge BLE-discovered peers into `knob_remote` (`transport=ble`), used only when the peer has no reachable IP (per the IP-primary rule).
- [ ] **Step 4: Build** harness + knob envs → SUCCESS. **Step 5: Commit** `feat(proto): on-demand BLE central controller`.

### Task 4.3: HARDWARE verification (BLE, harness ↔ target)

- [ ] **Step 1:** On the harness, disable IP to the target (or point it at a BLE-only peer); flash harness + display.
- [ ] **Step 2:** Confirm the harness BLE-scans, finds the display by its Control service, connects once, switches a view (frame appears in harness color), disconnects. Repeat in a loop.
- [ ] **Step 3: NimBLE soak** with the central role enabled (the headline risk): run the harness BLE loop + the display for the soak duration; assert no heap starvation / hang. If heap headroom is insufficient, fall back to scan-only-when-idle and document.
- [ ] **Step 4: Commit** any tuning.

---

# PHASE 5 — SignalK plugin migration

### Task 5.1: Plugin adopts the JS protocol lib

**Files:**
- Modify: `signalk/plugins/signalk-espdisp-manager/` (`package.json` add `@espdisp/proto` workspace dep; `lib/manager.js` + `index.js`)
- Create: `signalk/plugins/signalk-espdisp-manager/lib/proto-control.js`

- [ ] **Step 1:** Add `@espdisp/proto` (the `proto/js` package) as a local dependency; import `validate`, `versionCompatible`, `authOk`.
- [ ] **Step 2: `proto-control.js`** — implement discovery (mDNS browse `_espdisp._tcp` via the SK server's bonjour, or the existing announce listener) producing `DeviceRecord`s, and control (`attach`/`switch`/`detach`) via HTTP `POST /api/p2p/*` against each device, validated by the ajv validators. Reframe the existing `screen.set` command to issue a protocol `switch`; keep the registry/naming layer but mark each device's `pv`/transports.
- [ ] **Step 3: Tests** — `test/proto-control.test.js`: validate outbound messages against the schema; mock a device HTTP server and assert attach→switch→detach round-trip; `npm test` green.
- [ ] **Step 4:** `cd signalk/plugins/signalk-espdisp-manager && npm test` → PASS. **Step 5: Commit** `feat(plugin): migrate discovery+control onto the protocol lib`.

---

# PHASE 6 — Knob integration (build-verified; covered transitively by the harness)

### Task 6.1: Wire the knob menu to the protocol registry

**Files:**
- Modify: `src/knob_remote.cpp` (control via `proto_ip`/`proto_ble` instead of/in addition to the manager `screen.set`), `src/knob_input.cpp` or `main.cpp` (a `ctl color #RRGGBB` console command persisting the controller color in NVS `proto`/`color`)

- [ ] **Step 1:** The knob's `switch_view` for a remote entry now routes through the protocol (IP attach/switch/detach via the worker; BLE on-demand central when off-grid), carrying the knob's configured color + the shared key. Local entry 0 unchanged.
- [ ] **Step 2:** Add the `ctl color`/`ctl key` console commands (board-gated, via `handleMainCommand`, persisted NVS), mirroring the `knob counts` command pattern.
- [ ] **Step 3: Build** `pio run -e waveshare-knob-1_8` + `pio run -e esp32-4848s040` → SUCCESS; `pio test -e native` green.
- [ ] **Step 4: Commit** `feat(knob): drive remote displays via the control protocol`.

### Task 6.2: Final verification + docs

- [ ] **Step 1: Full gate:** `pio test -e native` (proto + all) ; `pio run -e esp32-4848s040` ; `pio run -e waveshare-knob-1_8` ; `pio run -e harness-s3-devkitc` ; `make sim` ; `cd proto/js && npm test` ; `cd signalk/plugins/signalk-espdisp-manager && npm test` ; `make proto && git diff --exit-code` ; `make pre-commit` — all green.
- [ ] **Step 2: Docs:** new `docs/control-protocol.md` (the protocol, versioning, auth, transports, the harness procedure, the colored-frame), update README + `docs/remote-knob.md` + `docs/knob-testing.md` (harness as verifier). Embed the control-frame sim shots.
- [ ] **Step 3: Push** `git push origin main`.

---

## Self-review

**Spec coverage:** §2 protocol/codegen → 1.1–1.3, 1.6; §2.4 versioning → 1.4/1.5/1.6; §2.5 auth → 1.4/1.5/3.1; §3 sessions+frame → 1.4/1.5, 3.1, 3.2; §4.1 IP → 3.1, 3.3; §4.2 BLE → 4.1, 4.2; §4.3 multi-source registry → 3.3, 4.2; §5 consumers (plugin) → 5.1, (knob) → 6.1, (harness) → Phase 2; §6 shared libs → 1.3/1.4/1.6; §7 build order → phase ordering; §8 testing → 1.5/1.6, 3.4, 4.3, 6.2; §9 memory traps → 3.1 (mutex, in-place), 3.2 (UI-task frame), 4.x (NimBLE central role, 512-B cap, setValue overload, soak).

**Placeholder scan:** Phase 1 is full real code/TDD. Phases 2–6 specify exact files, real interface/struct/handler code, exact build+harness verification, and acceptance; the larger handler bodies (proto_ip.cpp HTTP calls, proto_target.cpp mutex wrap, control_frame.cpp LVGL borders, proto_ble.cpp connect-sequence) are described by their precise contract + the pattern file to mirror — appropriate for integration code that is build- and hardware-(harness)-verified rather than unit-TDD'd. No "TBD/handle edge cases" placeholders.

**Type consistency:** `proto::{Attach,AttachAck,Switch,SwitchAck,Heartbeat,Detach,DeviceRecord,ControlState,Session,ViewRef,SessionTable}` defined in 1.3/1.4 and used unchanged in 1.5, 2.x, 3.x, 4.x. `proto_ip::{get_device,attach,do_switch,heartbeat,detach,get_state}` (2.2) consumed in 2.4, 3.3, 6.1. `proto_target::handle_*` (3.1) reused by web.cpp (3.1) and BLE (4.1) — one handler path for both transports. `from_json`/`to_json` generated names used consistently.

**Open items (from spec §10), to confirm during execution:** codegen edge handling for any future non-subset schema type; BLE adv size budget for `pv`/`role`; measured NimBLE central heap headroom (the gating risk — 4.3 has the fallback); frame style values into the style config; plugin migration depth in 5.1.
