#!/usr/bin/env python3
"""Report how far the pinned midl submodule is behind midl main.

Usage:
    python3 tools/midl_drift.py [--remote URL] [--github]

Exit codes:
    0  pinned == midl main, or pinned is an ancestor of midl main (behind).
       Behind is reported loudly (with --github: an ::error:: annotation and
       a job-summary table) but does not fail: the daily
       .github/workflows/midl-bump.yml opens the bump PR, and an unrelated PR
       should not go red because midl moved.
    1  pinned is NOT on midl main (a feature-branch or force-pushed-away
       commit): the firmware embeds a midl nobody merged. Fails CI.
    2  cannot resolve the commits (no network, no submodule).

The pinned commit is the superproject gitlink (`git rev-parse HEAD:midl`).
Needs the midl submodule checked out (history is fetched from --remote).
"""

import os
import subprocess
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
MIDL_DIR = os.path.join(REPO_ROOT, "midl")
DEFAULT_REMOTE = "https://github.com/yey-boats/midl.git"


def git(*args, cwd=REPO_ROOT, check=True):
    r = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True)
    if check and r.returncode != 0:
        raise RuntimeError("git %s: %s" % (" ".join(args), r.stderr.strip()))
    return r


def main(argv):
    remote = DEFAULT_REMOTE
    if "--remote" in argv:
        remote = argv[argv.index("--remote") + 1]
    gh = "--github" in argv
    try:
        pinned = git("rev-parse", "HEAD:midl").stdout.strip()
        if not os.path.exists(os.path.join(MIDL_DIR, ".git")):
            raise RuntimeError("midl submodule not checked out (git submodule update --init midl)")
        # CI checks the submodule out at depth 1: without its history every
        # pin that midl main has moved past looks "not on main" (red) instead
        # of "behind" (the warning-only case). midl is small; fetch it whole.
        shallow = git("rev-parse", "--is-shallow-repository", cwd=MIDL_DIR).stdout.strip() == "true"
        git("fetch", "--quiet", *(["--unshallow"] if shallow else []), remote, "main", cwd=MIDL_DIR)
        main_sha = git("rev-parse", "FETCH_HEAD", cwd=MIDL_DIR).stdout.strip()
    except RuntimeError as e:
        print("midl drift: %s" % e)
        if gh:
            print("::warning title=midl drift::could not check: %s" % e)
        return 2

    summary = os.environ.get("GITHUB_STEP_SUMMARY") if gh else None

    def write_summary(text):
        if summary:
            with open(summary, "a", encoding="utf-8") as f:
                f.write(text + "\n")

    if pinned == main_sha:
        print("midl pinned at %s = midl main" % pinned[:12])
        write_summary("midl submodule is at midl main (`%s`)." % pinned[:12])
        return 0

    on_main = git("merge-base", "--is-ancestor", pinned, main_sha, cwd=MIDL_DIR,
                  check=False).returncode == 0
    if not on_main:
        msg = ("pinned midl %s is not on midl main %s: bump the submodule to a merged "
               "commit (make midl-latest)" % (pinned[:12], main_sha[:12]))
        print("midl drift: " + msg)
        if gh:
            print("::error title=midl not on main::" + msg)
        write_summary("**midl submodule is not on midl main**: " + msg)
        return 1

    log = git("log", "--oneline", "--no-decorate", "%s..%s" % (pinned, main_sha),
              cwd=MIDL_DIR).stdout.strip().splitlines()
    msg = ("pinned midl %s is %d commit(s) behind midl main %s: run `make midl-latest` "
           "(the daily midl-bump workflow opens the PR)" % (pinned[:12], len(log), main_sha[:12]))
    print("midl drift: " + msg)
    for line in log[:30]:
        print("  " + line)
    if gh:
        print("::error title=midl behind main::" + msg)
    write_summary("### midl submodule behind midl main\n\n" + msg + "\n\n```\n" +
                  "\n".join(log[:30]) + "\n```")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
