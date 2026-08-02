<p align="center">
  <img src="../assets/switchyard-wine-logo.png" alt="Switchyard Wine logo" width="420">
</p>

# Switchyard Wine

[English](../../README.md) · [Altre lingue](../README.md#translations)

Switchyard Wine è la runtime Wine dietro a
[Switchyard](https://github.com/jungwuk-ryu/Switchyard), un’app macOS per gestire
giochi e launcher Windows in contenitori gestiti. L’app controlla l’interfaccia,
l’area del contenitore e la selezione della runtime; questo repository gestisce il
codice Wine, la pipeline di build e il lavoro di compatibilità sottostante.

Il progetto fa una scelta deliberata: rimane su una base Wine nota finché i
workload verificati di Switchyard non sono stati confermati, invece di aggiornare
solo perché esiste una versione Wine più recente. Le modifiche a valle restano
commit Git revisabili e ordinari, applicati sopra una revisione WineHQ fissata.

## Perché esiste questo branch

- **Lavoro macOS nella runtime.** Il branch include correzioni per percorsi di errore
  reali osservati in Switchyard, tra cui callbacks e bridge delle risorse D3DMetal,
  sincronizzazione macOS MSync, rendering Chromium/CEF, selezione del provider
  grafico, riproduzione multimediale e fallback tipografico multilingue.
- **Runtime con identità verificabile.** Le build fissano e calcolano hash degli input
  esterni, vengono assemblate fuori dalla runtime attiva e pubblicate solo dopo
  verifica. Ogni runtime registra la revisione sorgente, i digest delle dipendenze,
  l’architettura e gli hash dei binari core.
- **Test di regressione insieme ai fix.** Il repository testa D3DMetal, callback
  nativi, MSync, hotpatching dell’overlay Steam, TLS, media, OpenGL e percorsi di
  sicurezza runtime, invece di fermarsi a una compilazione riuscita.
- **Compatibilità con contesto.** I risultati indicano runtime esatta, host macOS,
  percorso grafico, data e limiti noti. Un risultato in una configurazione non deve
  essere letto come supporto universale.

## MSIX e app desktop pacchettizzate

Switchyard Wine include `wineappx` e `appxsvc.dll` per pacchetti MSIX/AppX full-trust
firmati e non crittografati. Il ciclo implementato copre ispezione, estrazione
verificata, installazione e aggiornamento per prefisso, rimozione, recupero,
garbage collection e avvio di app Win32 o WinUI 3 dichiarate con identità di
pacchetto e dipendenze statiche.

L’ambito è intenzionalmente più ristretto di Windows. Non è un client Microsoft
Store, non è compatibilità UWP/AppContainer, non consente di evitare la verifica di
pacchetti non firmati, né una promessa che tutte le API di Windows App SDK
funzionino. I requisiti esatti del pacchetto, i comandi, il modello di durata e i
limiti attuali sono nella
[guida MSIX](../msix.md).

## Vederlo in esecuzione

<p align="center">
  <img src="../assets/switchyard-container-library.png" alt="Libreria contenitori Switchyard con giochi e launcher Windows su macOS" width="100%">
</p>

<p align="center">
  <img src="../assets/switchyard-session-workspace.png" alt="Grand Theft Auto V Enhanced e Rockstar Games Launcher in esecuzione in Switchyard" width="100%">
  <br>
  <sub>Grand Theft Auto V Enhanced e Rockstar Games Launcher in una sessione Wine gestita.</sub>
</p>

## Ottenere o compilare la runtime

Gli utenti di Switchyard in genere lasciano che l’app gestisca contenitori e
selezione runtime. Gli archivi Wine-only firmati e notarizzati sono disponibili su
[GitHub Releases](https://github.com/jungwuk-ryu/switchyard-wine/releases).
L’Apple Game Porting Toolkit è software fornito dall’utente e non è incluso né nel
repository né nelle release.

Per compilare da sorgente su Apple Silicon macOS:

```shell
./switchyard/verify_source.sh
./switchyard/build_runtime.sh
```

Leggi la [guida alla build](../building.md) prima di pubblicare o sostituire una
runtime. Copre toolchain richiesta, dipendenze verificate, staging, firma,
notarizzazione e controlli di regressione disponibili.

## Documentazione

- [Indice documentazione](../README.md)
- [Architettura e confini del repository](../architecture.md)
- [Build e rilascio della runtime](../building.md)
- [Supporto MSIX e desktop pacchettizzato](../msix.md)
- [Compatibilità applicazioni registrata](../compatibility.md)
- [Provenienza codice e dipendenze](../provenance.md)
- [Troubleshooting giochi Unity](../troubleshooting-unity-games.md)

Per l’uso generale di Wine e sviluppo, utilizzare la
[documentazione WineHQ](https://gitlab.winehq.org/wine/wine/-/wikis/home) e
[il sorgente upstream](https://gitlab.winehq.org/wine/wine). Questa README descrive
Switchyard Wine e non duplica il manuale Wine upstream.

## Community e licenza

Partecipa a
[Switchyard Discord](https://discord.gg/USNfzUza7B) per test runtime, report di
compatibilità e discussioni di sviluppo.

Wine e Switchyard Wine sono concessi sotto LGPL-2.1-or-later; vedi `LICENSE`
e `COPYING.LIB`. Switchyard Wine è indipendente e non è avallato da WineHQ,
Apple, Microsoft o dai prodotti mostrati qui sopra.
