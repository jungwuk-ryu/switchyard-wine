<p align="center">
  <img src="../assets/switchyard-wine-logo.png" alt="Switchyard Wine logo" width="420">
</p>

# Switchyard Wine

[English](../../README.md) · [Andre språk](../README.md#translations)

Switchyard Wine er Wine-runtime bak [Switchyard](https://github.com/jungwuk-ryu/Switchyard), en macOS-app for å kjøre Windows-spill og launchere i styrte containere. Switchyard-appen styrer grensesnittet, containerstatus og valg av runtime; dette repositoriet håndterer Wine-koden, byggeflyten og kompatibilitetsarbeidet under.

Prosjektet velger bevisst en fast Wine-base: det forblir på en kjent Wine-revisjon til
Switchyards etablerte arbeidsmengder er validert, i stedet for å oppdatere bare fordi en ny
Wine-versjon finnes. Endringer nedstrøms er vanlige, gjennomgåelige Git-commits på toppen av
en fastpinnen WineHQ-revisjon.

## Hvorfor denne grenen eksisterer

- **macOS-arbeid i runtime.** Grenen inneholder fikser for faktiske feilstrømmer observert i
  Switchyard, inkludert D3DMetal-callback/resource bridging, macOS MSync-synkronisering,
  Chromium/CEF-rendering, valg av grafikk-provider, medieavspilling og flerspråklig
  fontfallback.
- **En runtime med verifiserbar identitet.** Builds pinner og hasher eksterne inputs, blir
  bygget utenfor live runtime og publisert først etter verifisering. Hver runtime registrerer
  kilde-revisjon, dependency-digester, arkitektur og hashes for kjernebinærer.
- **Regresjonstester ved siden av endringer.** Repositoriet tester D3DMetal, native callbacks,
  MSync, Steam overlay hotpatch, TLS, media, OpenGL og runtime-sikkerhetsveier i stedet for å
  stole på en vellykket kompilering alene.
- **Kompatibilitetsresultater med kontekst.** Resultatene viser eksakt runtime, macOS host,
  grafikkvei, dato og kjente begrensninger. Ett miljø som fungerer betyr ikke universell støtte.

## MSIX og pakket desktop-app

Switchyard Wine inkluderer `wineappx` og `appxsvc.dll` for signerte, ukrypterte
desktop full-trust MSIX/AppX-pakker. Den implementerte livssyklusen dekker inspeksjon,
verifisert utpakking, installasjon og oppdatering per prefiks, fjerning, gjenoppretting,
opprydding og oppstart av deklarerte Win32- eller WinUI 3-apper med pakkeidentitet
og statiske avhengigheter.

Omfanget er bevisst smalere enn Windows. Dette er ikke en Microsoft Store-klient, ingen
UWP/AppContainer-støtte, ingen omgåelse av usignerte pakker, og ingen garanti for at alle
Windows App SDK-API-er fungerer. Eksakte pakkekrav, kommandoer, holdbarhetsmodell og
gjeldende begrensninger står i [MSIX-guiden](../msix.md).

## Se det i bruk

<p align="center">
  <img src="../assets/switchyard-container-library.png" alt="Switchyard container library med Windows spill og launchere på macOS" width="100%">
</p>

<p align="center">
  <img src="../assets/switchyard-session-workspace.png" alt="Grand Theft Auto V Enhanced og Rockstar Games Launcher kjører i Switchyard" width="100%">
  <br>
  <sub>Grand Theft Auto V Enhanced og Rockstar Games Launcher i en styrt Wine-økt.</sub>
</p>

## Hent eller bygg runtime

Vanlige Switchyard-brukere lar appen håndtere containere og runtime-valget.
Signerte og notariserte Wine-only-arkiver finnes på
[GitHub Releases](https://github.com/jungwuk-ryu/switchyard-wine/releases).
Apple Game Porting Toolkit er brukerleverte filer og er ikke inkludert i dette repositoriet
eller utgivelsene.

Bygg fra kilde på Apple Silicon macOS:

```shell
./switchyard/verify_source.sh
./switchyard/build_runtime.sh
```

Les [byggguide](../building.md) før du publiserer eller bytter runtime. Den dekker nødvendig
toolchain, verifiserte avhengigheter, staging, signering, notarisering og tilgjengelige
regresjonskontroller.

## Dokumentasjon

- [Dokumentasjonsindeks](../README.md)
- [Arkitektur og repo-grenser](../architecture.md)
- [Bygge og gi ut runtime](../building.md)
- [MSIX og pakket desktop-støtte](../msix.md)
- [Registrert applikasjonskompatibilitet](../compatibility.md)
- [Kilde- og avhengighetsprovenans](../provenance.md)
- [Unity-feilsøking](../troubleshooting-unity-games.md)

For generell Wine-bruk og utvikling, bruk
[WineHQ-dokumentasjonen](https://gitlab.winehq.org/wine/wine/-/wikis/home) og
[upstream-kilde](https://gitlab.winehq.org/wine/wine). Denne README-en beskriver
Switchyard Wine og erstatter ikke upstream Wine-håndboken.

## Fellesskap og lisens

Bli med i [Switchyard Discord](https://discord.gg/USNfzUza7B) for runtime-tester,
kompatibilitetsrapporter og utviklingsdiskusjoner.

Wine- og Switchyard Wine-endringer er lisensiert under LGPL-2.1-or-later; se `LICENSE`
og `COPYING.LIB`. Switchyard Wine er uavhengig og er ikke offisielt godkjent av WineHQ,
Apple, Microsoft eller produktene nevnt ovenfor.
