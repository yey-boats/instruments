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
