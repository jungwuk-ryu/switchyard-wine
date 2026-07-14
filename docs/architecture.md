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

Apple Game Porting Toolkit components are outside both repositories. When a user selects a local GPTK installation, the runtime builder fingerprints and overlays the selected redistributable files into that user's local runtime. Those files are never downloaded, committed, or published here.

## Runtime identity

Every generated `switchyard-runtime.json` records the source repository and commit, upstream Wine revision, dirty-tree state and digest, dependency digests, supported PE architectures, installed executable, and hashes of core Wine/PE binaries. A runtime directory name includes the same immutable inputs. Builds happen outside the live path and are promoted with an atomic directory swap only after verification, so changing source or dependencies cannot partially mutate an existing installation.

## Release model

The `main` branch is a linear downstream branch rooted at the revision in `switchyard/upstream-base.txt`. Each Switchyard compatibility change is represented by an ordinary reviewable commit, and Git history is the canonical change record. Source tags identify immutable inputs; they do not imply that Apple or other third-party binaries are included.
