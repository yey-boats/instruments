export interface MidlVersion { major: number; minor: number; build: number; }

export type Source =
  | { kind: "signalk"; path: string }
  | { kind: "local"; id: string }
  | { kind: "const"; value: unknown }
  | { kind: "computed"; expr?: string };

export interface Action { kind: "put" | "nav" | "command"; target?: string; value?: unknown; }

export interface Element {
  type: string;
  name?: string;
  bindings?: Record<string, Source>;
  style?: Record<string, unknown>;
  format?: Record<string, unknown>;
  markers?: Array<{ glyph?: string; [k: string]: unknown }>;
  action?: Action;
  zoom?: string;
}

export type Node =
  | { element: string }
  | { dir: "row" | "col"; children: Node[]; weights?: number[] }
  | { rows: number; cols: number; cells: Node[] }
  | { preset: string; slots?: string[] };

export interface Variant { class: string; layout: Node; }

export interface Screen {
  id: string;
  title?: string;
  elements: Record<string, Element>;
  layout: Node;
  variants?: Variant[];
}

export interface ConfigDoc {
  midl: string;
  settings?: Record<string, unknown>;
  defaults?: Record<string, unknown>;
  screens: Screen[];
  alarms?: unknown[];
  presets?: Record<string, unknown>;
}

export interface DeviceClass {
  id: string;
  width?: number;
  height?: number;
  maxTiles: number;
  maxDepth: number;
  presets?: string[];
  elements?: string[];
}

export interface ElementCap {
  type: string;
  bindings?: string[];
  attrs?: string[];
  units?: string[];
  glyphs?: string[];
}

export interface Manifest {
  midl: string;
  firmwareVersion?: string;
  board: string;
  classes: DeviceClass[];
  elements: ElementCap[];
  sources?: string[];
  actionKinds?: string[];
  presets?: string[];
  fonts?: number[];
  themes?: string[];
}

export interface Issue { path: string; message: string; }
