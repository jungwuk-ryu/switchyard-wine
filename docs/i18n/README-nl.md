<p align="center">
  <img src="../assets/switchyard-wine-logo.png" alt="Switchyard Wine logo" width="420">
</p>

# Switchyard Wine

[English](../../README.md) · [Andere talen](../README.md#translations)

Switchyard Wine is de Wine-runtime achter [Switchyard](https://github.com/jungwuk-ryu/Switchyard),
een macOS-app voor het draaien van Windows-games en launchers in beheerde containers.
Switchyard beheert de interface, containerstatus en runtime-keuze; deze repository bevat
Wine-code, build-pijplijn en compatibiliteitswerk onder de motorkap.

Het project maakt een bewuste afweging: het blijft op een bekende Wine-basis staan
tot de vastgestelde workloads van Switchyard zijn gevalideerd, in plaats van te updaten
omdat er een nieuwere Wine-release bestaat. Wijzigingen stroomafwaarts blijven gewone,
controleerbare Git-commits bovenop een vastgepinde WineHQ-revisie.

## Waarom deze branch bestaat

- **macOS-werk in de runtime.** De branch bevat fixes voor echte foutenpaden die in
  Switchyard zijn gezien, inclusief D3DMetal callback en resource bridging, macOS MSync
  synchronisatie, Chromium/CEF-rendering, grafische providerkeuze, mediaspelerwerking en
  meertalige font fallback.
- **Een runtime met verifieerbare identiteit.** Builds pinnen en hashen externe inputs,
  worden buiten de live runtime opgebouwd en alleen gepubliceerd na verificatie. Elke
  runtime registreert bronrevisie, dependency-digests, architectuur en kernbinary hashes.
- **Regressietests naast de fixes.** De repository test D3DMetal, native callbacks, MSync,
  Steam-overlay hotpatching, TLS, media, OpenGL en runtime-safety paden, niet alleen
  een succesvolle build.
- **Compatibiliteitsresultaten met context.** Resultaten noemen exacte runtime, macOS host,
  graphics pad, datum en bekende beperkingen; één succesvolle setup is geen claim van
  universele ondersteuning.

## MSIX en verpakte desktop-apps

Switchyard Wine bevat `wineappx` en `appxsvc.dll` voor ondertekende, niet-versleutelde,
full-trust desktop MSIX/AppX-pakketten. De geïmplementeerde levenscyclus bevat inspectie,
geverifieerde extractie, installatie en updates per prefix, verwijdering, herstel,
garbage collection en het starten van gedeclareerde Win32- of WinUI 3-apps met
package identity en statische dependencies.

Deze scope is bewust smaller dan Windows. Dit is geen Microsoft Store-client, geen
UWP/AppContainer-ondersteuning, geen omzeiling van niet-ondertekende pakketten, en geen
garantie dat elke Windows App SDK API werkt. De exacte pakketvereisten, commando's,
duurzaamheidswaarborgen en huidige limieten staan in de
[MSIX-handleiding](../msix.md).

## Zie het in actie

<p align="center">
  <img src="../assets/switchyard-container-library.png" alt="Switchyard container library met Windows games en launchers op macOS" width="100%">
</p>

<p align="center">
  <img src="../assets/switchyard-session-workspace.png" alt="Grand Theft Auto V Enhanced en Rockstar Games Launcher draaiend in Switchyard" width="100%">
  <br>
  <sub>Grand Theft Auto V Enhanced en Rockstar Games Launcher in een beheerde Wine-sessie.</sub>
</p>

## Runtime verkrijgen of bouwen

Switchyard-gebruikers laten doorgaans de app containers en runtime-keuze beheren.
Getekende en genotariseerde Wine-only archieven zijn beschikbaar op
[GitHub Releases](https://github.com/jungwuk-ryu/switchyard-wine/releases).
Apple Game Porting Toolkit is gebruikersoftware en zit niet in dit repository of releases.

Builden vanaf bron op Apple Silicon macOS:

```shell
./switchyard/verify_source.sh
./switchyard/build_runtime.sh
```

Lees de [buildgids](../building.md) voordat je een runtime uitbrengt of vervangt.
Deze beschrijft vereiste toolchain, geverifieerde dependencies, staging, ondertekening,
notarisatie en beschikbare regressietoetsen.

## Documentatie

- [Documentatie-index](../README.md)
- [Architectuur en repo-grenzen](../architecture.md)
- [Runtime builden en releasen](../building.md)
- [MSIX en verpakte desktop-ondersteuning](../msix.md)
- [Geregistreerde applicatie-compatibiliteit](../compatibility.md)
- [Bron- en dependency provenance](../provenance.md)
- [Unity foutoplossing](../troubleshooting-unity-games.md)

Voor algemeen Wine-gebruik en ontwikkeling gebruik de
[WineHQ-documentatie](https://gitlab.winehq.org/wine/wine/-/wikis/home) en
[upstream broncode](https://gitlab.winehq.org/wine/wine). Deze README beschrijft
Switchyard Wine en vervangt niet de upstream Wine-handleiding.

## Community en licentie

Word lid van de [Switchyard Discord](https://discord.gg/USNfzUza7B) voor runtime-tests,
compatibiliteitsrapporten en ontwikkelgesprekken.

Wijzigingen in Wine en Switchyard Wine vallen onder LGPL-2.1-or-later; zie `LICENSE`
en `COPYING.LIB`. Switchyard Wine is onafhankelijk en niet goedgekeurd door WineHQ,
Apple, Microsoft, of de hierboven getoonde producten.
