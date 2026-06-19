import type { MidlVersion } from "./types";

export function parseVersion(s: string): MidlVersion {
  const m = /^(\d+)\.(\d+)\.(\d+)$/.exec(s);
  if (!m) throw new Error(`bad MIDL version: ${s}`);
  return { major: Number(m[1]), minor: Number(m[2]), build: Number(m[3]) };
}

// A config is admissible on a device iff majors match and the config's
// minor is <= the device's minor (forward compat: old config on new build).
export function compatible(config: MidlVersion, device: MidlVersion): boolean {
  return config.major === device.major && config.minor <= device.minor;
}
