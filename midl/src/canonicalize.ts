import { parse as parseYaml, stringify as stringifyYaml } from "yaml";

// YAML is a JSON superset, so this parses both YAML and JSON input.
export function parseDoc(text: string): unknown {
  return parseYaml(text);
}

export function toCanonicalJson(doc: unknown): string {
  return JSON.stringify(doc, null, 2);
}

export function toYaml(doc: unknown): string {
  return stringifyYaml(doc);
}
