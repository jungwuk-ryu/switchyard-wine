<p align="center">
  <img src="../assets/switchyard-wine-logo.png" alt="Switchyard Wine logo" width="420">
</p>

# Switchyard Wine

[English](../../README.md) · [Andra språk](../README.md#translations)

Switchyard Wine är Wine-runtime bakom [Switchyard](https://github.com/jungwuk-ryu/Switchyard),
en macOS-app som håller Windows-spel och launchers i hanterade containrar. Appen äger
gränssnittet, containerstatus och runtime-val; detta repo äger Wine-koden, bygg-pipelinen
och kompatibilitetsarbetet under den.

Projektet gör ett medvetet val: det hålls kvar på en känd Wine-bas tills Switchyards
etablerade arbetsflöden har validerats, istället för att uppdatera bara för att en ny
Wine-version finns. Nedströmsändringar är vanliga, granskningsbara Git-commits på en låst
WineHQ-revision.

## Varför denna branch finns

- **macOS-arbete i runtime.** Grenen innehåller fixar för verkliga felvägar som ses i
  Switchyard, inklusive D3DMetal callback/resource bridging, macOS MSync-synkronisering,
  Chromium/CEF-rendering, val av grafikleverantör, mediespelning och flerspråkig fontfallback.
- **En runtime med verifierbar identitet.** Byggen låser och hashar externa indata,
  byggs utanför aktiv runtime och publiceras först efter verifiering. Varje runtime registrerar
  källrevision, dependency-digests, arkitektur och kärn-binary-hashar.
- **Regressionstester tillsammans med fixar.** Repositoriet testar D3DMetal, native callbacks,
  MSync, Steam overlay hotpatch, TLS, media, OpenGL och runtime-säkerhetsvägar i stället för att
  nöja sig med en lyckad kompilering.
- **Kompatibilitetsresultat med kontext.** Resultaten anger exakt runtime, macOS-host, grafikväg,
  datum och kända begränsningar. Ett lyckat resultat i en setup betyder inte universellt stöd.

## MSIX och paketerade desktop-appar

Switchyard Wine innehåller `wineappx` och `appxsvc.dll` för signerade, okrypterade
full-trust desktop MSIX/AppX-paket. Den implementerade livscykeln täcker inspektion,
verifierad extrahering, per-prefix-installation och uppdatering, borttagning, återställning,
skräprensning och start av deklarerade Win32- eller WinUI 3-appar med paketidentitet och
statiska beroenden.

Omfånget är avsiktligt snävare än Windows. Detta är inte en Microsoft Store-klient, ingen
UWP/AppContainer-stöd, ingen omväg för osignerade paket och ingen garanti att alla Windows
App SDK API:er fungerar. Exakta paketkrav, kommandon, hållbarhetsmodell och
aktuella begränsningar finns i [MSIX-guiden](../msix.md).

## Se det i drift

<p align="center">
  <img src="../assets/switchyard-container-library.png" alt="Switchyard container library med Windows-spel och launchers på macOS" width="100%">
</p>

<p align="center">
  <img src="../assets/switchyard-session-workspace.png" alt="Grand Theft Auto V Enhanced och Rockstar Games Launcher som körs i Switchyard" width="100%">
  <br>
  <sub>Grand Theft Auto V Enhanced och Rockstar Games Launcher i en kontrollerad Wine-session.</sub>
</p>

## Hämta eller bygga runtime

Vanliga Switchyard-användare låter appen hantera containrar och runtime-val.
Signerade och notariserade Wine-only-arkiv finns på
[GitHub Releases](https://github.com/jungwuk-ryu/switchyard-wine/releases).
Apple Game Porting Toolkit är användarens egna programvara och ingår inte i detta repo eller
dessa releaser.

Bygg från källa på Apple Silicon macOS:

```shell
./switchyard/verify_source.sh
./switchyard/build_runtime.sh
```

Läs [byggguiden](../building.md) innan du publicerar eller byter en runtime.
Guiden täcker nödvändigt verktygsbete, verifierade beroenden, staging, signering,
notarisation och tillgängliga regression checks.

## Dokumentation

- [Dokumentationsöversikt](../README.md)
- [Arkitektur och repositories gränser](../architecture.md)
- [Bygga och släppa runtime](../building.md)
- [MSIX och paketerad desktop-support](../msix.md)
- [Registrerad applikationskompatibilitet](../compatibility.md)
- [Käll- och dependency-proveniens](../provenance.md)
- [Felsökning av Unity-spel](../troubleshooting-unity-games.md)

För allmän Wine-användning och utveckling, använd
[WineHQ-dokumentationen](https://gitlab.winehq.org/wine/wine/-/wikis/home) och
[upstream-källan](https://gitlab.winehq.org/wine/wine). Denna README beskriver
Switchyard Wine och ersätter inte upstreams Wine-manual.

## Gemenskap och licens

Gå med i [Switchyard Discord](https://discord.gg/USNfzUza7B) för runtime-tester,
kompatibilitetsrapporter och utvecklingsdiskussion.

Wine och Switchyard Wine-ändringar är licensierade under LGPL-2.1-or-later; se
`LICENSE` och `COPYING.LIB`. Switchyard Wine är oberoende och har inte stöd av WineHQ,
Apple, Microsoft eller produkterna ovan.
