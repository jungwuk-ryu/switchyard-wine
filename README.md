<p align="center">
  <img src="docs/assets/switchyard-wine-logo.png" alt="Switchyard Wine logo" width="420">
</p>

# Switchyard Wine

[![GitHub Stars](https://img.shields.io/github/stars/jungwuk-ryu/switchyard-wine?style=flat&logo=github&label=Stars)](https://github.com/jungwuk-ryu/switchyard-wine/stargazers)

<p align="center">
  <a href="docs/i18n/README-ko.md">한국어</a> ·
  <a href="docs/README.md#translations">Other languages</a>
</p>

Switchyard Wine is the Wine runtime behind
[Switchyard](https://github.com/jungwuk-ryu/Switchyard), a macOS app for
keeping Windows games and launchers in managed containers. The app owns the
interface, container state, and runtime selection; this repository owns the
Wine code, build pipeline, and compatibility work underneath it.

Switchyard Wine does not jump to every new Wine release. Its base moves only
after the games and launchers already used with Switchyard have been checked.
Downstream changes stay as ordinary, reviewable Git commits on top of a pinned
WineHQ revision.

## Why this branch exists

- **macOS-specific compatibility fixes.** The branch carries fixes for real
  failure paths seen in Switchyard, including D3DMetal callback and resource
  bridging, macOS MSync synchronization, Chromium/CEF rendering,
  graphics-provider selection, media playback, and multilingual font fallback.
- **A runtime with a verifiable identity.** Builds pin and hash external inputs,
  assemble outside the live runtime, and publish only after verification. Each
  runtime records its source revision, dependency digests, architecture, and
  core binary hashes.
- **Regression tests beside the fixes.** The repository exercises D3DMetal,
  native callbacks, MSync, Steam overlay hotpatching, TLS, media, OpenGL, and
  runtime-safety paths rather than treating a successful compile as enough.
- **Compatibility records with context.** Results name the exact runtime,
  macOS host, graphics path, date, and known limitation. A result for one setup
  is not presented as universal support.

## MSIX and packaged desktop apps

Switchyard Wine includes `wineappx` and `appxsvc.dll` for signed, unencrypted,
full-trust desktop MSIX/AppX packages. The implemented lifecycle covers
inspection, verified extraction, per-prefix install and update, removal,
recovery, garbage collection, and launching declared Win32 or WinUI 3 apps
with package identity and static dependencies.

That scope is intentionally narrower than Windows. This is not a Microsoft
Store client, UWP or AppContainer support, an unsigned-package bypass, or a
promise that every Windows App SDK API works. The exact package requirements,
commands, durability model, and current limits are in the
[MSIX guide](docs/msix.md).

## See it running

<p align="center">
  <img src="docs/assets/switchyard-container-library.png" alt="Switchyard container library showing Windows games and launchers on macOS" width="100%">
</p>

<p align="center">
  <img src="docs/assets/switchyard-session-workspace.png" alt="Grand Theft Auto V Enhanced and Rockstar Games Launcher running in Switchyard" width="100%">
  <br>
  <sub>Grand Theft Auto V Enhanced and Rockstar Games Launcher in one managed Wine session.</sub>
</p>

## Get or build the runtime

Switchyard users normally let the app manage containers and runtime selection.
Signed and notarized Wine-only archives are available from
[GitHub Releases](https://github.com/jungwuk-ryu/switchyard-wine/releases).
Apple Game Porting Toolkit is user-provided software and is never included in
this repository or its releases.

To build from source on Apple Silicon macOS:

```shell
./switchyard/verify_source.sh
./switchyard/build_runtime.sh
```

Read the [build guide](docs/building.md) before publishing or replacing a
runtime. It covers the required toolchain, verified dependencies, staging,
signing, notarization, and available regression checks.

## Documentation

- [Documentation index](docs/README.md)
- [Architecture and repository boundaries](docs/architecture.md)
- [Building and releasing the runtime](docs/building.md)
- [MSIX and packaged desktop support](docs/msix.md)
- [Recorded application compatibility](docs/compatibility.md)
- [Source and dependency provenance](docs/provenance.md)
- [Unity game troubleshooting](docs/troubleshooting-unity-games.md)
- [Contribution workflow and compatibility policy](CONTRIBUTING.md)

For general Wine usage and development, use the
[WineHQ documentation](https://gitlab.winehq.org/wine/wine/-/wikis/home) and
[upstream source](https://gitlab.winehq.org/wine/wine). This README describes
Switchyard Wine and does not duplicate the upstream Wine manual.

## Community and license

Join the [Switchyard Discord](https://discord.gg/USNfzUza7B) for runtime
testing, compatibility reports, and development discussion.

Wine and Switchyard Wine changes are licensed under LGPL-2.1-or-later; see
`LICENSE` and `COPYING.LIB`. Switchyard Wine is independent and is not endorsed
by WineHQ, Apple, Microsoft, or the products shown above.
