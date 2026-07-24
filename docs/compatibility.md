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
| Blender 5.2.0 LTS | Working — Mesa llvmpipe software rendering is CPU-bound | 2026-07-24 | Switchyard Wine development runtime (`b94f2c85edd85faa31df989d24fd97eed53cd798`) | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Windows Steam client (App ID 365670) · container-wide `WINE_OPENGL_DRIVER=llvmpipe` · Mesa 26.1.1 OpenGL 4.6 |
| Heartopia | Working | 2026-07-24 | Switchyard Wine runtime `c408b7a2789dae37dc228831bdf621a60c757e22` (GPTK 4: `switchyard-local-wow64-x86_64-c408b7a2789d-05ce84e4cb6f-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11`; GPTK 3: `switchyard-local-wow64-x86_64-c408b7a2789d-4835e0c18f43-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11`) | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Windows Steam client (App ID 4025700) · GPTK 4.0 beta 1 and GPTK 3.0 D3D11/DXGI → D3DMetal · unsafe Apple Metal HUD suppressed; D3DMetal HUD statistics retained |
| Terraria | Working | 2026-07-21 | Switchyard Wine 11.12 (`783c55de9a5b`) | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Steam · GPTK 3.0-enabled runtime |
| Steam | Working | 2026-07-24 | Switchyard Wine runtime (`switchyard-local-wow64-x86_64-906d2007a625-05ce84e4cb6f-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11`) | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Direct Windows Steam client · GPTK 4.0 beta 1 environment · preserved Wine graphics fallback for CEF helpers |
| Battle.net 2.52.5.17620 | Working | 2026-07-25 | Switchyard Wine 11.12 (source `930ac82bc07a7262dfe75762a4d3f8d476baf810`; GPTK 4 runtime `switchyard-local-wow64-x86_64-930ac82bc07a-05ce84e4cb6f-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11`; GPTK 3 runtime `switchyard-local-wow64-x86_64-930ac82bc07a-4835e0c18f43-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11`) | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Direct `Battle.net.exe` · GPTK 4.0 beta 1 and GPTK 3.0 · CEF/ANGLE foreign WGL surface → macOS CAContext |
| KakaoTalk | Working | 2026-07-24 | Switchyard Wine runtime (`switchyard-local-wow64-x86_64-906d2007a625-05ce84e4cb6f-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11`) | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Direct `KakaoTalk.exe` · shared Steam prefix · GPTK 4.0 beta 1 environment |
| Bro Falls: Ultimate Showdown | Working | 2026-07-21 | Switchyard Wine 11.12 (`783c55de9a5b`) | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Steam · GPTK 3.0-enabled runtime |
| Pratfall | Working | 2026-07-21 | Switchyard Wine 11.12 (`783c55de9a5b`) | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Steam · Vulkan renderer |
| Overwatch | Working | 2026-07-24 | Switchyard Wine runtime (`switchyard-local-wow64-x86_64-906d2007a625-05ce84e4cb6f-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11`) | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Direct `Overwatch.exe` with Battle.net active · GPTK 4.0 beta 1 D3D11/DXGI → D3DMetal |
| Supermarket Together | Working | 2026-07-21 | Switchyard Wine 11.12 (`783c55de9a5b`) | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Steam · GPTK 3.0-enabled runtime |
| Rockstar Games Launcher 1.0.108.2970 | Working | 2026-07-24 | Switchyard Wine 11.12 (source `3cfb3445ac6b0c50f831ecb9ec621dfc0954c3da`; runtime `switchyard-local-wow64-x86_64-3cfb3445ac6b-4835e0c18f43-b4525679e7da-9245db166022-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11`) | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Switchyard `switchyard-runner` · direct `Launcher.exe` · `WINEDEBUG=-all,+timestamp,err+all,warn+all` · GPTK 3.0 environment |
| RV There Yet? 1.2.0.17491 | Working | 2026-07-23 | Switchyard Wine development runtime (`switchyard-local-wow64-x86_64-d9ce85d3f02b-dirty-daf7e86ab65b-4835e0c18f43-b4525679e7da-9245db166022-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11`) | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Windows Steam default launch (App ID 3949040; no game launch options) · GPTK 3.0 · Direct3D 12 / SM6 |
| Epic Games Launcher Setup 20.1.0 | Working | 2026-07-22 | Switchyard Wine development runtime (`eeb99326c247dca4a78b05ddaef13b3ac08dc24e`) | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Direct MSI (`epic-games-launcher-20-1-0.msi`) · dedicated Switchyard container · Wine graphics path with GPTK overlay disabled |

## Status interpretation

`Working` means that no blocking issue was reported within the workflow exercised by the user.
Application versions, detailed test steps, session duration, and peripheral combinations were not
recorded, so this status should not be interpreted as a guarantee that every feature works. Status
notes are reserved for confirmed bugs and material limitations, not successful behavior or untested
workflows.

Compatibility may change with application updates, runtime revisions, macOS versions, hardware,
or graphics layers. Future updates should record the application version and distribution path,
verification date, runtime revision, host environment, and any known limitations.

The shared 2026-07-21 runtime revision expands to
`783c55de9a5b631b6710ed690ec696654a7d17b9`. It uses the `switchyard-wow64-pe` profile
with `i386` and `x86_64` PE support. GPTK refers to the user-provided Apple Game Porting
Toolkit 3.0 overlay; it is not distributed by this repository.
