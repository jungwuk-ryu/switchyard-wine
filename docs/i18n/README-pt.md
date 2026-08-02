<p align="center">
  <img src="../assets/switchyard-wine-logo.png" alt="Switchyard Wine logo" width="420">
</p>

# Switchyard Wine

[English](../../README.md) · [Outras línguas](../README.md#translations)

Switchyard Wine é a runtime Wine por trás do
[Switchyard](https://github.com/jungwuk-ryu/Switchyard), uma aplicação macOS para
manter jogos e launchers Windows em contentores geridos. A aplicação gere a
interface, o estado do contentor e a seleção de runtime; este repositório trata do
código Wine, da cadeia de compilação e do trabalho de compatibilidade subjacente.

O projeto faz uma decisão deliberada: fica numa base Wine conhecida até que os fluxos
de trabalho validados do Switchyard sejam verificados, em vez de atualizar apenas por
ter uma versão Wine mais nova. As alterações a jusante permanecem como commits Git
revisáveis, aplicados sobre uma revisão fixa do WineHQ.

## Por que existe este branch

- **Trabalho de macOS na runtime.** O branch traz correções para caminhos de falha
  reais observados no Switchyard, incluindo callbacks e ponte de recursos D3DMetal,
  sincronização macOS MSync, renderização Chromium/CEF, seleção de fornecedor gráfico,
  reprodução de mídia e fallback tipográfico multilíngue.
- **Runtime com identidade verificável.** Builds fixam e fazem hash de entradas externas,
  são montadas fora da runtime ativa e publicadas apenas após verificação. Cada runtime
  regista revisão de origem, digests de dependências, arquitetura e hashes dos binários
  principais.
- **Testes de regressão com as correções.** O repositório cobre D3DMetal, callbacks
  nativos, MSync, hotpatching do overlay Steam, TLS, média, OpenGL e caminhos de segurança
  da runtime, em vez de depender apenas de uma compilação com sucesso.
- **Registos de compatibilidade contextualizados.** Os resultados indicam runtime exacta,
  host macOS, caminho gráfico, data e limitação conhecida. Um resultado numa configuração
  não é apresentado como promessa de suporte universal.

## MSIX e aplicações de desktop empacotadas

Switchyard Wine inclui `wineappx` e `appxsvc.dll` para pacotes MSIX/AppX full-trust
assinados e não encriptados de desktop. O ciclo implementado cobre inspeção, extração
verificada, instalação e atualização por prefixo, remoção, recuperação, garbage
collection e arranque de apps Win32 ou WinUI 3 declaradas com identidade de pacote e
dependências estáticas.

Este alcance é intencionalmente mais estreito do que o Windows. Não é um cliente
Microsoft Store, nem compatibilidade UWP/AppContainer, nem permite contornar a
assinatura de pacotes não assinados, nem garantia de que todas as APIs do Windows
App SDK funcionem. Os
requisitos exatos do pacote, comandos, modelo de durabilidade e limites atuais estão
no [guia MSIX](../msix.md).

## Ver em funcionamento

<p align="center">
  <img src="../assets/switchyard-container-library.png" alt="Biblioteca de contentores do Switchyard com jogos e launchers Windows no macOS" width="100%">
</p>

<p align="center">
  <img src="../assets/switchyard-session-workspace.png" alt="Grand Theft Auto V Enhanced e Rockstar Games Launcher a correr no Switchyard" width="100%">
  <br>
  <sub>Grand Theft Auto V Enhanced e Rockstar Games Launcher numa sessão Wine gerida.</sub>
</p>

## Obter ou construir a runtime

Normalmente, utilizadores do Switchyard deixam que a app gere contentores e selecção de
runtime. Arquivos Wine-only assinados e notarizados estão disponíveis em
[GitHub Releases](https://github.com/jungwuk-ryu/switchyard-wine/releases).
O Apple Game Porting Toolkit é software fornecido pelo utilizador e nunca está incluído
nem no repositório nem nas releases.

Para compilar a partir da fonte no Apple Silicon macOS:

```shell
./switchyard/verify_source.sh
./switchyard/build_runtime.sh
```

Leia o [guia de compilação](../building.md) antes de publicar ou substituir uma runtime.
Inclui toolchain necessária, dependências verificadas, staging, assinatura, notarização e
checks de regressão disponíveis.

## Documentação

- [Índice de documentação](../README.md)
- [Arquitetura e limites do repositório](../architecture.md)
- [Compilar e lançar a runtime](../building.md)
- [Suporte MSIX e desktop empacotado](../msix.md)
- [Compatibilidade de aplicações registada](../compatibility.md)
- [Proveniência do código e dependências](../provenance.md)
- [Resolução de problemas Unity](../troubleshooting-unity-games.md)

Para uso geral do Wine e desenvolvimento, use
[documentação WineHQ](https://gitlab.winehq.org/wine/wine/-/wikis/home) e
[fonte upstream](https://gitlab.winehq.org/wine/wine). Este README descreve
Switchyard Wine e não duplica o manual Wine upstream.

## Comunidade e licença

Junte-se ao
[Switchyard Discord](https://discord.gg/USNfzUza7B) para testes de runtime,
relatórios de compatibilidade e discussão de desenvolvimento.

As mudanças em Wine e Switchyard Wine são licenciadas sob LGPL-2.1-or-later; veja
`LICENSE` e `COPYING.LIB`. Switchyard Wine é independente e não é apoiado por
WineHQ, Apple, Microsoft ou pelos produtos mostrados acima.
