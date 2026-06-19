import { parseDoc } from "./canonicalize";
import { validateConfigStructure } from "./validate";
import { satisfies } from "./satisfy";
import { parseVersion, compatible } from "./version";
import type { ConfigDoc, Issue, Manifest } from "./types";

export * from "./types";
export { parseVersion, compatible } from "./version";
export { expand, countTiles, depth, PRESETS } from "./presets";
export { parseDoc, toCanonicalJson, toYaml } from "./canonicalize";
export { validateConfigStructure, validateManifestStructure } from "./validate";
export { satisfies } from "./satisfy";

export interface ValidationResult { ok: boolean; issues: Issue[]; }

// Full pipeline: structural -> version compat -> capability satisfaction.
// Returns the first failing stage's issues (do not run satisfaction on a
// structurally-invalid doc).
export function validateDocument(text: string, manifest: Manifest, className: string): ValidationResult {
  const doc = parseDoc(text) as ConfigDoc;
  const structural = validateConfigStructure(doc);
  if (structural.length) return { ok: false, issues: structural };
  if (!compatible(parseVersion(doc.midl), parseVersion(manifest.midl)))
    return { ok: false, issues: [{ path: "/midl", message: `incompatible MIDL ${doc.midl} vs device ${manifest.midl}` }] };
  const sat = satisfies(doc, manifest, className);
  return { ok: sat.length === 0, issues: sat };
}
