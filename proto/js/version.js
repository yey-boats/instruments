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
