# Repository Guidelines

## Commit messages

Follow the commit-message style already established by `wine-mirror/wine`. Do not
use Conventional Commit prefixes such as `feat`, `fix`, or `refactor`.

- Format subjects as `<component>: <Imperative sentence>.`
- Use the Wine component or directory name as the lowercase scope. For test-only
  changes, use `<component>/tests`; separate multiple components with commas.
- Start the summary with a capitalized imperative verb and end it with a period.
- Keep each commit to one logical change and keep the subject concise.
- After a blank line, use the body to explain the reason for the change and any
  behavior that is not clear from the diff. Put trailers after another blank line.
- Preserve the original Wine subject, body, and trailers when importing an
  upstream commit. Do not rewrite it into Conventional Commit style.

Examples:

```text
pdh: Implement PdhGetFormattedCounterArrayA/W.
winemac: Preserve Chrome content during window moves.
ntoskrnl/tests: Add tests for PDO behavior.
dcomp, winemac: Stabilize custom-frame composition.
```

This style applies to all commits in this repository, including build tooling and
documentation changes.

## Worktree tasks

When the user explicitly identifies a task as worktree work—for example, by saying
`worktree 작업입니다.` or equivalent wording—complete the entire task in a newly
created, dedicated Git worktree:

1. Create the worktree and its task branch before making implementation changes.
2. Perform all implementation, edits, patches, verification, review fixes, and
   commits in that worktree.
3. After the task is complete, merge the task branch into `main`.
4. Verify that `main` contains the completed change, then remove the dedicated
   worktree and delete the temporary task branch when safe.

Do not report a worktree task as complete while its changes exist only on the
worktree branch. If unrelated user changes or repository state prevent a safe
merge or cleanup, preserve the affected work and report the exact blocker instead
of forcing the operation.

## Compatibility documentation

When application testing establishes whether a program works, update
`docs/compatibility.md` before finishing the task.

- Keep the document in English.
- Maintain one self-contained row per application.
- Record the compatibility status, confirmation date in `YYYY-MM-DD` format,
  exact Switchyard Wine runtime revision, host environment, launch or graphics
  path, and any known limitations.
- Keep the status concise. Use only `Working` when no problem was observed; do
  not describe expected successful behavior such as smooth rendering, responsive
  controls, or individual workflows that completed normally.
- Add text after the status only for a confirmed bug or material limitation, for
  example `Working — <known limitation>` or `Partially working — <known
  limitation>`. Do not list untested workflows as limitations.
- Do not include a time of day in the confirmation date.
- Do not reuse another application's runtime or environment unless the same
  configuration was actually verified.
