<p align="center">
  <img src="../assets/switchyard-wine-logo.png" alt="Switchyard Wine logo" width="420">
</p>

# Switchyard Wine

[English](../../README.md) · [Más nyelv](../README.md#translations)

A Switchyard Wine a
[Switchyard](https://github.com/jungwuk-ryu/Switchyard)-hez kapcsolódó Wine-runtime egy
macOS-alkalmazáshoz, amely Windows-játékok és -indítók futtatását végzi konténerben.
A Switchyard kezeli a felhasználói felületet, a konténerállapotot és a runtime-választást;
ez a repó pedig a Wine-kódot, a build folyamatot és a Switchyard-specifikus
kompatibilitási munkát tartalmazza.

A projekt tudatos döntést hoz: nem ugorjuk át rögtön az új Wine-verziókra; csak akkor
frissítünk, amikor a Switchyard kipróbált munkafolyamatait hitelesítettük. A downstream
változások sima, visszakövethető Git commitok a rögzített WineHQ-alaprevízió felett.

## Miért létezik ez az ág

- **macOS-fókuszú runtime-javítások.** Az ág valós hibafolyamatokhoz adott javításokat
  tartalmaz, többek között D3DMetal callback/resource-bridging, macOS MSync
  szinkronizáció, Chromium/CEF renderelés, grafikus szolgáltató-választás, médiakezelés
  és többnyelvű betűkészlet-váltás.
- **Ellenőrizhető runtime-identitás.** A build bemenetei pineltek és hash-elvek; az
  építés az élő runtime-on kívül történik, és csak ellenőrzés után kerül kiadásra.
  Minden runtime tárolja a forrásrevíziót, függőségi digesteket, architektúrát és a
  kulcsbinárisok hash-értékeit.
- **Regressziós tesztek a javításokkal együtt.** A repó D3DMetal, natív callbackek,
  MSync, Steam overlay hotpatch, TLS, média, OpenGL és runtime biztonsági útvonalak
  tesztelését végzi, nem csak a sikeres buildelést.
- **Kontekstussal ellátott kompatibilitási rekordok.** Az eredmények pontos runtime-t,
  macOS hostot, grafikus utat, dátumot és ismert korlátozásokat tartalmaznak; egy
  konfigurációban mért eredmény nem jelenti az általános támogatást.

## MSIX és csomagolt desktop alkalmazások

A Switchyard Wine a `wineappx` és `appxsvc.dll` segítségével kezeli az aláírt,
titkosítatlan full-trust desktop MSIX/AppX csomagokat. A megvalósított életciklus
fedezi a vizsgálatot, a hitelesített kibontást, a prefixenkénti telepítést és
frissítést, a törlést, a helyreállítást, a szemétszedést, valamint a package identity
és statikus függőségek használatával deklarált Win32 vagy WinUI 3 alkalmazások indítását.

Ez a hatókör szándékosan szűkebb, mint a Windowsé. Nem Microsoft Store kliens, nem
UWP/AppContainer-támogatás, nem aláíratlan csomagok megkerülése, és nem jelenti, hogy minden
Windows App SDK API működik. A pontos
csomagkövetelmények, parancsok, tartóssági modell és jelenlegi korlátok a
[MSIX útmutatóban](../msix.md) szerepelnek.

## Működés közben

<p align="center">
  <img src="../assets/switchyard-container-library.png" alt="Switchyard konténerkönyvtár Windows játékokkal és launcherekkel macOS-en" width="100%">
</p>

<p align="center">
  <img src="../assets/switchyard-session-workspace.png" alt="Grand Theft Auto V Enhanced és Rockstar Games Launcher futtatása Switchyardben" width="100%">
  <br>
  <sub>Grand Theft Auto V Enhanced és Rockstar Games Launcher egy közös, kezelt Wine-munkamenetben.</sub>
</p>

## Runtime beszerzése vagy építése

A Switchyard felhasználók általában a Switchyard appra bízzák a konténer- és runtime-kiválasztást.
A [GitHub Releases](https://github.com/jungwuk-ryu/switchyard-wine/releases)-en elérhetők az
aláírt és notarisált Wine-only archívumok. Az Apple Game Porting Toolkit felhasználói
oldali szoftver, és nem része a repónak vagy a release csomagoknak.

Apple Silicon macOS-on a forrásból való építés:

```shell
./switchyard/verify_source.sh
./switchyard/build_runtime.sh
```

Az [építési útmutató](../building.md) elolvasása kötelező a runtime publikálása vagy
cseréje előtt; tartalmazza a szükséges toolchain-t, a hitelesített függőségeket, a staginget,
az aláírást, a notarisálást és a rendelkezésre álló regressziós ellenőrzéseket.

## Dokumentáció

- [Dokumentációs index](../README.md)
- [Architektúra és repozitoriumi határok](../architecture.md)
- [Runtime építés és kiadás](../building.md)
- [MSIX és csomagolt desktop támogatás](../msix.md)
- [Rögzített alkalmazás-kompatibilitás](../compatibility.md)
- [Forrás- és függőség provenance](../provenance.md)
- [Unity játékok hibakeresése](../troubleshooting-unity-games.md)

Általános Wine használathoz és fejlesztéshez használd a
[WineHQ dokumentációt](https://gitlab.winehq.org/wine/wine/-/wikis/home) és az
[upstream forrást](https://gitlab.winehq.org/wine/wine). Ez a README a Switchyard Winet
ismerteti, és nem másolja az upstream Wine kézikönyvet.

## Közösség és licenc

Csatlakozz a [Switchyard Discord](https://discord.gg/USNfzUza7B) csatornához runtime
tesztekért, kompatibilitási jelentésekért és fejlesztői beszélgetésért.

A Wine és a Switchyard Wine módosításai LGPL-2.1-or-later licenc alatt állnak; lásd a
`LICENSE` és a `COPYING.LIB` fájlokat. A Switchyard Wine független, és nem támogatott
a WineHQ, az Apple vagy a Microsoft (se a fent látható termékek) által.
