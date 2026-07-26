# Application compatibility

This document records point-in-time application compatibility results for Switchyard Wine.
Each row is a self-contained verification record with its own confirmation date, runtime, host
environment, and launch or graphics path. The current entries repeat the selected runtime and
host recorded when the results were supplied, but these fields remain per-application so later
checks can update independently.

Some historical rows were supplied by the user rather than independently retested. New checks
record the application build or Steam App ID and the exercised launch path when that information
is available.

## Compatibility results

| Application | Status | Last confirmed | Runtime | Host environment | Launch / graphics path |
| --- | --- | --- | --- | --- | --- |
| Blender 5.2.0 LTS | Working — Mesa llvmpipe software rendering is CPU-bound | 2026-07-26 | Switchyard Wine source `1917e45cc0a6a440b124aa16e021d49998966fd9` · GPTK 4 runtime `switchyard-local-wow64-x86_64-1917e45cc0a6-05ce84e4cb6f-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11` | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Windows Steam client (App ID 365670) · `WINE_OPENGL_DRIVER=llvmpipe` · Mesa 26.1.1 OpenGL 4.6 |
| Heartopia (Steam build 24086143) | Working | 2026-07-25 | Switchyard Wine source `d4e28800b11ed95e2c6483f713cd9d10399aa132` · GPTK 4 runtime `switchyard-local-wow64-x86_64-d4e28800b11e-05ce84e4cb6f-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11` | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Windows Steam client (App ID 4025700) · GPTK 4.0 beta 1 D3D11/DXGI → D3DMetal · 32-minute post-login live-world soak |
| Terraria (Steam build 22266454) | Working | 2026-07-26 | Switchyard Wine source `1917e45cc0a6a440b124aa16e021d49998966fd9` · GPTK 4 runtime `switchyard-local-wow64-x86_64-1917e45cc0a6-05ce84e4cb6f-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11` | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Windows Steam client (App ID 105600) · GPTK 4.0 beta 1 · 32-bit Steam overlay hotpatch path |
| Steam (client build 1784778118) | Working | 2026-07-27 | Switchyard Wine source `9a822257b1f2660254d38e0443db474b989d7b78` · notarized Wine-only runtime `switchyard-local-wow64-x86_64-9a822257b1f2-no-gptk-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11` | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Direct Windows Steam client · externally selected GPTK 4.0 beta 1 · update verification completed, automatic login succeeded, and the Store was visually rendered |
| Battle.net 2.52.5.17620 | Working | 2026-07-26 | Switchyard Wine source `1917e45cc0a6a440b124aa16e021d49998966fd9` · GPTK 4 runtime `switchyard-local-wow64-x86_64-1917e45cc0a6-05ce84e4cb6f-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11` | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Direct `Battle.net.exe` · GPTK 4.0 beta 1 · automatic login and CEF/ANGLE GPU/renderer processes |
| KakaoTalk | Working | 2026-07-26 | Switchyard Wine source `1917e45cc0a6a440b124aa16e021d49998966fd9` · GPTK 4 runtime `switchyard-local-wow64-x86_64-1917e45cc0a6-05ce84e4cb6f-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11` | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Direct `KakaoTalk.exe` · shared Steam prefix · GPTK 4.0 beta 1 |
| Goose Goose Duck (Steam build 23328520) | Blocked — Easy Anti-Cheat module mapping failed; excluded from runtime diagnosis at user request | 2026-07-25 | Switchyard Wine source `b9c90d58679b8022915ff040e139143064477111` · GPTK 4 runtime `switchyard-local-wow64-x86_64-b9c90d58679b-05ce84e4cb6f-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11` | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Windows Steam client (App ID 1568590) · Easy Anti-Cheat launcher |
| Poppy Playtime (Steam build 21905565) | Working | 2026-07-26 | Switchyard Wine source `1917e45cc0a6a440b124aa16e021d49998966fd9` · GPTK 4 runtime `switchyard-local-wow64-x86_64-1917e45cc0a6-05ce84e4cb6f-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11` | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Windows Steam client (App ID 1721470) · launcher plus Steam-tracked game executable · GPTK 4.0 beta 1 |
| Unturned 3.26.3.4 (Steam build 24080152) | Partially working — BattlEye is unsupported; cancelling it reaches the non-BattlEye main menu | 2026-07-25 | Switchyard Wine source `d08996c05014888ca1c0f81b95b6d5555f32efa5` · GPTK 4 runtime `switchyard-local-wow64-x86_64-d08996c05014-05ce84e4cb6f-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11` | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Windows Steam client (App ID 304930) · BattlEye cancelled · D3D11/DXGI → D3DMetal · asset load and `Menu UI ready` verified |
| Bro Falls: Ultimate Showdown | Working | 2026-07-21 | Switchyard Wine 11.12 (`783c55de9a5b`) | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Steam · GPTK 3.0-enabled runtime |
| Pratfall | Working | 2026-07-21 | Switchyard Wine 11.12 (`783c55de9a5b`) | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Steam · Vulkan renderer |
| Overwatch | Working | 2026-07-26 | Switchyard Wine source `1917e45cc0a6a440b124aa16e021d49998966fd9` · GPTK 4 runtime `switchyard-local-wow64-x86_64-1917e45cc0a6-05ce84e4cb6f-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11` | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Direct `Overwatch.exe` with Battle.net active · GPTK 4.0 beta 1 D3D11/DXGI → D3DMetal |
| Supermarket Together | Working | 2026-07-21 | Switchyard Wine 11.12 (`783c55de9a5b`) | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Steam · GPTK 3.0-enabled runtime |
| Rockstar Games Launcher 1.0.108.2970 | Working | 2026-07-27 | Switchyard Wine source `9a822257b1f2660254d38e0443db474b989d7b78` · notarized Wine-only runtime `switchyard-local-wow64-x86_64-9a822257b1f2-no-gptk-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11` | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Direct `LauncherPatcher.exe` · externally selected GPTK 4.0 beta 1 · signed-in library and GTA V tile visually rendered without the transparent-window regression |
| Google Chrome 150.0.7871.184 | Working | 2026-07-27 | Switchyard Wine source `9a822257b1f2660254d38e0443db474b989d7b78` · notarized Wine-only runtime `switchyard-local-wow64-x86_64-9a822257b1f2-no-gptk-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11` | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Direct `chrome.exe --new-window https://example.com` in the Steam container · externally selected GPTK 4.0 beta 1 · browser chrome and page body visually rendered |
| Grand Theft Auto V Enhanced 1.0.1158.13 | Working | 2026-07-27 | Switchyard Wine source `9a822257b1f2660254d38e0443db474b989d7b78` · GitHub-downloaded notarized Wine-only runtime `switchyard-local-wow64-x86_64-9a822257b1f2-no-gptk-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11` · GPTK 3 runtime `switchyard-local-wow64-x86_64-61fd4fb53479-4835e0c18f43-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11` | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Rockstar Games Launcher · `PlayGTAV.exe -nobattleye` · `-onSteamDeck` · GitHub release plus externally selected GPTK 4.0 beta 1, and local GPTK 3.0 runtime · Direct3D 12 / D3DMetal · Online/GTA+/Story selector visually reached |
| RV There Yet? 1.2.0.17491 (Steam build 22864294) | Working — first-launch PSO precompilation took 96 seconds | 2026-07-26 | Switchyard Wine source `1917e45cc0a6a440b124aa16e021d49998966fd9` · GPTK 4 runtime `switchyard-local-wow64-x86_64-1917e45cc0a6-05ce84e4cb6f-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11` | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Windows Steam default launch (App ID 3949040; no game launch options) · GPTK 4.0 beta 1 · Direct3D 12 / SM6 |
| Epic Games Launcher 20.1.5-55906714 | Working — Mesa llvmpipe software rendering is CPU-bound | 2026-07-26 | Switchyard Wine source `1917e45cc0a6a440b124aa16e021d49998966fd9` · Wine-only runtime `switchyard-local-wow64-x86_64-1917e45cc0a6-no-gptk-b4525679e7da-9245db166022-37a4f0cfb0fb-4fbf9011be92-1b749a3204a2-b40553c5dc41-62f8fecd4b11` | macOS 26.5.2 (25F84) · Apple M5 Pro · Rosetta 2 | Direct `EpicGamesLauncher.exe` · dedicated Switchyard container · `WINE_OPENGL_DRIVER=llvmpipe` · Mesa D3D11 feature level 11_1 · automatic login, entitlement loading, and self-update verification |

## Status interpretation

`Working` means that no blocking issue was observed within the recorded workflow. A visible window
or live process is not sufficient: current checks require an application-specific load-completion
milestone, temporal progress, a safe input response, and no new crash or unexpected Steam tracking
loss. Historical rows without those details should not be interpreted as a guarantee that every
feature works. Status notes are reserved for confirmed bugs and material limitations, not successful
behavior or untested workflows.

Compatibility may change with application updates, runtime revisions, macOS versions, hardware,
or graphics layers. Future updates should record the application version and distribution path,
verification date, runtime revision, host environment, and any known limitations.

The shared 2026-07-21 runtime revision expands to
`783c55de9a5b631b6710ed690ec696654a7d17b9`. It uses the `switchyard-wow64-pe` profile
with `i386` and `x86_64` PE support. GPTK refers to the user-provided Apple Game Porting
Toolkit 3.0 overlay; it is not distributed by this repository.
