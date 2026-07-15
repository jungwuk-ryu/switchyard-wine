# Architecture

`switchyard-wine` is the source and build boundary for Switchyard's Wine runtime. It is deliberately separate from the SwiftUI application and from user-provided Apple software.

## Repository boundary

This repository owns:

- the downstream Wine commit history;
- the pinned upstream Wine base;
- source-integrity checks;
- the reproducible local runtime builder; and
- Wine-specific provenance and build documentation.

The Switchyard application owns container state, runtime selection, process execution, logs, and the macOS user interface. The app invokes Wine only through its external runner. It does not link against Wine.

## Multilingual font baseline

The runtime carries an unmodified, hash-pinned Noto font baseline under the SIL Open Font License 1.1. Wine loads it from its shared data directory for every container. DirectWrite range fallback, GDI SystemLink entries, and non-clobbering Windows-family replacements cooperate so an application-installed Microsoft font still wins while a clean prefix can render multilingual text immediately. Font binaries remain downloaded build inputs rather than tracked source files; the runtime contains their manifest and license notices.

Apple Game Porting Toolkit components are outside both repositories. When a user selects a local GPTK installation, the runtime builder fingerprints and overlays the selected redistributable files into that user's local runtime. Those files are never downloaded, committed, or published here.

## Runtime identity

Every generated `switchyard-runtime.json` records the source repository and commit, upstream Wine revision, dirty-tree state and digest, dependency digests, supported PE architectures, installed executable, and hashes of core Wine/PE binaries. A runtime directory name includes the same immutable inputs. Builds happen outside the live path and are promoted with an atomic directory swap only after verification, so changing source or dependencies cannot partially mutate an existing installation.

## Release model

The `main` branch is a linear downstream branch rooted at the revision in `switchyard/upstream-base.txt`. That revision is a compatibility baseline, not a promise to follow WineHQ's release cadence. Upstream fixes may be cherry-picked or adapted individually, and the base advances only after established Switchyard launcher and game workflows have been checked for regressions. Preserving known-working behavior takes priority over adopting a newer Wine version.

Each Switchyard compatibility change is represented by an ordinary reviewable commit, and Git history is the canonical change record. A change does not need to be suitable for upstream Wine to remain in the downstream branch, but it must keep its rationale and validation evidence reviewable. Source tags identify immutable inputs; they do not imply that Apple or other third-party binaries are included.
