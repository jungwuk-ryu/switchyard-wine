<p align="center">
  <img src="../assets/switchyard-wine-logo.png" alt="Switchyard Wine logo" width="420">
</p>

# Switchyard Wine

[English](../../README.md) · [Outros idiomas](../README.md#translations)

Switchyard Wine é a runtime Wine por trás do
[Switchyard](https://github.com/jungwuk-ryu/Switchyard), um app de macOS para manter
jogos e launchers Windows em containers gerenciados. O app cuida da interface, do estado
do container e da seleção da runtime; este repositório contém o código Wine, o pipeline de
build e o trabalho de compatibilidade subjacente.

O projeto faz uma escolha deliberada: permanece em uma base Wine conhecida até que os
cargas de trabalho validadas do Switchyard tenham sido verificadas, em vez de atualizar
apenas porque uma versão Wine mais nova existe. Alterações downstream continuam como commits
Git revisáveis e regulares em cima de uma revisão fixa do WineHQ.

## Por que este branch existe

- **Trabalho de macOS na runtime.** O branch carrega correções para fluxos de falha reais
  observados no Switchyard, incluindo callbacks e ponte de recursos D3DMetal, sincronização
  macOS MSync, renderização Chromium/CEF, seleção de provedor gráfico, reprodução de mídia
  e fallback de fontes multilíngue.
- **Runtime com identidade verificável.** Builds fixam e geram hash de entradas externas,
  montadas fora da runtime ativa e só são publicadas após verificação. Cada runtime registra
  revisão de origem, hashes de dependências, arquitetura e hashes de binários principais.
- **Testes de regressão junto com os fixes.** O repositório testa D3DMetal, callbacks nativos,
  MSync, hotpatching do overlay Steam, TLS, mídia, OpenGL e caminhos de segurança da runtime
  em vez de confiar apenas em build bem-sucedido.
- **Registros de compatibilidade com contexto.** Os resultados nomeiam a runtime exata, host
  macOS, caminho gráfico, data e limitação conhecida. Um resultado em uma configuração não é
  apresentado como suporte universal.

## MSIX e apps desktop empacotadas

Switchyard Wine inclui `wineappx` e `appxsvc.dll` para pacotes MSIX/AppX full-trust de desktop
assinados e não criptografados. O ciclo implementado inclui inspeção, extração verificada,
instalação e atualização por prefixo, remoção, recuperação, coleta de lixo e execução de apps
Win32 ou WinUI 3 declaradas com identidade de pacote e dependências estáticas.

Esse escopo é intencionalmente mais restrito que o Windows. Não é um cliente do Microsoft
Store, não é suporte UWP/AppContainer, não permite contornar a validação de pacote sem
assinatura e não promete
que toda API do Windows App SDK funcione. Requisitos exatos de pacote, comandos, modelo de
durabilidade e limitações atuais estão no
[guia MSIX](../msix.md).

## Ver funcionando

<p align="center">
  <img src="../assets/switchyard-container-library.png" alt="Biblioteca de containers do Switchyard mostrando jogos e launchers Windows no macOS" width="100%">
</p>

<p align="center">
  <img src="../assets/switchyard-session-workspace.png" alt="Grand Theft Auto V Enhanced e Rockstar Games Launcher rodando no Switchyard" width="100%">
  <br>
  <sub>Grand Theft Auto V Enhanced e Rockstar Games Launcher em uma sessão Wine gerenciada.</sub>
</p>

## Obter ou compilar a runtime

Usuários do Switchyard normalmente deixam o app gerenciar containers e seleção da runtime.
Arquivos Wine-only assinados e notarizados estão disponíveis nos
[GitHub Releases](https://github.com/jungwuk-ryu/switchyard-wine/releases).
Apple Game Porting Toolkit é software fornecido pelo usuário e nunca está incluído neste
repositório ou releases.

Para compilar do código-fonte no macOS Apple Silicon:

```shell
./switchyard/verify_source.sh
./switchyard/build_runtime.sh
```

Leia o [guia de build](../building.md) antes de publicar ou substituir uma runtime.
Ela cobre toolchain necessária, dependências verificadas, staging, assinatura, notarização e
checks de regressão disponíveis.

## Documentação

- [Índice de documentação](../README.md)
- [Arquitetura e fronteiras do repositório](../architecture.md)
- [Construção e publicação da runtime](../building.md)
- [Suporte MSIX e desktop empacotado](../msix.md)
- [Compatibilidade de aplicações registrada](../compatibility.md)
- [Proveniência do código e dependências](../provenance.md)
- [Solução de problemas do Unity](../troubleshooting-unity-games.md)

Para uso geral de Wine e desenvolvimento, use
[documentação WineHQ](https://gitlab.winehq.org/wine/wine/-/wikis/home) e
[código-fonte upstream](https://gitlab.winehq.org/wine/wine). Este README descreve
o Switchyard Wine e não duplica o manual do Wine upstream.

## Comunidade e licença

Participe do
[Switchyard Discord](https://discord.gg/USNfzUza7B) para testes de runtime,
relatórios de compatibilidade e discussão de desenvolvimento.

As mudanças em Wine e Switchyard Wine são licenciadas sob LGPL-2.1-or-later; veja
`LICENSE` e `COPYING.LIB`. Switchyard Wine é independente e não é endossado por
WineHQ, Apple, Microsoft ou pelos produtos mostrados acima.
