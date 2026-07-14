# Contributing

Contributions should keep the downstream Wine history reviewable and suitable for eventual upstreaming.

## Workflow

1. Branch from `main`.
2. Make one logical Wine change per commit.
3. Explain the rationale, provenance, upstream status, and build or test evidence in the commit message.
4. Run `./switchyard/verify_source.sh`.
5. Build the affected Wine components or a complete runtime on Apple Silicon.
6. Open a pull request describing the interoperability workload without including proprietary binaries, credentials, or user data.

Wine source commits should follow Wine's subsystem-oriented subject style. Repository tooling and documentation commits should use Conventional Commit subjects such as `build:`, `docs:`, or `chore:`.

Do not commit Apple Game Porting Toolkit components, downloaded launchers, proprietary SDK files, runtime caches, logs, prefixes, credentials, or generated runtime installations. GPTK must remain a user-selected local input.

Complex Wine changes should be developed in source and reviewed as commits. Do not reintroduce a parallel checked-in patch queue; release patch files can be generated from the commit range when needed.
