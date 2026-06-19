import { test, expect } from "vitest";
import { parseVersion, compatible } from "../src/version";

test("parseVersion parses MAJOR.MINOR.BUILD", () => {
  expect(parseVersion("1.2.37")).toEqual({ major: 1, minor: 2, build: 37 });
});

test("parseVersion rejects malformed", () => {
  expect(() => parseVersion("1.2")).toThrow();
});

test("same major, older config minor runs on newer build", () => {
  expect(compatible(parseVersion("1.2.0"), parseVersion("1.5.9"))).toBe(true);
});

test("newer config minor on older build is incompatible", () => {
  expect(compatible(parseVersion("1.6.0"), parseVersion("1.5.0"))).toBe(false);
});

test("different major is incompatible", () => {
  expect(compatible(parseVersion("1.0.0"), parseVersion("2.0.0"))).toBe(false);
});
