# Repository Agent Notes

## Completion Discipline

- Do not report implementation work as complete while the worktree still has
  uncommitted changes from that work.
- Before a final status for implementation or deployment work, check
  `git status --short`.
- If changes should remain uncommitted, say that explicitly and list the
  pending files instead of calling the task done.
- When the user asks to commit, include all files required by the completed
  work, run the relevant tests, and report the commit hash.

## Local Checks

- Run `make pre-commit` before committing; it is the same lint command used by
  the local git hook and CI.
- Install the repo hook with `make hooks-install` if `core.hooksPath` is not
  already `.githooks`.
- If `make pre-commit` fails, fix formatting with `make format` or address the
  reported syntax/version issue before running tests or committing.
