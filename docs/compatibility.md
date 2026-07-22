# Application compatibility

This document records point-in-time application compatibility results for Switchyard Wine.
Each row is a self-contained verification record with its own confirmation date, runtime, host
environment, and launch or graphics path. The current entries repeat the selected runtime and
host recorded when the results were supplied, but these fields remain per-application so later
checks can update independently.

Unless a row says otherwise, the results were supplied by the user and were not independently
retested while preparing this document. Application versions and detailed test steps were not
recorded for those entries.

## Compatibility results

| Application | Status | Last confirmed | Runtime | Host environment | Launch / graphics path |
| --- | --- | --- | --- | --- | --- |
| Heartopia | Working — the title screen rendered and the Steam Overlay opened with Shift+Tab; broader gameplay was not reverified. | 2026-07-23 | Switchyard Wine overlay test runtime (base `eeb99326c247`; hotpatch build `f9cc05b718`) | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Windows Steam client (app 4025700) · GPTK 3.0 D3D11/DXGI · `GameOverlayRenderer64.dll` |
| Terraria | Working | 2026-07-21 | Switchyard Wine 11.12 (`783c55de9a5b`) | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Steam · GPTK 3.0-enabled runtime |
| Steam | Working — window movement and STORE/LIBRARY/COMMUNITY hover menus were verified without white or blank regions; broader client and game workflows were not reverified. | 2026-07-22 | Switchyard Wine development runtime (`d5ec127bafb632ee717819db8eba1d9fccc43e23`, source tree `6207795aa523`) | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Windows Steam client · CEF remote-DIB surfaces → persistent HWND-scoped CAContext layer · GPTK 3.0 environment with Wine graphics fallback |
| Battle.net 2.52.5.17620 | Working — the authenticated home and game library, friends list, and friend chat were verified; opening chat no longer raised the prior 32-bit `0xe0000008` debugger. An Overwatch download was active, but installation completion and game launch were not tested. | 2026-07-23 | Switchyard Wine diagnostic build (`9a1ff5894dc3`, base `16ede5945aed`) | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Direct `Battle.net.exe` · CEF/ANGLE GLES2 fallback · default 4 GiB WoW64 address space with `WINE_LARGE_ADDRESS_AWARE=0` opt-out · CEF scale forced to 1 by the app |
| KakaoTalk | Working | 2026-07-21 | Switchyard Wine 11.12 (`783c55de9a5b`) | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Direct executable · shared Steam prefix |
| Bro Falls: Ultimate Showdown | Working | 2026-07-21 | Switchyard Wine 11.12 (`783c55de9a5b`) | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Steam · GPTK 3.0-enabled runtime |
| Pratfall | Working | 2026-07-21 | Switchyard Wine 11.12 (`783c55de9a5b`) | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Steam · Vulkan renderer |
| Overwatch | Working | 2026-07-21 | Switchyard Wine 11.12 (`783c55de9a5b`) | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Steam · GPTK 3.0-enabled runtime |
| Supermarket Together | Working | 2026-07-21 | Switchyard Wine 11.12 (`783c55de9a5b`) | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Steam · GPTK 3.0-enabled runtime |
| Rockstar Games Launcher | Partially working — the login flow was not reverified. | 2026-07-22 | Switchyard Wine development runtime (`853f1082db553eb94632c245d630291c0642b810`) | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Direct launcher · CEF foreign-surface composition · GPTK 3.0 environment |

## Status interpretation

`Working` means that no blocking issue was reported within the workflow exercised by the user.
Application versions, detailed test steps, session duration, and peripheral combinations were not
recorded, so this status should not be interpreted as a guarantee that every feature works.

Compatibility may change with application updates, runtime revisions, macOS versions, hardware,
or graphics layers. Future updates should record the application version and distribution path,
verification date, runtime revision, host environment, and any known limitations.

The shared 2026-07-21 runtime revision expands to
`783c55de9a5b631b6710ed690ec696654a7d17b9`. It uses the `switchyard-wow64-pe` profile
with `i386` and `x86_64` PE support. GPTK refers to the user-provided Apple Game Porting
Toolkit 3.0 overlay; it is not distributed by this repository.
