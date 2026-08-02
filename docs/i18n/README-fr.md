<p align="center">
  <img src="../assets/switchyard-wine-logo.png" alt="Switchyard Wine logo" width="420">
</p>

# Switchyard Wine

[English](../../README.md) · [Autres langues](../README.md#translations)

Switchyard Wine est le runtime Wine derrière
[Switchyard](https://github.com/jungwuk-ryu/Switchyard), une application macOS pour
gérer les jeux et lanceurs Windows dans des conteneurs contrôlés. L’application
contrôle l’interface, l’état du conteneur et la sélection de la runtime ; ce dépôt
gère le code Wine, la chaîne de build et le travail de compatibilité sous-jacent.

Le projet fait un arbitrage délibéré : il reste sur une base Wine connue tant que les
charges de travail validées de Switchyard ne sont pas vérifiées, au lieu de se
mettre à jour simplement parce qu’une version Wine plus récente existe. Les
modifications en aval restent des commits Git révisables, appliqués sur une
révision WineHQ épinglée.

## Pourquoi cette branche existe

- **Travail macOS dans la runtime.** Cette branche contient des correctifs pour des
  chemins de panne réels observés dans Switchyard, notamment les callbacks et ponts de
  ressources D3DMetal, la synchronisation macOS MSync, le rendu Chromium/CEF, la
  sélection du fournisseur graphique, la lecture vidéo et le fallback multi-langue des
  polices.
- **Une runtime à identité vérifiable.** Les builds fixent et hachent les entrées
  externes, sont assemblées hors runtime active et ne sont publiées qu’après
  vérification. Chaque runtime enregistre la révision source, les digests des
  dépendances, l’architecture et les hachages des binaires principaux.
- **Tests de régression avec les correctifs.** Le dépôt couvre D3DMetal, callbacks
  natifs, MSync, hotpatching de l’overlay Steam, TLS, médias, OpenGL et chemins de
  sécurité runtime au lieu de se contenter d’une compilation réussie.
- **Données de compatibilité contextualisées.** Les résultats indiquent la runtime
  exacte, l’hôte macOS, le chemin graphique, la date et les limitations connues.
  Un résultat dans une configuration ne constitue pas une garantie de support universel.

## MSIX et applications de bureau packagées

Switchyard Wine inclut `wineappx` et `appxsvc.dll` pour les paquets MSIX/AppX
desktop full-trust signés et non chiffrés. Le cycle implémenté couvre l’inspection,
l’extraction vérifiée, l’installation et la mise à jour par préfixe, la suppression,
la récupération, le nettoyage et le lancement d’apps Win32 ou WinUI 3 déclarées avec
identité de package et dépendances statiques.

Cette portée est intentionnellement plus étroite que celle de Windows. Il ne s’agit
ni d’un client Microsoft Store, ni d’une prise en charge UWP/AppContainer, ni d’un
contournement de la vérification des paquets non signés. Le projet ne promet pas
non plus le fonctionnement de toutes les API du Windows App SDK. Les exigences des
paquets, les commandes, le modèle de durabilité et les limites actuelles figurent
dans le [guide MSIX](../msix.md).

## Voir en action

<p align="center">
  <img src="../assets/switchyard-container-library.png" alt="Bibliothèque de conteneurs Switchyard montrant des jeux et lanceurs Windows sur macOS" width="100%">
</p>

<p align="center">
  <img src="../assets/switchyard-session-workspace.png" alt="Grand Theft Auto V Enhanced et Rockstar Games Launcher en cours d’exécution dans Switchyard" width="100%">
  <br>
  <sub>Grand Theft Auto V Enhanced et Rockstar Games Launcher dans une session Wine gérée.</sub>
</p>

## Obtenir ou construire la runtime

Les utilisateurs de Switchyard laissent normalement l’application gérer les
conteneurs et la sélection runtime. Les archives Wine-only signées et notarisées sont
disponibles sur
[GitHub Releases](https://github.com/jungwuk-ryu/switchyard-wine/releases).
L’Apple Game Porting Toolkit est un logiciel fourni par l’utilisateur et n’est jamais
inclus dans ce dépôt ni dans ses releases.

Pour compiler depuis le code source sur Apple Silicon macOS :

```shell
./switchyard/verify_source.sh
./switchyard/build_runtime.sh
```

Lisez le [guide de compilation](../building.md) avant de publier ou remplacer une
runtime. Il couvre la chaîne d’outils requise, les dépendances vérifiées, la
préparation, la signature, la notarisation et les tests de régression disponibles.

## Documentation

- [Index de documentation](../README.md)
- [Architecture et frontières du dépôt](../architecture.md)
- [Construction et publication de la runtime](../building.md)
- [Support MSIX et applications de bureau packagées](../msix.md)
- [Compatibilité applicative enregistrée](../compatibility.md)
- [Provenance du code et des dépendances](../provenance.md)
- [Dépannage des jeux Unity](../troubleshooting-unity-games.md)

Pour l’usage général de Wine et le développement, consultez la
[documentation WineHQ](https://gitlab.winehq.org/wine/wine/-/wikis/home) et
la [source upstream](https://gitlab.winehq.org/wine/wine). Cette README décrit
Switchyard Wine et ne reproduit pas le manuel de Wine en amont.

## Communauté et licence

Rejoignez [Switchyard Discord](https://discord.gg/USNfzUza7B) pour les tests de
runtime, rapports de compatibilité et discussions de développement.

Les modifications de Wine et de Switchyard Wine sont sous LGPL-2.1-or-later; voir
`LICENSE` et `COPYING.LIB`. Switchyard Wine est indépendant et n’est pas
approuvé par WineHQ, Apple, Microsoft, ni par les produits présentés ci-dessus.
