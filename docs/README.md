# Documentation

This directory is the documentation home for Switchyard Wine. The technical
guides are maintained in English so that runtime behavior, commands, and safety
limits have one source of truth. Translated project overviews are available
below.

## Start here

- [`architecture.md`](architecture.md) explains what this repository owns, how
  it stays separate from the Switchyard app and user-provided Apple software,
  and how a generated runtime identifies itself.
- [`building.md`](building.md) covers the Apple Silicon build, verified external
  inputs, runtime promotion, signing, notarization, and regression checks.
- [`hdr-edr.md`](hdr-edr.md) defines display-colour semantics, HDR/EDR provider
  capability policy, lifecycle rules, and the graphics validation procedure.
- [`msix.md`](msix.md) documents signed full-trust desktop MSIX/AppX deployment,
  `wineappx`, packaged process identity, recovery, and the unsupported UWP and
  Microsoft Store surfaces.
- [`compatibility.md`](compatibility.md) records application results against an
  exact runtime revision, host, and launch or graphics path.
- [`provenance.md`](provenance.md) records the upstream Wine base, downstream
  history policy, licenses, and external build inputs.
- [`shader-cache.md`](shader-cache.md) documents D3D12 application shader-cache
  persistence, its security and recovery invariants, and the provider-cache
  capability boundary.
- [`troubleshooting-unity-games.md`](troubleshooting-unity-games.md) covers the
  Unity startup failure modes currently documented for Switchyard.
- [`CONTRIBUTING.md`](../CONTRIBUTING.md) defines the downstream change policy,
  expected evidence, and contribution workflow.

## Translations

These pages translate the Switchyard project overview in the root
[`README.md`](../README.md). The technical guides linked from them remain the
canonical reference.

- [Deutsch](i18n/README-de.md)
- [Español](i18n/README-es.md)
- [Suomi](i18n/README-fi.md)
- [Français](i18n/README-fr.md)
- [Magyar](i18n/README-hu.md)
- [Italiano](i18n/README-it.md)
- [日本語](i18n/README-ja.md)
- [한국어](i18n/README-ko.md)
- [Nederlands](i18n/README-nl.md)
- [Norsk](i18n/README-no.md)
- [Português](i18n/README-pt.md)
- [Português do Brasil](i18n/README-pt_br.md)
- [Русский](i18n/README-ru.md)
- [Svenska](i18n/README-sv.md)
- [Türkçe](i18n/README-tr.md)
- [Українська](i18n/README-uk.md)
- [简体中文](i18n/README-zh_cn.md)

General Wine documentation belongs upstream. See the
[WineHQ wiki](https://gitlab.winehq.org/wine/wine/-/wikis/home) for Wine usage,
building, debugging, and contribution guidance that is not specific to
Switchyard.
