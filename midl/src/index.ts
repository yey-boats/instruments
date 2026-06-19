import { parseDoc } from "./canonicalize";
import { validateConfigStructure, validateManifestStructure } from "./validate";
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

// Full pipeline: manifest well-formedness -> config structural -> version
// compat -> capability satisfaction. Returns the first failing stage's
// issues. The manifest is checked first so a malformed manifest version
// surfaces as an issue rather than throwing out of parseVersion below.
export function validateDocument(text: string, manifest: Manifest, className: string): ValidationResult {
  const manifestIssues = validateManifestStructure(manifest);
  if (manifestIssues.length)
    return { ok: false, issues: manifestIssues.map((i) => ({ path: `/manifest${i.path === "/" ? "" : i.path}`, message: i.message })) };

  const doc = parseDoc(text) as ConfigDoc;
  const structural = validateConfigStructure(doc);
  if (structural.length) return { ok: false, issues: structural };

  if (!compatible(parseVersion(doc.midl), parseVersion(manifest.midl)))
    return { ok: false, issues: [{ path: "/midl", message: `incompatible MIDL ${doc.midl} vs device ${manifest.midl}` }] };

  const sat = satisfies(doc, manifest, className);
  return { ok: sat.length === 0, issues: sat };
}
