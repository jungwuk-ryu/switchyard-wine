<p align="center">
  <img src="../assets/switchyard-wine-logo.png" alt="Switchyard Wine logo" width="420">
</p>

# Switchyard Wine

[English](../../README.md) · [その他の言語](../README.md#translations)

## Switchyard Wine とは

Switchyard Wine は、Switchyard の基盤となる Wine ランタイムです。Switchyard は
Windows ゲームやランチャーを管理されたコンテナで実行する macOS アプリで、
インターフェース、コンテナ状態、ランタイムの選択を管理します。
このリポジトリは Wine 本体、ビルドパイプライン、互換性検証を管理します。

このブランチは、Windows 実行可能ファイル全般の理想的なサポートではなく、
Switchyard の既知ワークロードを壊さないことを優先して、固定された Wine ベース上で
段階的に更新します。主要変更は、固定した WineHQ リビジョン上の、通常のレビュー可能な
Git コミットとして残ります。

## なぜこのブランチか

- **macOS 向けランタイム修正**: D3DMetal コールバック、macOS MSync、Chromium/CEF
  レンダリング、グラフィックス選択、メディア再生、マルチ言語フォントなどの
  実障パス修正を担います。
- **検証可能なランタイム ID**: 外部入力はハッシュ化され、実行中のパスではなく
  検証済みステージで組み立てられ、ランタイム起動前にチェックされます。
  各ランタイムはソースリビジョン、依存関係ハッシュ、アーキテクチャ、主要バイナリ
  ハッシュを記録します。
- **修正と同時に回帰テスト**: D3DMetal、ネイティブコールバック、MSync、Steam
  オーバーレイ hotpatch、TLS、メディア、OpenGL、ランタイム安全性テストを実施します。
- **根拠付き互換性記録**: 結果には runtime、macOS ホスト、グラフィックス経路、日付、
  制限を明記し、別構成への汎用保証と見なさないようにします。

## MSIX とパッケージアプリ

Switchyard Wine は、署名済みの暗号化されていない full-trust デスクトップ
MSIX/AppX パッケージ向けに `wineappx` と `appxsvc.dll` を提供します。実装済みライフサイクルは、
検査、検証付き展開、インストール/更新、削除、リカバリ、ガーベジコレクション、パッケージ ID と
静的依存関係を持つ Win32/WinUI 3 アプリ起動です。

このスコープは Windows 全体より狭く、Microsoft Store クライアントや UWP/AppContainer、
未署名パッケージの検証回避、Windows App SDK API の完全互換を意味しません。詳細なコマンド、耐久性制約、
既知制限は [MSIX ガイド](../msix.md) に記載されています。

## 動作イメージ

<p align="center">
  <img src="../assets/switchyard-container-library.png" alt="Switchyard container library showing Windows games and launchers on macOS" width="100%">
</p>

<p align="center">
  <img src="../assets/switchyard-session-workspace.png" alt="Grand Theft Auto V Enhanced and Rockstar Games Launcher running in Switchyard" width="100%">
  <br>
  <sub>1 つの管理 Wine セッションで GTA V Enhanced と Rockstar Games Launcher を同時表示</sub>
</p>

## runtime の取得・ビルド

Switchyard 利用者は通常、アプリ側でコンテナとランタイム選択を管理します。
署名・公証済みの Wine-only アーカイブは
[GitHub Releases](https://github.com/jungwuk-ryu/switchyard-wine/releases)
から取得できます。Apple Game Porting Toolkit（GPTK）はユーザー提供であり、この
リポジトリや配布物に含まれません。

Apple Silicon macOS でソースからビルドするには:

```shell
./switchyard/verify_source.sh
./switchyard/build_runtime.sh
```

ランタイムの公開・差し替え前に [ビルドガイド](../building.md) を確認してください。
必要なツールチェーン、検証済み依存関係、ステージング、署名／公証、回帰チェックを
含みます。

## ドキュメント

- [ドキュメント一覧](../README.md)
- [構成とリポジトリ境界](../architecture.md)
- [runtime のビルドとリリース](../building.md)
- [MSIX とパッケージデスクトップ対応](../msix.md)
- [アプリ互換性記録](../compatibility.md)
- [ソースと依存関係の provenance](../provenance.md)
- [Unity ゲーム向けトラブルシュート](../troubleshooting-unity-games.md)

一般的な Wine 利用は [WineHQ ドキュメント](https://gitlab.winehq.org/wine/wine/-/wikis/home)
および [Upstream ソース](https://gitlab.winehq.org/wine/wine) を参照してください。

## コミュニティ・ライセンス

ランタイム検証や互換性報告、開発議論は [Switchyard Discord](https://discord.gg/USNfzUza7B)
で行えます。

Wine と Switchyard Wine の変更は LGPL-2.1-or-later です。`LICENSE` と
`COPYING.LIB` を参照してください。

Switchyard Wine は独立したプロジェクトであり、WineHQ、Apple、Microsoft、
表示されている製品による公式後援を受けません。
