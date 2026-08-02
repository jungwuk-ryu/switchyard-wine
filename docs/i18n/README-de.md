<p align="center">
  <img src="../assets/switchyard-wine-logo.png" alt="Switchyard Wine Logo" width="420">
</p>

# Switchyard Wine

[English](../../README.md) · [Andere Sprachen](../README.md#translations)

Switchyard Wine ist die Wine-Runtime hinter
[Switchyard](https://github.com/jungwuk-ryu/Switchyard), einer macOS-App zum
Verwalten von Windows-Spielen und Launchern in verwalteten Containern. Die App
verwaltet die Oberfläche, den Containerstatus und die Runtime-Auswahl; dieses
Repository enthält den Wine-Code, die Build-Pipeline und die Kompatibilitätsarbeit
darunter.

Das Projekt trifft eine bewusste Abwägung: Es bleibt auf einer bekannten Wine-Basis,
bis die etablierten Workloads von Switchyard geprüft wurden, statt nur wegen einer
neueren Wine-Version zu aktualisieren. Änderungen nachgelagerter Komponenten sind
als normale, nachvollziehbare Git-Commits auf Basis einer festen WineHQ-Revision
implementiert.

## Warum dieser Branch existiert

- **macOS-Arbeit in der Runtime.** Der Branch enthält Korrekturen für reale
  Fehlerpfade in Switchyard, inklusive D3DMetal-Callbacks und Ressourcen-Bridging,
  macOS-MSync-Synchronisation, Chromium/CEF-Rendering, Grafikprovider-Auswahl,
  Medienwiedergabe und mehrsprachigem Font-Fallback.
- **Runtime mit verifizierbarer Identität.** Builds verifizieren externe Eingaben
  per Hash, werden außerhalb der Live-Runtime erstellt und erst nach Verifikation
  veröffentlicht. Jede Runtime dokumentiert Quellrevision, Abhängigkeits-Digests,
  Architektur und Kern-Binärdatei-Hashes.
- **Regressionstests neben den Fixes.** Das Repository testet D3DMetal,
  Native Callbacks, MSync, Steam-Overlay-Hotpatching, TLS, Medien, OpenGL und
  Sicherheits-Pfade der Runtime statt nur auf einen erfolgreichen Build zu setzen.
- **Kompatibilitätsdaten mit Kontext.** Ergebnisse nennen die exakte Runtime,
  macOS-Host, Grafikpfad, Datum und bekannte Einschränkung. Ein Resultat in
  einer Umgebung wird nicht als universelle Unterstützung dargestellt.

## MSIX und paketierte Desktop-Apps

Switchyard Wine enthält `wineappx` und `appxsvc.dll` für signierte, unverschlüsselte
full-trust Desktop-MSIX/AppX-Pakete. Der implementierte Lebenszyklus umfasst
Prüfung, verifizierte Extraktion, Installation und Update pro Prefix, Entfernung,
Wiederherstellung, Garbage Collection und Start deklarierter Win32- oder WinUI 3-Apps
mit Package-Identität und statischen Abhängigkeiten.

Dieser Umfang ist bewusst enger als bei Windows. Dies ist kein Microsoft Store-Client,
keine UWP/AppContainer-Unterstützung, kein Umgehen der Signaturprüfung und keine
Umgehung für nicht signierte Pakete. Eine Garantie, dass jede Windows App SDK-API
funktioniert, wird ebenfalls nicht gegeben. Exakte Paketanforderungen,
Befehle, Haltbarkeitsmodell und aktuelle Grenzen stehen im
[MSIX-Leitfaden](../msix.md).

## Live-Demo

<p align="center">
  <img src="../assets/switchyard-container-library.png" alt="Switchyard-Container-Bibliothek mit Windows-Spielen und Launchern auf macOS" width="100%">
</p>

<p align="center">
  <img src="../assets/switchyard-session-workspace.png" alt="Grand Theft Auto V Enhanced und Rockstar Games Launcher in einer verwalteten Switchyard Wine-Sitzung" width="100%">
  <br>
  <sub>Grand Theft Auto V Enhanced und Rockstar Games Launcher in einer verwalteten Wine-Sitzung.</sub>
</p>

## Runtime beziehen oder selbst bauen

Switchyard-Nutzer lassen in der Regel App Container- und Runtime-Auswahl vom
Programm verwalten. Signierte und notarierte Wine-only Archive sind in den
[GitHub Releases](https://github.com/jungwuk-ryu/switchyard-wine/releases)
verfügbar. Das Apple Game Porting Toolkit ist von Nutzern bereitzustellende
Software und ist nicht Bestandteil dieses Repositories oder der Releases.

So bauen Sie auf Apple Silicon macOS aus dem Quellcode:

```shell
./switchyard/verify_source.sh
./switchyard/build_runtime.sh
```

Lesen Sie vor der Veröffentlichung oder Ersetzung einer Runtime die
[Build-Anleitung](../building.md). Sie deckt benötigtes Tooling, verifizierte
Abhängigkeiten, Staging, Signierung, Notarisierung und verfügbare Regressionstests ab.

## Dokumentation

- [Dokumentationsindex](../README.md)
- [Architektur und Repository-Grenzen](../architecture.md)
- [Runtime bauen und veröffentlichen](../building.md)
- [MSIX und paketierte Desktop-Unterstützung](../msix.md)
- [Aufgezeichnete Anwendungs-Kompatibilität](../compatibility.md)
- [Quellen und Abhängigkeitsherkunft](../provenance.md)
- [Fehlerbehebung für Unity-Games](../troubleshooting-unity-games.md)

Für allgemeine Wine-Nutzung und Entwicklung nutzen Sie die
[WineHQ-Dokumentation](https://gitlab.winehq.org/wine/wine/-/wikis/home) und
[die Upstream-Quelle](https://gitlab.winehq.org/wine/wine). Diese README beschreibt
Switchyard Wine und wiederholt nicht das upstream Wine-Handbuch.

## Community und Lizenz

Treffen Sie sich in der [Switchyard Discord](https://discord.gg/USNfzUza7B) für
Runtime-Tests, Kompatibilitätsberichte und Entwicklungsdiskussionen.

Änderungen an Wine und Switchyard Wine sind unter LGPL-2.1-or-later lizenziert;
siehe `LICENSE` und `COPYING.LIB`. Switchyard Wine ist unabhängig und wird nicht
von WineHQ, Apple, Microsoft oder den oben gezeigten Produkten unterstützt.
