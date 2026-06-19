import type { ConfigDoc, Element, ElementCap, Issue, Manifest, Node, Screen, Source } from "./types";
import { expand, countTiles, depth } from "./presets";

function resolveLayout(screen: Screen, className: string): Node {
  const v = screen.variants?.find((x) => x.class === className);
  return v ? v.layout : screen.layout;
}

function elementIds(n: Node, out: string[]): void {
  if ("element" in n) out.push(n.element);
  else if ("children" in n) n.children.forEach((c) => elementIds(c, out));
  else if ("cells" in n) n.cells.forEach((c) => elementIds(c, out));
}

function checkElement(
  el: Element,
  path: string,
  allowedTypes: Set<string>,
  capByType: Map<string, ElementCap>,
  allowedSources: Set<string>,
  allowedActions: Set<string>,
  issues: Issue[],
): void {
  if (!allowedTypes.has(el.type)) {
    issues.push({ path: `${path}/type`, message: `element type not supported: ${el.type}` });
    return;
  }
  const cap = capByType.get(el.type);
  if (el.bindings) {
    for (const [field, src] of Object.entries(el.bindings)) {
      if (cap?.bindings && !cap.bindings.includes(field))
        issues.push({ path: `${path}/bindings/${field}`, message: `binding field not supported by ${el.type}: ${field}` });
      if (!allowedSources.has((src as Source).kind))
        issues.push({ path: `${path}/bindings/${field}`, message: `source kind not supported: ${(src as Source).kind}` });
    }
  }
  if (el.action && allowedActions.size > 0 && !allowedActions.has(el.action.kind))
    issues.push({ path: `${path}/action`, message: `action kind not supported: ${el.action.kind}` });
}

export function satisfies(config: ConfigDoc, manifest: Manifest, className: string): Issue[] {
  const issues: Issue[] = [];
  const cls = manifest.classes.find((c) => c.id === className);
  if (!cls) return [{ path: "/", message: `class not supported: ${className}` }];

  const allowedTypes = new Set(cls.elements ?? manifest.elements.map((e) => e.type));
  const capByType = new Map(manifest.elements.map((e) => [e.type, e]));
  const allowedSources = new Set(manifest.sources ?? ["signalk"]);
  const allowedActions = new Set(manifest.actionKinds ?? []);

  config.screens.forEach((screen, si) => {
    let tree: Node;
    try {
      tree = expand(resolveLayout(screen, className));
    } catch (e) {
      issues.push({ path: `/screens/${si}/layout`, message: (e as Error).message });
      return;
    }
    const tiles = countTiles(tree);
    if (tiles > cls.maxTiles)
      issues.push({ path: `/screens/${si}/layout`, message: `too many tiles: ${tiles} > ${cls.maxTiles}` });
    const d = depth(tree);
    if (d > cls.maxDepth)
      issues.push({ path: `/screens/${si}/layout`, message: `nesting too deep: ${d} > ${cls.maxDepth}` });

    const ids: string[] = [];
    elementIds(tree, ids);
    for (const id of ids) {
      const el = screen.elements[id];
      if (!el) {
        issues.push({ path: `/screens/${si}`, message: `layout references unknown element: ${id}` });
        continue;
      }
      checkElement(el, `/screens/${si}/elements/${id}`, allowedTypes, capByType, allowedSources, allowedActions, issues);
    }
  });
  return issues;
}
